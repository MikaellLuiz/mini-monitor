#include "https_client.h"

#include <WiFiClientSecure.h>
#include <esp_crt_bundle.h>
#include <lwip/sockets.h>
#undef connect

static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();


// Lastro de heap: no boot o heap esta inteiro, depois fragmenta (Wi-Fi, consultas) e a TLS
// passa a falhar. A TLS pede ~34 KB CONTIGUOS (dois buffers de ~16,7 KB + contexto; falha com maior
// bloco 32.756, passa com 34.804). Dois blocos menores nao se juntam (a adjacencia depende de onde o
// alocador os pos), entao o lastro e UM bloco de RESERVE_BYTES, devolvido so durante a conexao.
// O ponteiro so muda com resMutex (curto, nunca segurado durante uma TLS): https_send roda no
// nucleo 0 e https_reserve_check no loop (nucleo 1).
static constexpr size_t RESERVE_BYTES = 34 * 1000;  // ajustavel; o maior bloco ja visto passando foi 34804
// A TLS pega dois buffers de ~16,7 KB que NAO precisam ser adjacentes (conexoes completaram com maior
// bloco 32756). So pula a TLS quando nem um buffer cabe com folga: maior bloco < 16 KB (a TLS do Spotify passou com 21 KB; 20 KB recusava consultas por 12 bytes).
static constexpr uint32_t TLS_MIN_BLOCK = 16 * 1024;
static constexpr uint32_t REALLOC_MIN_FREE = 24 * 1024;  // so reserva se sobrar isso depois
static constexpr uint32_t VALVE_MIN_FREE = 16 * 1024;    // abaixo disso a valvula solta o lastro
static SemaphoreHandle_t resMutex = xSemaphoreCreateMutex();
static void *reserve = nullptr;
static int lastState = -1;  // 1 alocada, 0 liberada/sem espaco: so loga quando muda

void heap_mark(const char *step) {
  static int32_t last = -1;  // so a ordem do boot importa; entre tasks a diferenca e aproximada
  int32_t freeNow = (int32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
  Serial.printf("[audit] %-28s livre=%d maior_bloco=%u delta=%d\n", step, (int)freeNow,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), last < 0 ? 0 : (int)(freeNow - last));
  last = freeNow;
}

static void noteState(const char *what, void *ptr) {
  int n = reserve != nullptr;
  if (n == lastState) return;
  lastState = n;
  // "bloco" e o maior bloco livre naquele instante: em "liberada" e o que a TLS vai ter. ptr = o
  // endereco do bloco de lastro (alocado ou o que acabou de ser liberado).
  Serial.printf("[mem] reserva TLS: %s ptr=%p (heap %u, bloco %u)\n", what, ptr, ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// Chamadores seguram resMutex.
static void reserveFree() {
  if (!reserve) return;
  void *p = reserve;
  free(reserve);
  reserve = nullptr;
  noteState("liberada", p);
}
static void reserveTry() {
  if (reserve) return;
  // O lastro segura o maior buraco enquanto nao ha TLS, para pequenas alocacoes nao o fatiarem. Depois de
  // uma TLS o buraco que sobra (~32,7 KB) e menor que RESERVE_BYTES + 2 KB: exigir o tamanho cheio
  // deixava o buraco sem lastro e ele se fragmentava ate < TLS_MIN_BLOCK (Claude e Spotify
  // "sem memoria contigua"). Entao o lastro encolhe para caber no que existe; ao ser liberado, a TLS
  // recebe o buraco inteiro de novo.
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (largest < TLS_MIN_BLOCK + 2048 || ESP.getFreeHeap() < REALLOC_MIN_FREE) {
    noteState("sem espaco", nullptr);
    return;
  }
  size_t want = largest - 2048 < RESERVE_BYTES ? largest - 2048 : RESERVE_BYTES;
  reserve = heap_caps_malloc(want, MALLOC_CAP_8BIT);
  if (reserve && ESP.getFreeHeap() < REALLOC_MIN_FREE) {
    free(reserve);
    reserve = nullptr;
  }
  if (reserve) noteState("alocada", reserve);
  else noteState("sem espaco", nullptr);
}

void https_reserve_init() {
  xSemaphoreTake(resMutex, portMAX_DELAY);
  reserveTry();
  xSemaphoreGive(resMutex);
}

// Quem segura a trava (https_send, diagnostico, ...) ganha o bloco contiguo: libera ao pegar,
// refaz o lastro antes de soltar.
void https_lock() {
  xSemaphoreTake(mutex, portMAX_DELAY);
  xSemaphoreTake(resMutex, portMAX_DELAY);
  reserveFree();
  xSemaphoreGive(resMutex);
}
void https_unlock() {
  xSemaphoreTake(resMutex, portMAX_DELAY);
  reserveTry();
  xSemaphoreGive(resMutex);
  xSemaphoreGive(mutex);
}

int https_reserve_held() { return reserve != nullptr ? 1 : 0; }

// Loop, a cada ~1 s, sem nunca bloquear (timeout 0). Sem lastro: tenta refaze-lo quando nao ha TLS em
// curso (a trava `mutex` esta livre). Com lastro: a valvula o solta se o heap livre ficar baixo.
void https_reserve_check() {
  if (reserve) {  // leitura simples; a decisao real e refeita com o mutex
    if (xSemaphoreTake(resMutex, 0) != pdTRUE) return;  // https_send mexendo: tenta no proximo segundo
    if (reserve && ESP.getFreeHeap() < VALVE_MIN_FREE) reserveFree();
    xSemaphoreGive(resMutex);
    return;
  }
  if (xSemaphoreTake(mutex, 0) != pdTRUE) return;  // TLS em curso: o unlock dela ja tenta refazer
  if (xSemaphoreTake(resMutex, 0) == pdTRUE) {
    reserveTry();  // so loga se mudar de estado
    xSemaphoreGive(resMutex);
  }
  xSemaphoreGive(mutex);
}

// Depois do lock a reserva ja foi devolvida: o maior bloco livre e o que a TLS vai ter. Abaixo de
// TLS_MIN_BLOCK (16 KB) espera 200 ms e le de novo (outra task pode liberar algo); se continuar baixo, nao
// vale tentar (falharia em ~100 ms por "Memory allocation failed", tres vezes).
static bool contiguousOk() {
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= TLS_MIN_BLOCK) return true;
  delay(200);
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= TLS_MIN_BLOCK;
}

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
  mbedtls_ssl_session_free(&sl->sess);
  mbedtls_ssl_session_init(&sl->sess);
  sl->valid = false;
}
// Falta de memoria nao invalida o ticket (X509 e outras falhas sim).
static bool isMemFail(const String &err) {
  return err.indexOf("Memory allocation") >= 0 || err.indexOf("ALLOC") >= 0;
}

