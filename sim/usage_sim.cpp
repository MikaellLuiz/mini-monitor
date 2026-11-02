// Uso das IAs falso: estados escolhidos pelo roteiro ("usage <provedor> <estado>"),
// so para conferir as telas. Os numeros reais vem da placa.

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "sim.h"
#include "usage.h"

static ProviderUsage data[PROV_COUNT];
static bool account[PROV_COUNT] = {true, true, true};
static CodexLogin loginState;

static void setWin(UsageWindow &w, const char *name, float pct, int resetInMin) {
  uint32_t now;
  board_epoch_now(&now);
  snprintf(w.name, sizeof(w.name), "%s", name);
  w.pct = pct;
  w.reset_epoch = resetInMin ? (uint32_t)(now + resetInMin * 60) : 0;
}

static void set(Provider p, UsageSource src, int agoMin, const char *err) {
  uint32_t now;
  board_epoch_now(&now);
  data[p].source = src;
  data[p].updated_epoch = agoMin >= 0 ? now - agoMin * 60 : 0;
  snprintf(data[p].error, sizeof(data[p].error), "%s", err);
}

void usage_begin() {
  data[PROV_CLAUDE] = {};
  setWin(data[PROV_CLAUDE].win[0], "5h", 37, 108);
  setWin(data[PROV_CLAUDE].win[1], "Semana", 12, 3 * 24 * 60 + 95);
  set(PROV_CLAUDE, UsageSource::Direct, 1, "");
  data[PROV_CODEX] = {};  // plano free: uma janela de 30 dias
  setWin(data[PROV_CODEX].win[0], "Mes", 91, 22 * 24 * 60);
  set(PROV_CODEX, UsageSource::Direct, 2, "");
  data[PROV_ANTIGRAVITY] = {};
  setWin(data[PROV_ANTIGRAVITY].win[0], "5h", 0, 255);
  setWin(data[PROV_ANTIGRAVITY].win[1], "Semana", 10, 3 * 24 * 60 + 9 * 60);
  set(PROV_ANTIGRAVITY, UsageSource::Direct, 3, "");
}

ProviderUsage usage_get(Provider p) {
  if (!account[p]) {
    ProviderUsage u = {};
    u.source = UsageSource::NoAccount;
    u.win[0].pct = u.win[1].pct = -1;
    return u;
  }
  return data[p];
}

void usage_refresh_now() { board_log("[sim] usage: atualizar agora"); }
bool usage_is_refreshing(Provider) { return false; }
bool usage_has_account(Provider p) { return account[p]; }
bool usage_set_account(Provider p, const char *) { return account[p] = true; }
void usage_forget_account(Provider p) { account[p] = false; }

void codex_login_start() {
  loginState = {};
  loginState.state = LoginState::Waiting;
  snprintf(loginState.code, sizeof(loginState.code), "R3P3-K7CVL");
  snprintf(loginState.url, sizeof(loginState.url), "auth.openai.com/codex/device");
  loginState.seconds_left = 14 * 60 + 20;
}
void codex_login_cancel() { loginState = {}; }
CodexLogin codex_login_status() { return loginState; }

// Roteiro: "usage <claude|codex|agy> <direct|cache|failed|waiting|none|high|closed>"
bool usage_sim_set_state(const char *prov, const char *name) {
  Provider p = !strcmp(prov, "claude") ? PROV_CLAUDE : !strcmp(prov, "codex") ? PROV_CODEX
             : !strcmp(prov, "agy") ? PROV_ANTIGRAVITY : PROV_COUNT;
  if (p == PROV_COUNT) return false;
  account[p] = true;
  if (!strcmp(name, "direct")) {
    set(p, UsageSource::Direct, 1, "");
  } else if (!strcmp(name, "high")) {
    setWin(data[p].win[0], data[p].win[0].name, 93, 40);
    if (data[p].win[1].name[0]) setWin(data[p].win[1], data[p].win[1].name, 78, 2 * 24 * 60);
    set(p, UsageSource::Direct, 0, "");
  } else if (!strcmp(name, "cache")) {
    set(p, UsageSource::Cache, 42, "sem conexao");
  } else if (!strcmp(name, "closed")) {
    setWin(data[p].win[0], data[p].win[0].name, 61, -5);
    set(p, UsageSource::Direct, 20, "");
  } else if (!strcmp(name, "failed")) {
    data[p] = {};
    data[p].win[0].pct = data[p].win[1].pct = -1;
    set(p, UsageSource::Failed, -1, "login expirou");
  } else if (!strcmp(name, "waiting")) {
    data[p] = {};
    data[p].win[0].pct = data[p].win[1].pct = -1;
    set(p, UsageSource::Waiting, -1, "");
  } else if (!strcmp(name, "none")) {
    account[p] = false;
  } else {
    return false;
  }
  return true;
}
