#include "https_client.h"

#include "board.h"

#include <WiFiClientSecure.h>
#include <esp_crt_bundle.h>
#include <lwip/sockets.h>
#include <mbedtls/platform.h>
#undef connect

static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();


// Lastro de heap: no boot o heap esta inteiro, depois fragmenta (Wi-Fi, consultas) e a TLS passa a
// falhar. A TLS aloca DOIS buffers (entrada e saida) de ~16,7 KB cada (5 + 16384 + sobrecarga de
// registro, medido) que precisam de blocos separados >= isso; o heap do ESP32 e dividido em regioes e um
// bloco de 32 KB livre nunca comporta os dois. Entao o lastro e de DOIS blocos fixos de RESERVE_BLOCK,
// emprestados direto ao mbedtls_calloc (sem passar pelo heap geral, evitando que o TLSF parta um rabo
// livre de 668 B e que outra task roube ou fragmente os buracos durante o handshake).
// Os ponteiros so mudam com resMutex (curto): https_send roda no nucleo 0 e https_reserve_check no loop (nucleo 1).
// Igual ao buffer do mbedTLS 2.28: 16384 + IV 16 + MAC 48 + padding 256 + cabecalho 13 = 16717, arredondado
// para 16720. O TLSF do IDF arredonda o pedido PARA CIMA para classes de 1 KB nessa faixa: um malloc de 16717
// so acha blocos da classe de 17408, e um buraco de 16720 fica na classe abaixo e nao serve. 17396 (17408 - 12 de
// cabecalho) e o maior alocavel num bloco dessa classe: e o que heap_caps_get_largest_free_block reporta e cabe de volta.
static constexpr size_t RESERVE_BLOCK = 17396;
static constexpr int RESERVE_N = 2;
// Pula a TLS quando falta bloco na reserva e nem 16 KB contiguos existem no heap geral.
static constexpr uint32_t TLS_MIN_BLOCK = 16 * 1024;
static constexpr uint32_t TLS_MIN_BLOCK_WITH_RESERVE = 4 * 1024;
// Piso de heap livre ao iniciar a TLS = max(base, min(pico_aprendido(host) + folga, teto)).
// Com a reserva de 2 blocos entregue direto ao mbedtls_calloc, os ~34,8 KB dos buffers de I/O ja
// estao garantidos fora da disputa do heap geral. A folga precisa cobrir apenas pacotes TCP/Wi-Fi
// (~8 KB) e o pico aprendido nao deve ultrapassar o teto maximo seguro (51 KB) para nao travar
// consultas quando o heap em repouso estiver em ~54-55 KB.
static constexpr uint32_t TLS_MIN_FREE = 48 * 1024;
static constexpr uint32_t TLS_MIN_FREE_RESUME = 44 * 1024;
static constexpr uint32_t TLS_FREE_MARGIN = 8 * 1024;
static constexpr uint32_t TLS_MAX_FLOOR = 51 * 1024;
static constexpr uint32_t REALLOC_MIN_FREE = 24 * 1024;  // so reserva se sobrar isso depois
static constexpr uint32_t VALVE_MIN_FREE = 16 * 1024;    // abaixo disso a valvula solta o lastro
// Requisicao em curso (https_send com a trava): instante (millis|1, 0 = nenhuma) e host, lidos por
// https_reserve_check (outra task) para detectar uma chamada de rede presa. Sem String.
static constexpr uint32_t STUCK_MS = 120 * 1000;  // timeouts sao 10/15 s; uma requisicao nao passa de ~40 s
static volatile uint32_t reqSince = 0;
static char reqHost[40] = "";
static SemaphoreHandle_t resMutex = xSemaphoreCreateMutex();
static void *reserve[RESERVE_N] = {};
static bool reserveUsed[RESERVE_N] = {};
static int lastState = -1;  // blocos em maos no ultimo log: so loga quando muda

static int reserveCount() {
  int n = 0;
  for (void *r : reserve) n += r != nullptr;
  return n;
}

static int reserveAvail() {
  int n = 0;
  for (int i = 0; i < RESERVE_N; i++) n += (reserve[i] != nullptr && !reserveUsed[i]);
  return n;
}

void heap_mark(const char *step) {
  static int32_t last = -1;  // so a ordem do boot importa; entre tasks a diferenca e aproximada
  int32_t freeNow = (int32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
  Serial.printf("[audit] %-28s livre=%d maior_bloco=%u delta=%d\n", step, (int)freeNow,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), last < 0 ? 0 : (int)(freeNow - last));
  last = freeNow;
}

