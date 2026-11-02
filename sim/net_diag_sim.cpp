// Diagnostico de rede falso: resultados de exemplo saindo um a cada 800 ms,
// so para conferir a tela. Os numeros reais vem da placa.

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "net_diag.h"

static const NetDiagResult SAMPLE[] = {
    {"Claude", "api.anthropic.com", DiagState::Ok, 401, 1180, 1530, 46 * 1024, "cloudflare"},
    {"Codex", "chatgpt.com", DiagState::Blocked, 403, 1420, 1690, 47 * 1024, "Cloudflare"},
    {"Login OpenAI", "auth.openai.com", DiagState::Ok, 200, 1310, 1720, 46 * 1024, "cloudflare"},
    {"Antigravity", "daily-cloudcode-pa.googleapis.com", DiagState::Ok, 401, 980, 1240, 45 * 1024, "ESF"},
    {"Spotify", "api.spotify.com", DiagState::Failed, 0, 0, 15000, 0, "TLS: timeout"},
};
static constexpr int N = sizeof(SAMPLE) / sizeof(SAMPLE[0]);
static constexpr uint32_t STEP_MS = 800;
static uint32_t startedAt = 0;
static bool started = false;

void net_diag_start() {
  if (net_diag_running()) return;
  startedAt = board_millis();
  started = true;
}

static int done() { return started ? (int)((board_millis() - startedAt) / STEP_MS) : 0; }

bool net_diag_running() { return started && done() < N; }
int net_diag_count() { return N; }

bool net_diag_get(int idx, NetDiagResult *out) {
  if (idx < 0 || idx >= N) return false;
  int d = done();
  if (idx < d) {
    *out = SAMPLE[idx];
  } else {
    *out = {};
    snprintf(out->label, sizeof(out->label), "%s", SAMPLE[idx].label);
    out->state = idx == d && started ? DiagState::Running : DiagState::Pending;
  }
  return true;
}

NetDiagSummary net_diag_summary() { return {47 * 1024, 61 * 1024, 64 * 1024, 3100}; }
