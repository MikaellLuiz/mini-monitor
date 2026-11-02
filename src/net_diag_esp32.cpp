// Sonda HTTPS numa task propria: o handshake leva segundos e nao pode travar o
// loop do LVGL. Uma conexao por vez (cada uma reserva ~32 KB so de buffer TLS).

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "board.h"
#include "https_client.h"
#include "net_diag.h"

struct Target {
  const char *label, *host, *method, *path;
};
static const Target TARGETS[] = {
    {"Claude", "api.anthropic.com", "GET", "/v1/models"},
    {"Codex", "chatgpt.com", "GET", "/backend-api/wham/usage"},
    {"Login OpenAI", "auth.openai.com", "GET", "/.well-known/openid-configuration"},
    {"Antigravity", "daily-cloudcode-pa.googleapis.com", "POST", "/v1internal:fetchAvailableModels"},
    {"Spotify", "api.spotify.com", "GET", "/v1/me"},
};
static constexpr int N = sizeof(TARGETS) / sizeof(TARGETS[0]);
static constexpr uint32_t TASK_STACK = 10 * 1024;

static NetDiagResult results[N];
static NetDiagSummary summary;
static volatile bool running = false;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static void publish(int i, const NetDiagResult &r) {
  portENTER_CRITICAL(&mux);
  results[i] = r;
  portEXIT_CRITICAL(&mux);
}

static void probe(int i) {
  const Target &t = TARGETS[i];
  NetDiagResult r = results[i];
  r.state = DiagState::Running;
  publish(i, r);

  if (WiFi.status() != WL_CONNECTED) {
    r.state = DiagState::Failed;
    snprintf(r.detail, sizeof(r.detail), "sem Wi-Fi");
    publish(i, r);
    return;
  }

  WiFiClientSecure c;
  c.setCACertBundle(ca_bundle_start);
  c.setHandshakeTimeout(15);
  c.setTimeout(10000);

  uint32_t before = ESP.getFreeHeap();
  uint32_t t0 = millis();
  if (!c.connect(t.host, 443)) {
    char err[64];
    c.lastError(err, sizeof(err));
    r.state = DiagState::Failed;
    r.total_ms = millis() - t0;
    snprintf(r.detail, sizeof(r.detail), "TLS: %s", err);
    publish(i, r);
    board_log("[tls] %s %s: FALHOU (%s) em %u ms", t.label, t.host, err, (unsigned)r.total_ms);
    return;
  }
  r.handshake_ms = millis() - t0;
  r.tls_heap_bytes = before - ESP.getFreeHeap();
  if (r.tls_heap_bytes > summary.tls_heap_max) summary.tls_heap_max = r.tls_heap_bytes;

  bool post = !strcmp(t.method, "POST");
  c.printf("%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: painel-cyd/0.1\r\nAccept: */*\r\n"
           "Connection: close\r\n",
           t.method, t.path, t.host);
  c.print(post ? "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}" : "\r\n");

  String status = c.readStringUntil('\n');
  int sp = status.indexOf(' ');
  r.http_status = sp > 0 ? status.substring(sp + 1).toInt() : 0;

  bool cfChallenge = false;
  String server;
  while (c.connected() || c.available()) {
    String h = c.readStringUntil('\n');
    h.trim();
    if (h.isEmpty()) break;
    h.toLowerCase();
    if (h.startsWith("cf-mitigated:")) cfChallenge = true;
    if (h.startsWith("server:")) server = h.substring(7);
  }
  char body[160] = {0};
  c.readBytes(body, sizeof(body) - 1);
  if (strstr(body, "Just a moment") || strstr(body, "challenge-platform")) cfChallenge = true;
  c.stop();
  r.total_ms = millis() - t0;

  server.trim();
  if (cfChallenge) {
    r.state = DiagState::Blocked;
    snprintf(r.detail, sizeof(r.detail), "Cloudflare");  // pagina de desafio no lugar da API
  } else if (r.http_status > 0) {
    r.state = DiagState::Ok;
    snprintf(r.detail, sizeof(r.detail), "%s", server.c_str());
  } else {
    r.state = DiagState::Failed;
    snprintf(r.detail, sizeof(r.detail), "sem resposta HTTP");
  }
  publish(i, r);
  board_log("[tls] %s %s: HTTP %d %s | handshake %u ms, total %u ms | TLS %u B | heap livre %u, min %u | %s",
            t.label, t.host, r.http_status, cfChallenge ? "(CLOUDFLARE)" : "", (unsigned)r.handshake_ms,
            (unsigned)r.total_ms, (unsigned)r.tls_heap_bytes, ESP.getFreeHeap(), ESP.getMinFreeHeap(),
            server.c_str());
}

static void task(void *) {
  https_lock();  // nao abrir TLS junto com a consulta de uso (nao cabem duas)
  for (int i = 0; i < N; i++) probe(i);
  https_unlock();
  summary.heap_min_free = ESP.getMinFreeHeap();
  summary.largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  summary.stack_free_min = uxTaskGetStackHighWaterMark(nullptr);
  board_log("[tls] fim | maior TLS %u B | heap min %u | maior bloco %u | pilha livre min %u B",
            (unsigned)summary.tls_heap_max, (unsigned)summary.heap_min_free,
            (unsigned)summary.largest_block, (unsigned)summary.stack_free_min);
  running = false;
  vTaskDelete(nullptr);
}

void net_diag_start() {
  if (running) return;
  for (int i = 0; i < N; i++) {
    results[i] = {};
    strlcpy(results[i].label, TARGETS[i].label, sizeof(results[i].label));
    strlcpy(results[i].host, TARGETS[i].host, sizeof(results[i].host));
  }
  summary = {};
  running = true;
  // Nucleo 0 (o do Wi-Fi): a TLS segurava o nucleo do loop e a placa atrasava as respostas pelo USB.
  // Prioridade da idle (tskIDLE_PRIORITY): a idle do nucleo 0 e vigiada pelo watchdog, ver usage_esp32.cpp.
  if (xTaskCreatePinnedToCore(task, "netdiag", TASK_STACK, nullptr, tskIDLE_PRIORITY, nullptr, 0) != pdPASS) {
    running = false;
    board_log("[tls] sem memoria para criar a task");
  }
}

bool net_diag_running() { return running; }
int net_diag_count() { return N; }

bool net_diag_get(int idx, NetDiagResult *out) {
  if (idx < 0 || idx >= N) return false;
  portENTER_CRITICAL(&mux);
  *out = results[idx];
  portEXIT_CRITICAL(&mux);
  return true;
}

NetDiagSummary net_diag_summary() { return summary; }