static void noteState(const char *what) {
  int n = reserveCount();
  if (n == lastState) return;
  lastState = n;
  // "bloco" e o maior bloco livre naquele instante: apos liberar e o que a TLS vai ter.
  Serial.printf("[mem] reserva TLS: %s, %d/%d blocos (heap %u, bloco %u)\n", what, n, RESERVE_N, ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// Chamadores seguram resMutex. So libera blocos que nao estao em uso por uma sessao mbedTLS ativa.
static void reserveFree() {
  bool any = false;
  for (int i = 0; i < RESERVE_N; i++) {
    if (reserve[i] && !reserveUsed[i]) {
      free(reserve[i]);
      reserve[i] = nullptr;
      any = true;
    }
  }
  if (any) noteState(reserveCount() ? "alocada" : "liberada");
}
// Aloca o que falta, um bloco por vez; um so e melhor que nenhum.
static void reserveTry() {
  for (int i = 0; i < RESERVE_N; i++) {
    if (reserve[i]) continue;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < RESERVE_BLOCK || ESP.getFreeHeap() < REALLOC_MIN_FREE) break;
    reserve[i] = heap_caps_malloc(RESERVE_BLOCK, MALLOC_CAP_8BIT);
    if (reserve[i] && ESP.getFreeHeap() < REALLOC_MIN_FREE) {
      free(reserve[i]);
      reserve[i] = nullptr;
    }
    if (!reserve[i]) break;
    reserveUsed[i] = false;
  }
  noteState(reserveCount() ? "alocada" : "sem espaco");
}

// Alocador do mbedTLS: entrega os dois buffers de ~16,7 KB (in_buf e out_buf do mbedtls_ssl_setup)
// direto de reserve[], sem devolve-los ao heap geral entre conexoes.
static void *tlsCalloc(size_t n, size_t size) {
  size_t total = n * size;
  if (total >= 16000 && total <= RESERVE_BLOCK) {
    xSemaphoreTake(resMutex, portMAX_DELAY);
    for (int i = 0; i < RESERVE_N; i++) {
      if (reserve[i] && !reserveUsed[i]) {
        reserveUsed[i] = true;
        void *p = reserve[i];
        xSemaphoreGive(resMutex);
        memset(p, 0, total);
        return p;
      }
    }
    xSemaphoreGive(resMutex);
    void *p = heap_caps_malloc(RESERVE_BLOCK, MALLOC_CAP_8BIT);
    if (!p) return heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
    memset(p, 0, total);
    xSemaphoreTake(resMutex, portMAX_DELAY);
    for (int i = 0; i < RESERVE_N; i++) {
      if (!reserve[i]) {
        reserve[i] = p;
        reserveUsed[i] = true;
        noteState("alocada");
        break;
      }
    }
    xSemaphoreGive(resMutex);
    return p;
  }
  return heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
}

static void tlsFree(void *ptr) {
  if (!ptr) return;
  xSemaphoreTake(resMutex, portMAX_DELAY);
  for (int i = 0; i < RESERVE_N; i++) {
    if (ptr == reserve[i]) {
      reserveUsed[i] = false;
      if (ESP.getFreeHeap() < VALVE_MIN_FREE) {
        free(reserve[i]);
        reserve[i] = nullptr;
        noteState(reserveCount() ? "alocada" : "liberada");
      }
      xSemaphoreGive(resMutex);
      return;
    }
  }
  xSemaphoreGive(resMutex);
  heap_caps_free(ptr);
}

void https_reserve_init() {
  mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
  xSemaphoreTake(resMutex, portMAX_DELAY);
  reserveTry();
  xSemaphoreGive(resMutex);
}

// Quem segura a trava (https_send, diagnostico, ...) tem acesso exclusivo aos dois blocos da reserva TLS.
void https_lock() {
  xSemaphoreTake(mutex, portMAX_DELAY);
  xSemaphoreTake(resMutex, portMAX_DELAY);
  if (reserveCount() < RESERVE_N) reserveTry();
  xSemaphoreGive(resMutex);
}
static void lockKeepReserve(const char *host) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  strlcpy(reqHost, host, sizeof(reqHost));
  uint32_t now = millis();
  reqSince = now ? now : 1;
}
static uint32_t ballastPrepare() {
  xSemaphoreTake(resMutex, portMAX_DELAY);
  if (reserveCount() < RESERVE_N) reserveTry();
  uint32_t avail = (uint32_t)reserveAvail() * RESERVE_BLOCK;
  xSemaphoreGive(resMutex);
  return avail;
}
void https_unlock() {
  xSemaphoreTake(resMutex, portMAX_DELAY);
  if (reserveCount() < RESERVE_N) reserveTry();
  xSemaphoreGive(resMutex);
  xSemaphoreGive(mutex);
}