// WiFiClientSecure com um connect proprio (sslclient e protegido). start_ssl_client abaixo e uma copia enxuta
// de ssl_client.cpp do core Arduino-ESP32 2.0.17: so CA bundle, sem PSK/cert de cliente/insecure/ALPN, mais
// mbedtls_ssl_set_session antes do handshake. Em erro o chamador faz stop() (libera o contexto).
class ResumeClient : public WiFiClientSecure {
 public:
  uint32_t dns_ms = 0, hs_ms = 0;
  bool resumed = false;
  int openResume(const char *host, SessSlot *sl) {
    uint32_t t0 = millis();
    IPAddress ip;
    if (!WiFi.hostByName(host, ip)) return 0;
    dns_ms = millis() - t0;
    t0 = millis();
    int ret = start(ip, 443, host, sl);
    hs_ms = millis() - t0;
    _lastError = ret;
    if (ret < 0) {
      stop();
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
    if (sl->valid && sl->ip != (uint32_t)WiFi.localIP()) {
      Serial.printf("[tls] %s: IP local mudou, sessao descartada\n", host);
      sessDrop(sl);
    }
    if (sl->valid) {
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
    if (full) {  // so grava depois de handshake completo COM verificacao ok
      sessDrop(sl);
      mbedtls_ssl_session &ss = sl->sess;
      if (mbedtls_ssl_get_session(&sc->ssl_ctx, &ss) == 0 && ss.ticket_len > 0) {
        // A copia traz a cadeia do servidor (~3 KB); a retomada nao a usa: solta como mbedtls_ssl_session_free faria.
        if (ss.peer_cert) {
          mbedtls_x509_crt_free(ss.peer_cert);
          mbedtls_free(ss.peer_cert);
          ss.peer_cert = NULL;
        }
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
  ResumeClient c;
  SessSlot *sl = slotFor(rq.host);
  const bool resume = sl != nullptr;
  c.setCACertBundle(ca_bundle_start);
  c.setHandshakeTimeout(15);
  c.setTimeout(10000);

  uint32_t t0 = millis();
  uint32_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  Serial.printf("[tls] %s antes do connect: livre=%u maior_bloco=%u\n", rq.host, (unsigned)freeBefore,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (!(resume ? c.openResume(rq.host, sl) : c.connect(rq.host, 443))) {
    char err[64];
    c.lastError(err, sizeof(err));
    rs.error = err;
    if (resume && !isMemFail(rs.error)) sessDrop(sl);
    rs.total_ms = millis() - t0;
    return false;
  }
  rs.handshake_ms = resume ? c.hs_ms : millis() - t0;  // no Spotify exclui o DNS
  if (resume)
    Serial.printf("[tls] %s: retomou=%d dns=%u ms hs=%u ms sessao=%s\n", rq.host, (int)c.resumed, (unsigned)c.dns_ms,
                  (unsigned)c.hs_ms, sl->valid ? "guardada" : "nenhuma");
  {
    uint32_t freeAfter = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    Serial.printf("[tls] %s apos handshake: livre=%u maior_bloco=%u | a TLS pegou %d B\n", rq.host, (unsigned)freeAfter,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (int)freeBefore - (int)freeAfter);
  }

  String head = String(rq.method) + " " + rq.path + (rq.http10 ? " HTTP/1.0" : " HTTP/1.1") + "\r\nHost: " + rq.host +
                "\r\nConnection: close\r\n" + rq.headers;
  if (rq.body.length()) head += "Content-Length: " + String(rq.body.length()) + "\r\n";
  head += "\r\n";
  // Cabecalho e corpo num unico envio: dois registros TLS seguidos ja deram resposta vazia.
  String all = head + rq.body;
  size_t sent = c.write((const uint8_t *)all.c_str(), all.length());

  // O servidor pode levar um instante para responder, e available() so enxerga o que
  // ja foi lido do socket: espera ate 8 s pelo primeiro byte.
  uint32_t waitUntil = millis() + 8000;
  while (!c.available() && c.connected() && (int32_t)(millis() - waitUntil) < 0) delay(10);
  bool avail = c.available() > 0, conn = c.connected();

  String status = c.readStringUntil('\n');
  rs.statusLine = status;
  int sp = status.indexOf(' ');
  rs.status = sp > 0 ? status.substring(sp + 1).toInt() : 0;
  if (rs.status == 0) {
    rs.error = String("sem resposta (enviou ") + sent + "/" + all.length() + ", dados=" + avail + ", conectado=" + conn + ")";
    c.stop();
    if (resume) sessDrop(sl);
    rs.total_ms = millis() - t0;
    return false;
  }

  while (c.connected() || c.available()) {
    String h = c.readStringUntil('\n');
    h.trim();
    if (h.isEmpty()) break;
    int colon = h.indexOf(':');
    if (colon <= 0) continue;
    String name = h.substring(0, colon);
    name.toLowerCase();
    for (int i = 0; i < rq.want_count; i++) {
      if (name == rq.want[i].name) {
        rq.want[i].value = h.substring(colon + 1);
        rq.want[i].value.trim();
      }
    }
  }

  if (rq.onBody) {
    rq.onBody(c);
  } else {
    rs.body.reserve(rq.max_body);
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
  c.stop();
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
  https_lock();
  if (!contiguousOk()) {
    // Sem nem 20 KB contiguos a TLS falharia: falha rapido e deixa o chamador adiar a consulta.
    unsigned blk = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);    Serial.printf("[https] %s%s: sem memoria contigua (bloco %u B < %u B), nao tenta a TLS\n", rq.host, rq.path, blk,
                  (unsigned)TLS_MIN_BLOCK);
    char err[64];
    snprintf(err, sizeof(err), "sem memoria contigua (bloco %u B)", blk);
    rs.error = err;
    rs.memOrCertFail = true;
    if (yields) memCertFlag = true;
    https_unlock();
    return false;
  }
  bool ok = attempt(rq, rs);
  if (!ok) {
    // Primeira conexao sem resposta (visto com Anthropic e Google): uma nova tentativa
    // logo em seguida costuma passar. O motivo da primeira vai para o log.
    // X509 FATAL / falta de memoria nao mudam em 2 s: no maximo 1 tentativa extra nesses casos.
    uint32_t firstMs = rs.total_ms;
    bool memCert = isMemOrCertFail(rs.error);
    int maxRetry = memCert ? 1 : 2;
    for (int retry = 0; retry < maxRetry && !ok; retry++) {
      if (yields && interactivePending) {
        Serial.printf("[https] %s%s: cedendo a vez a um comando do Spotify\n", rq.host, rq.path);
        break;
      }
      Serial.printf("[https] %s%s: %s | heap %u, bloco %u | tentando de novo\n", rq.host, rq.path, rs.error.c_str(),
                    ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      // A pausa e SEM a trava: um comando do Spotify entra na frente. O unlock refaz o lastro de heap
      // e o lock seguinte o libera de novo, entao a proxima tentativa ve o mesmo heap de antes.
      https_unlock();
      delay(2000);  // a memoria se reorganiza quando a conexao anterior termina de ser liberada
      https_lock();
      if (yields && interactivePending) {
        Serial.printf("[https] %s%s: cedendo a vez a um comando do Spotify\n", rq.host, rq.path);
        break;
      }
      if (!contiguousOk()) {  // a pausa nao devolveu memoria: nao adianta repetir
        Serial.printf("[https] %s%s: sem memoria contigua, desistindo das retentativas\n", rq.host, rq.path);
        memCert = true;
        break;
      }
      String lastErr = rs.error;
      rs = HttpsResult();
      ok = attempt(rq, rs);
      if (!ok && rs.error.isEmpty()) rs.error = lastErr;
      if (!ok) memCert = memCert || isMemOrCertFail(rs.error);
    }
    rs.total_ms += firstMs;
    rs.memOrCertFail = !ok && memCert;
    if (yields && rs.memOrCertFail) memCertFlag = true;
  }
  https_unlock();
  return ok;
}
