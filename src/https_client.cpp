#include "https_client.h"

#include <WiFiClientSecure.h>

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

// Uma tentativa completa. Retorna false so se nao houve resposta HTTP nenhuma
// (conexao/TLS falhou, ou o servidor fechou sem responder).
static bool attempt(HttpsRequest &rq, HttpsResult &rs) {
  WiFiClientSecure c;
  c.setCACertBundle(ca_bundle_start);
  c.setHandshakeTimeout(15);
  c.setTimeout(10000);

  uint32_t t0 = millis();
  uint32_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  Serial.printf("[tls] %s antes do connect: livre=%u maior_bloco=%u\n", rq.host, (unsigned)freeBefore,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (!c.connect(rq.host, 443)) {
    char err[64];
    c.lastError(err, sizeof(err));
    rs.error = err;
    rs.total_ms = millis() - t0;
    return false;
  }
  rs.handshake_ms = millis() - t0;
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
void https_interactive(bool on) { interactivePending = on; }
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
  if (yields && interactivePending) {
    uint32_t w0 = millis();
    while (interactivePending && millis() - w0 < 20000) delay(50);
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