int https_reserve_held() { return reserveCount(); }

// Loop, a cada ~1 s, sem nunca bloquear (timeout 0). Sem lastro: tenta refaze-lo quando nao ha TLS em
// curso (a trava `mutex` esta livre). Com lastro: a valvula solta os dois blocos se o heap livre ficar baixo.
void https_reserve_check() {
  uint32_t r0 = reqSince;
  int32_t dt = (int32_t)(millis() - r0);
  if (r0 && dt > (int32_t)STUCK_MS) {  // task presa dentro de uma chamada de rede segurando a trava
    board_log("[https] requisicao a %s presa ha %u s: reiniciando", reqHost, (unsigned)(dt / 1000));
    board_restart();
  }
  int avail = reserveAvail();
  if (avail && ESP.getFreeHeap() < VALVE_MIN_FREE) {
    if (xSemaphoreTake(resMutex, 0) != pdTRUE) return;  // https_send mexendo: tenta no proximo segundo
    if (reserveAvail() && ESP.getFreeHeap() < VALVE_MIN_FREE) reserveFree();
    xSemaphoreGive(resMutex);
    return;
  }
  if (reserveCount() >= RESERVE_N) return;
  if (xSemaphoreTake(mutex, 0) != pdTRUE) return;  // TLS em curso: o unlock dela ja tenta refazer
  if (xSemaphoreTake(resMutex, 0) == pdTRUE) {
    reserveTry();  // so loga se mudar de estado
    xSemaphoreGive(resMutex);
  }
  xSemaphoreGive(mutex);
}

// Com os 2 blocos da reserva em maos, os dois buffers de 16,7 KB do mbedTLS ja estao garantidos e o heap
// geral so precisa de blocos menores (~4 KB) para certificados/handshake. Sem a reserva completa, exige 16 KB.
static bool contiguousOk() {
  uint32_t need = reserveAvail() >= RESERVE_N ? TLS_MIN_BLOCK_WITH_RESERVE : TLS_MIN_BLOCK;
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= need) return true;
  delay(200);
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= need;
}

// Vigia de rede (lido pelo wifi_loop): instante da ultima TLS ok (qualquer host) e falhas NAO de memoria
// desde entao (falha de memoria ja tem o reinicio proprio, ver noteTls). 0 = nenhum sucesso ainda.
static volatile uint32_t lastOkAt = 0;
static volatile uint32_t nonMemFails = 0;
static volatile uint32_t lowFree = 0;   // com noContig: livre 8-bit medido se foi o piso (0 = foi a contiguidade)
static volatile bool noContig = false;  // start() achou o heap sem bloco contiguo depois de soltar o lastro
uint32_t https_last_ok_ms() { return lastOkAt; }
uint32_t https_nonmem_fails() { return nonMemFails; }
void https_net_fails_reset() { nonMemFails = 0; }

// Fragmentacao para o log [heap] loop: so leitura das estatisticas do heap (sem malloc nem varredura).
void https_heap_frag(unsigned *freeBlocks, unsigned *largest) {
  multi_heap_info_t info;
  heap_caps_get_info(&info, MALLOC_CAP_8BIT);
  *freeBlocks = (unsigned)info.free_blocks;
  *largest = (unsigned)info.largest_free_block;
}

// Retomada de sessao TLS 1.2 por session ticket, SO para api.spotify.com (o servidor aceita ticket e nao
// session ID). A sessao fica numa estatica (~1 KB + ticket) e so e lida/gravada com a trava https_lock.
struct SessSlot {
  const char *host;
  mbedtls_ssl_session sess;  // zerada = mbedtls_ssl_session_init
  uint8_t ticket[384];       // evita manter o ticket (alocado durante o handshake) nos buracos da reserva
  bool valid;
  uint32_t ip;  // IP local de quando foi guardada: mudou, o ticket nao serve mais
};
static SessSlot slots[] = {{"api.spotify.com"}, {"i.scdn.co"}};
static SessSlot *slotFor(const char *host) {
  for (auto &sl : slots)
    if (strcmp(host, sl.host) == 0) return &sl;
  return nullptr;
}
static void sessDrop(SessSlot *sl) {
  if (sl->sess.ticket == sl->ticket) sl->sess.ticket = nullptr;
  mbedtls_ssl_session_free(&sl->sess);
  mbedtls_ssl_session_init(&sl->sess);
  sl->valid = false;
}
struct Peak {
  char host[40];
  uint32_t used;
};
static Peak peaks[8];
static Peak *peakFor(const char *host, bool create) {
  for (auto &p : peaks)
    if (p.host[0] && strcmp(p.host, host) == 0) return &p;
  if (create)
    for (auto &p : peaks)
      if (!p.host[0]) {
        strlcpy(p.host, host, sizeof(p.host));
        return &p;
      }
  return nullptr;
}
static uint32_t peakOf(const char *host) {
  Peak *p = peakFor(host, false);
  return p ? p->used : 0;
}
// Falta de memoria nao invalida o ticket (X509 e outras falhas sim).
static bool isMemFail(const String &err) {
  return err.indexOf("Memory allocation") >= 0 || err.indexOf("ALLOC") >= 0;
}

// WiFiClientSecure com um connect proprio (sslclient e protegido). start_ssl_client abaixo e uma copia enxuta
// de ssl_client.cpp do core Arduino-ESP32 2.0.17: so CA bundle, sem PSK/cert de cliente/insecure/ALPN, mais
// mbedtls_ssl_set_session antes do handshake. Usada para TODOS os hosts (sl == nullptr: sem retomada).
// Em erro o chamador faz stop() (fecha o socket e libera o contexto).
// Ordem: DNS, socket e TCP connect com o lastro ainda alocado (o pcb nao cai nos buracos dele); so entao
// solta o lastro, logo antes das alocacoes do mbedTLS. Conectar antes de soltar nao custa nada: se o TCP
// falha, a TLS nem comecaria.
class ResumeClient : public WiFiClientSecure {
 public:
  uint32_t dns_ms = 0, hs_ms = 0, freeRel = 0;  // freeRel: livre 8-bit logo apos soltar o lastro
  bool resumed = false;
  void stopNow() {
    if (sslclient && sslclient->socket >= 0) {
      struct linger lg = {1, 0};
      lwip_setsockopt(sslclient->socket, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    }
    stop();
  }
  int openResume(const char *host, SessSlot *sl) {  // sl pode ser nullptr
    uint32_t t0 = millis();
    IPAddress ip;
    if (!WiFi.hostByName(host, ip)) return 0;
    dns_ms = millis() - t0;
    t0 = millis();
    int ret = start(ip, 443, host, sl);
    hs_ms = millis() - t0;
    _lastError = ret;
    if (ret < 0) {
      stopNow();
      return 0;
    }
    _connected = true;
    return 1;
  }

 private:
  int start(const IPAddress &ip, uint32_t port, const char *host, SessSlot *sl) {
    sslclient_context *sc = sslclient;
    int ret, enable = 1;
    int timeout = _timeout > 0 ? _timeout : 30000;
    sc->socket = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sc->socket < 0) return sc->socket;
    fcntl(sc->socket, F_SETFL, fcntl(sc->socket, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ip;
    sa.sin_port = htons(port);
    sc->socket_timeout = timeout;
    fd_set fdset;
    struct timeval tv;
    FD_ZERO(&fdset);
    FD_SET(sc->socket, &fdset);
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
    if (lwip_connect(sc->socket, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS) return -1;
    if (select(sc->socket + 1, nullptr, &fdset, nullptr, &tv) <= 0) return -1;
    int sockerr;
    socklen_t len = sizeof(int);
    if (getsockopt(sc->socket, SOL_SOCKET, SO_ERROR, &sockerr, &len) < 0 || sockerr != 0) return -1;
    if (lwip_setsockopt(sc->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ||
        lwip_setsockopt(sc->socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0 ||
        lwip_setsockopt(sc->socket, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)) < 0 ||
        lwip_setsockopt(sc->socket, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable)) < 0)
      return -1;

    uint32_t availRes = ballastPrepare();  // socket e TCP prontos: garante os 2 blocos para o tlsCalloc
    freeRel = heap_caps_get_free_size(MALLOC_CAP_8BIT) + availRes;
    uint32_t floorB = sl && sl->valid ? TLS_MIN_FREE_RESUME : TLS_MIN_FREE;
    uint32_t peakFloor = peakOf(host) ? peakOf(host) + TLS_FREE_MARGIN : 0;
    floorB = min(max(floorB, peakFloor), TLS_MAX_FLOOR);
    if (freeRel < floorB) {  // mesmo efeito de "sem memoria contigua"
      lowFree = freeRel;
      noContig = true;
      return -1;
    }
    if (!contiguousOk()) {
      noContig = true;
      return -1;
    }

    mbedtls_entropy_init(&sc->entropy_ctx);
    ret = mbedtls_ctr_drbg_seed(&sc->drbg_ctx, mbedtls_entropy_func, &sc->entropy_ctx, (const unsigned char *)"esp32-tls", 9);
    if (ret < 0) return ret;
    ret = mbedtls_ssl_config_defaults(&sc->ssl_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) return ret;
    ret = arduino_esp_crt_bundle_attach(&sc->ssl_conf);  // o bundle ja foi definido por setCACertBundle
    if (ret < 0) return ret;
    mbedtls_ssl_conf_authmode(&sc->ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    ret = mbedtls_ssl_set_hostname(&sc->ssl_ctx, host);
    if (ret != 0) return ret;
    mbedtls_ssl_conf_rng(&sc->ssl_conf, mbedtls_ctr_drbg_random, &sc->drbg_ctx);
    ret = mbedtls_ssl_setup(&sc->ssl_ctx, &sc->ssl_conf);
    if (ret != 0) return ret;
    mbedtls_ssl_set_bio(&sc->ssl_ctx, &sc->socket, mbedtls_net_send, mbedtls_net_recv, NULL);

    // Sessao so vale com o mesmo IP local; set_session exige o ssl ja configurado (handshake alocado).
    bool offered = false;
    if (sl && sl->valid && sl->ip != (uint32_t)WiFi.localIP()) {
      Serial.printf("[tls] %s: IP local mudou, sessao descartada\n", host);
      sessDrop(sl);
    }
    if (sl && sl->valid) {
      if (mbedtls_ssl_set_session(&sc->ssl_ctx, &sl->sess) == 0) offered = true;
      else sessDrop(sl);
    }

    unsigned long hs0 = millis();
    while ((ret = mbedtls_ssl_handshake(&sc->ssl_ctx)) != 0) {
      if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) return ret;
      if ((millis() - hs0) > sc->handshake_timeout) return -1;
      vTaskDelay(2);
    }
    if (mbedtls_ssl_get_verify_result(&sc->ssl_ctx) != 0) return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;

    // Retomada: o servidor nao manda certificado, entao a sessao negociada fica sem peer_cert.
    bool full = mbedtls_ssl_get_peer_cert(&sc->ssl_ctx) != NULL;
    resumed = offered && !full;
    if (full && sl) {  // so grava depois de handshake completo COM verificacao ok
      sessDrop(sl);
      mbedtls_ssl_session &ss = sl->sess;
      if (mbedtls_ssl_get_session(&sc->ssl_ctx, &ss) == 0 && ss.ticket_len > 0 && ss.ticket_len <= sizeof(sl->ticket)) {
        // A copia traz a cadeia do servidor (~3 KB) e o ticket no heap; copia o ticket para sl->ticket e
        // libera ambos do heap para nao deixar nada alocado dentro dos buracos da reserva TLS.
        if (ss.peer_cert) {
          mbedtls_x509_crt_free(ss.peer_cert);
          mbedtls_free(ss.peer_cert);
          ss.peer_cert = NULL;
        }
        memcpy(sl->ticket, ss.ticket, ss.ticket_len);
        mbedtls_free(ss.ticket);
        ss.ticket = sl->ticket;
        sl->valid = true;
        sl->ip = (uint32_t)WiFi.localIP();
      } else {
        sessDrop(sl);
      }
    }
    return sc->socket;
  }
};

// Uma tentativa completa. Retorna false so se nao houve resposta HTTP nenhuma
// (conexao/TLS falhou, ou o servidor fechou sem responder).
static bool attempt(HttpsRequest &rq, HttpsResult &rs) {
  rs.error.reserve(96);
  for (int i = 0; i < rq.want_count; i++) rq.want[i].value.reserve(48);

  ResumeClient c;
  SessSlot *sl = slotFor(rq.host);
  noContig = false;
  lowFree = 0;
  c.setCACertBundle(ca_bundle_start);
  c.setHandshakeTimeout(15);
  c.setTimeout(10000);

  uint32_t t0 = millis();
  uint32_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  Serial.printf("[tls] %s antes do connect: livre=%u maior_bloco=%u\n", rq.host, (unsigned)freeBefore,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (!c.openResume(rq.host, sl)) {
    char err[64];
    c.lastError(err, sizeof(err));
    rs.error = err;
    if (sl && !noContig && !isMemFail(rs.error)) sessDrop(sl);
    rs.total_ms = millis() - t0;
    return false;
  }
  rs.handshake_ms = c.hs_ms;  // exclui o DNS
  if (sl)
    Serial.printf("[tls] %s: retomou=%d dns=%u ms hs=%u ms sessao=%s\n", rq.host, (int)c.resumed, (unsigned)c.dns_ms,
                  (unsigned)c.hs_ms, sl->valid ? "guardada" : "nenhuma");
  {
    uint32_t freeAfter = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    int used = (int)c.freeRel - (int)freeAfter;
    if (used > 0)
      if (Peak *pk = peakFor(rq.host, true))
        if ((uint32_t)used > pk->used) pk->used = used;
    Serial.printf("[tls] %s apos handshake: livre=%u maior_bloco=%u | apos soltar lastro=%u, a TLS pegou %d B (pico %u B)\n",
                  rq.host, (unsigned)freeAfter, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  (unsigned)c.freeRel, used, (unsigned)peakOf(rq.host));
  }

  // Monta e envia apos o handshake (quando a cadeia X509/RSA temporaria ja foi liberada) e solta `all` na hora.
  size_t allLen = 0, sent = 0;
  {
    String all;
    all.reserve(128 + rq.headers.length() + rq.body.length());
    all = rq.method;
    all += ' ';
    all += rq.path;
    all += rq.http10 ? " HTTP/1.0\r\nHost: " : " HTTP/1.1\r\nHost: ";
    all += rq.host;
    all += "\r\nConnection: close\r\n";
    all += rq.headers;
    if (rq.body.length()) {
      all += "Content-Length: ";
      all += rq.body.length();
      all += "\r\n";
    }
    all += "\r\n";
    all += rq.body;
    allLen = all.length();
    // Cabecalho e corpo num unico envio: dois registros TLS seguidos ja deram resposta vazia.
    sent = c.write((const uint8_t *)all.c_str(), allLen);
  }

  // O servidor pode levar um instante para responder, e available() so enxerga o que
  // ja foi lido do socket: espera ate 8 s pelo primeiro byte.
  uint32_t waitUntil = millis() + 8000;
  while (!c.available() && c.connected() && (int32_t)(millis() - waitUntil) < 0) delay(10);
  bool avail = c.available() > 0, conn = c.connected();

  auto readLine = [&](char *buf, size_t cap) -> int {
    size_t n = c.readBytesUntil('\n', buf, cap - 1);
    while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) n--;
    buf[n] = 0;
    return (int)n;
  };
  char line[256];
  readLine(line, sizeof(line));
  const char *sp = strchr(line, ' ');
  rs.status = sp ? atoi(sp + 1) : 0;
  if (rs.status == 0) {
    char msg[96];
    snprintf(msg, sizeof(msg), "sem resposta (enviou %u/%u, dados=%d, conectado=%d)", (unsigned)sent,
             (unsigned)allLen, (int)avail, (int)conn);
    rs.error = msg;
    c.stopNow();
    if (sl) sessDrop(sl);
    rs.total_ms = millis() - t0;
    return false;
  }

  while (c.connected() || c.available()) {
    if (readLine(line, sizeof(line)) == 0) break;
    char *colon = strchr(line, ':');
    if (!colon || colon == line) continue;
    *colon = 0;
    for (char *p = line; *p; p++) *p = (char)tolower((unsigned char)*p);
    char *val = colon + 1;
    while (*val == ' ' || *val == '\t') val++;
    for (int i = 0; i < rq.want_count; i++) {
      if (strcmp(line, rq.want[i].name) == 0) rq.want[i].value = val;
    }
  }

  if (rq.onBody) {
    rq.onBody(c);
  } else {
    if (rq.max_body) rs.body.reserve(rq.max_body);
    uint32_t deadline = millis() + 3000;
    while (rs.body.length() < rq.max_body && (c.connected() || c.available()) &&
           (int32_t)(millis() - deadline) < 0) {
      int ch = c.read();
      if (ch < 0) {
        delay(5);
        continue;
      }
      rs.body += (char)ch;
    }
  }
  // Deixa o servidor fechar primeiro (FIN dele chega antes do nosso); stopNow() usa SO_LINGER={1,0}
  // para liberar o tcp_pcb na hora caso o servidor nao tenha fechado em 300 ms.
  for (uint32_t end = millis() + 300; c.connected() && (int32_t)(millis() - end) < 0;) {
    uint8_t junk[64];
    if (c.available() <= 0 || c.read(junk, sizeof(junk)) <= 0) delay(10);
  }
  c.stopNow();
  rs.total_ms = millis() - t0;
  return true;
}

static TaskHandle_t interactiveTask = nullptr;
static volatile bool interactivePending = false;  // escrito por uma task, lido por outra: 1 byte, atomico

void https_mark_interactive_task() { interactiveTask = xTaskGetCurrentTaskHandle(); }
static volatile uint32_t interactiveOffAt = 0;  // millis() da ultima vez que interactive caiu
void https_interactive(bool on) {
  if (!on && interactivePending) interactiveOffAt = millis();
  interactivePending = on;
}
static volatile bool memCertFlag = false;  // so a task de uso le e limpa; o Spotify nao escreve
bool https_take_memcert_fail() {
  bool v = memCertFlag;
  memCertFlag = false;
  return v;
}

static bool isMemOrCertFail(const String &err) {
  return err.indexOf("X509") >= 0 || err.indexOf("Memory allocation") >= 0 || err.indexOf("ALLOC") >= 0 ||
         err.indexOf("memoria contigua") >= 0;
}

// Travado so por falta de memoria: sem nenhuma TLS ok (de qualquer host) e so com falhas de memoria por
// MEM_RESTART_MS, o heap nao se recupera sozinho e reiniciar resolve. Teto de reinicios seguidos no RTC
// (sobrevive ao reset, nao ao power-on: o numero magico valida). Chamado com a trava tomada.
static constexpr uint32_t MEM_RESTART_MS = 3 * 60 * 1000;
static constexpr uint32_t MEM_GAP_RESET_MS = 60 * 1000;  // >1 min sem falha de memoria: nao era travamento continuo
static constexpr uint32_t MEM_RESTART_MAX = 3;
static constexpr uint32_t RTC_MAGIC = 0x4D454D52;
static RTC_NOINIT_ATTR uint32_t rtcMagic;
static RTC_NOINIT_ATTR uint32_t rtcMemRestarts;
static uint32_t memOnlyFrom = 0;  // millis da primeira falha de memoria da sequencia; 0 = sem sequencia
static uint32_t memLastAt = 0;    // millis da ultima falha de memoria
static void noteTls(bool ok, bool memFail) {
  if (rtcMagic != RTC_MAGIC) {
    rtcMagic = RTC_MAGIC;
    rtcMemRestarts = 0;
  }
  uint32_t now = millis();
  if (!now) now = 1;
  if (ok) {
    lastOkAt = now;
    nonMemFails = 0;
  } else if (!memFail) {
    nonMemFails++;
  }
  if (!memFail) {  // sucesso ou falha de outro tipo (X509, rede): quebra a sequencia
    memOnlyFrom = 0;
    memLastAt = 0;
    if (ok) rtcMemRestarts = 0;
    return;
  }
  if (!memOnlyFrom || (int32_t)(now - memLastAt) > (int32_t)MEM_GAP_RESET_MS) memOnlyFrom = now;
  memLastAt = now;
  if ((int32_t)(now - memOnlyFrom) < (int32_t)MEM_RESTART_MS) return;
  if (rtcMemRestarts >= MEM_RESTART_MAX) {
    memOnlyFrom = now;  // so loga de novo daqui a 3 min
    board_log("[https] so falhas de memoria ha 3 min; %u reinicios seguidos ja feitos, nao reinicia", (unsigned)rtcMemRestarts);
    return;
  }
  rtcMemRestarts++;
  board_log("[https] so falhas de memoria ha 3 min: reiniciando (%u/%u)", (unsigned)rtcMemRestarts, (unsigned)MEM_RESTART_MAX);
  board_restart();
}

// Saida de https_send: zera o instante da requisicao em curso e devolve a trava (refaz o lastro).
static void unlockSend() {
  reqSince = 0;
  https_unlock();
}
static void noMemError(String &out, unsigned blk) {
  char err[64];
  if (lowFree) snprintf(err, sizeof(err), "pouca memoria livre (%u B)", (unsigned)lowFree);
  else snprintf(err, sizeof(err), "sem memoria contigua (bloco %u B)", blk);
  out = err;
}

bool https_send(HttpsRequest &rq, HttpsResult &rs) {
  // Consulta de uso (nao a task interativa) cede a vez a um comando do Spotify pendente.
  bool yields = xTaskGetCurrentTaskHandle() != interactiveTask;
  // Um comando do Spotify pendente entra antes: espera (teto 20 s) ate interactive cair.
  // Tambem espera 10 s depois que interactive caiu (poll pos-comando, proximo toque), sempre com teto de 20 s.
  auto holdOff = [] { return interactivePending || (interactiveOffAt && millis() - interactiveOffAt < 10000); };
  if (yields && holdOff()) {
    uint32_t w0 = millis();
    while (holdOff() && millis() - w0 < 20000) delay(50);
    Serial.printf("[https] %s%s: esperou %u ms por comando do Spotify\n", rq.host, rq.path, (unsigned)(millis() - w0));
  }
  lockKeepReserve(rq.host);  // o lastro so e solto dentro do start(), depois de criado o socket
  bool ok = attempt(rq, rs);
  if (!ok && noContig) {
    // Sem nem 16 KB contiguos a TLS falharia: falha rapido e deixa o chamador adiar a consulta.
    unsigned blk = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    noMemError(rs.error, blk);
    Serial.printf("[https] %s%s: %s, nao tenta a TLS\n", rq.host, rq.path, rs.error.c_str());
    rs.memOrCertFail = true;
    if (yields) memCertFlag = true;
    noteTls(false, true);
    unlockSend();
    return false;
  }
  if (!ok) {
    // Primeira conexao sem resposta (visto com Anthropic e Google): uma nova tentativa
    // logo em seguida costuma passar. O motivo da primeira vai para o log.
    // X509 FATAL / falta de memoria nao mudam em 2 s: no maximo 1 tentativa extra nesses casos.
    uint32_t firstMs = rs.total_ms;
    bool memCert = isMemOrCertFail(rs.error);
    int maxRetry = memCert ? 1 : 2;
    bool contigBreak = false;
    for (int retry = 0; retry < maxRetry && !ok; retry++) {
      if (yields && interactivePending) {
        Serial.printf("[https] %s%s: cedendo a vez a um comando do Spotify\n", rq.host, rq.path);
        break;
      }
      Serial.printf("[https] %s%s: %s | heap %u, bloco %u | tentando de novo\n", rq.host, rq.path, rs.error.c_str(),
                    ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      // A pausa e SEM a trava: um comando do Spotify entra na frente. O unlock refaz o lastro de heap
      // e o lock seguinte o libera de novo, entao a proxima tentativa ve o mesmo heap de antes.
      unlockSend();  // a pausa e sem a trava (nem conta como requisicao em curso)
      delay(2000);  // a memoria se reorganiza quando a conexao anterior termina de ser liberada
      lockKeepReserve(rq.host);
      if (yields && interactivePending) {
        Serial.printf("[https] %s%s: cedendo a vez a um comando do Spotify\n", rq.host, rq.path);
        break;
      }
      String lastErr = rs.error;
      rs = HttpsResult();
      ok = attempt(rq, rs);
      if (!ok && noContig) {  // a pausa nao devolveu memoria: nao adianta repetir
        Serial.printf("[https] %s%s: %s, desistindo das retentativas\n", rq.host, rq.path,
                      lowFree ? "pouca memoria livre" : "sem memoria contigua");
        rs.error = lastErr;
        memCert = true;
        contigBreak = true;
        break;
      }
      if (!ok && rs.error.isEmpty()) rs.error = lastErr;
      if (!ok) memCert = memCert || isMemOrCertFail(rs.error);
    }
    rs.total_ms += firstMs;
    rs.memOrCertFail = !ok && memCert;
    if (yields && rs.memOrCertFail) memCertFlag = true;
    noteTls(ok, !ok && (contigBreak || isMemFail(rs.error)));
  } else {
    noteTls(true, false);
  }
  unlockSend();
  return ok;
}
