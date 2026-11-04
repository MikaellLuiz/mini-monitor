// Consulta de uso das IAs na placa: uma task que, com Wi-Fi e relogio prontos,
// consulta cada provedor configurado no intervalo, guarda o ultimo valor bom na
// NVS (vira "Cache" depois de um reinicio) e publica para a interface. Tambem
// conduz o login do Codex por codigo. Uma conexao HTTPS por vez, sempre.
//
// Nenhum token vai para o log.

#include <ArduinoJson.h>
#include <Arduino.h>
#include <Preferences.h>
#include "ui.h"
#include <WiFi.h>
#include <mbedtls/base64.h>

#include "board.h"
#include "https_client.h"
#include "pcagent.h"
#include "spotify.h"
#include "usage.h"

static constexpr uint32_t INTERVAL_MS = 5 * 60 * 1000;
static constexpr uint32_t RETRY_MS = 60 * 1000;            // falha de rede/servidor
static constexpr uint32_t MEMFAIL_RETRY_MS = 150 * 1000;  // falha X509/memoria: nao martela a trava
static constexpr uint32_t BOOT_WAIT_MS = 30 * 1000;       // 1a rodada: espera o Spotify, no maximo isto apos o Wi-Fi
static constexpr uint32_t FIRST_SPACING_MS = 15 * 1000;   // 1a rodada: intervalo entre provedores
static constexpr uint32_t AUTH_RETRY_MS = 30 * 60 * 1000;  // login/token recusado
static constexpr uint32_t TASK_STACK = 10 * 1024;  // medido: pico 7168 B com a TLS do chatgpt.com (9216 deixou so 2048 B) + ~3 KB de folga
static constexpr uint32_t REFRESH_MARGIN_S = 300;  // renova o access token 5 min antes de vencer
static const char *NVS_ACCOUNTS = "accounts";
static const char *NVS_CACHE = "usage";

static const char *CLAUDE_MODEL = "claude-haiku-5-5";
static const char *CODEX_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann";
static const char *AUTH_HOST = "auth.openai.com";
// Client OAuth publico do proprio IDE do Antigravity (embutido no language_server).
static const char *AGY_CLIENT_ID = "1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps.googleusercontent.com";
static const char *AGY_CLIENT_SECRET = "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";

struct Slot {
  ProviderUsage u;
  String token;     // Claude: setup-token | Codex e Antigravity: refresh token
  uint32_t nextAt;  // millis da proxima consulta (0 = assim que der)
  String access;    // Codex/Antigravity: access token atual
  uint32_t accessExp;  // epoch em que o access token vence
  String accountId;    // Codex: ChatGPT-Account-Id
  bool refreshing;     // atualizacao pedida e ainda sem resposta (a interface mostra "atualizando...")
};
static Slot slots[PROV_COUNT];
static volatile bool refreshRequested = false;

// Mutex (nao spinlock): os trechos protegidos copiam String e leem a NVS. Criado
// na inicializacao estatica: a interface pode ler o uso antes de usage_begin().
static SemaphoreHandle_t lock = xSemaphoreCreateMutex();
struct Guard {
  Guard() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(lock); }
};

static const char *key(Provider p) {
  static const char *KEYS[] = {"claude", "codex", "antigravity"};
  return KEYS[p];
}

// ---- NVS ---------------------------------------------------------------------

static void nvsPut(const char *ns, const char *k, const String &v) {
  Preferences prefs;
  prefs.begin(ns, false);
  prefs.putString(k, v);
  prefs.end();
}
static String nvsGet(const char *ns, const char *k) {
  Preferences prefs;
  prefs.begin(ns, true);
  String v = prefs.getString(k, "");
  prefs.end();
  return v;
}
static void nvsDel(const char *ns, const char *k) {
  Preferences prefs;
  prefs.begin(ns, false);
  prefs.remove(k);
  prefs.end();
}

struct CacheBlob {
  char name[2][8];
  float pct[2];
  uint32_t reset[2], updated;
};

static void saveCache(Provider p, const ProviderUsage &u) {
  CacheBlob b = {};
  for (int i = 0; i < 2; i++) {
    memcpy(b.name[i], u.win[i].name, sizeof(b.name[i]));
    b.pct[i] = u.win[i].pct;
    b.reset[i] = u.win[i].reset_epoch;
  }
  b.updated = u.updated_epoch;
  Preferences prefs;
  prefs.begin(NVS_CACHE, false);
  prefs.putBytes(key(p), &b, sizeof(b));
  prefs.end();
}

static bool loadCache(Provider p, ProviderUsage &u) {
  CacheBlob b;
  Preferences prefs;
  prefs.begin(NVS_CACHE, true);
  bool ok = prefs.getBytes(key(p), &b, sizeof(b)) == sizeof(b);
  prefs.end();
  if (!ok) return false;
  for (int i = 0; i < 2; i++) {
    memcpy(u.win[i].name, b.name[i], sizeof(u.win[i].name));
    u.win[i].pct = b.pct[i];
    u.win[i].reset_epoch = b.reset[i];
  }
  u.updated_epoch = b.updated;
  return true;
}

// ---- publicacao --------------------------------------------------------------

// Publica o resultado de uma consulta (contando a tentativa, para quem espera por ela).
static void publish(Provider p, const ProviderUsage &u) {
  Guard g;
  uint16_t attempts = slots[p].u.attempts + 1;
  slots[p].u = u;
  slots[p].u.attempts = attempts;
}

static ProviderUsage snapshot(Provider p) {
  Guard g;
  return slots[p].u;
}

// Falhou: mantem os valores antigos como Cache (com o motivo), ou Failed se nao ha.
static void publishFailure(Provider p, const char *why) {
  ProviderUsage u = snapshot(p);
  u.source = u.updated_epoch ? UsageSource::Cache : UsageSource::Failed;
  strlcpy(u.error, why, sizeof(u.error));
  publish(p, u);
}

static void publishOk(Provider p, ProviderUsage &u) {
  u.source = UsageSource::Direct;
  u.error[0] = 0;
  board_epoch_now(&u.updated_epoch);
  publish(p, u);
  saveCache(p, u);
}

static void setWin(UsageWindow &w, const char *name, float pct, uint32_t reset) {
  strlcpy(w.name, name, sizeof(w.name));
  w.pct = pct;
  w.reset_epoch = reset;
}

static ProviderUsage newUsage() {
  ProviderUsage u = {};
  u.win[0].pct = u.win[1].pct = -1;
  return u;
}

// ---- utilitarios -------------------------------------------------------------

// "2026-10-31T07:50:01Z" (ou com fracao de segundo) -> segundos Unix (UTC).
static uint32_t parseIsoUtc(const char *s) {
  int y, mo, d, h, mi, se;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  y -= mo <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  int yoe = y - era * 400;
  int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + doe - 719468;
  return (uint32_t)(days * 86400L + h * 3600L + mi * 60L + se);
}

// Reset do Claude vem como segundos Unix; aceita tambem data ISO 8601.
static uint32_t parseReset(const String &v) {
  if (v.isEmpty()) return 0;
  if (v.indexOf('-') < 0) return (uint32_t)v.toInt();
  return parseIsoUtc(v.c_str());
}

static String urlEncode(const String &s) {
  String o;
  for (char c : s) {
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
    else {
      char b[4];
      snprintf(b, sizeof(b), "%%%02X", (unsigned char)c);
      o += b;
    }
  }
  return o;
}

// chatgpt_account_id dentro de um JWT (parte do meio, base64url).
static bool accountIdFromJwt(const char *jwt, String &out) {
  const char *p1 = strchr(jwt, '.');
  const char *p2 = p1 ? strchr(p1 + 1, '.') : nullptr;
  if (!p2) return false;
  String b64(p1 + 1, p2 - p1 - 1);
  b64.replace('-', '+');
  b64.replace('_', '/');
  while (b64.length() % 4) b64 += '=';
  size_t cap = b64.length(), n = 0;
  unsigned char *buf = (unsigned char *)malloc(cap + 1);
  if (!buf) return false;
  bool ok = false;
  if (mbedtls_base64_decode(buf, cap, &n, (const unsigned char *)b64.c_str(), b64.length()) == 0) {
    buf[n] = 0;
    const char *k = strstr((const char *)buf, "\"chatgpt_account_id\":\"");
    if (k) {
      k += strlen("\"chatgpt_account_id\":\"");
      const char *e = strchr(k, '"');
      if (e) {
        out = String(k, e - k);
        ok = true;
      }
    }
  }
  free(buf);
  return ok;
}

// Respostas "chunked" comecam com o tamanho do bloco (ex.: "1a3") antes do '{': pula
// ate o JSON. Com HTTP/1.0 nao ha nada a pular.
static void skipToJson(Stream &s) {
  for (int i = 0; i < 24 && s.available() && s.peek() != '{'; i++) s.read();
}

static bool epochNow(uint32_t &now) { return board_epoch_now(&now); }

// ---- Claude ------------------------------------------------------------------

// Retorna o atraso ate a proxima tentativa.
static uint32_t fetchClaude(String &token) {
  HttpsHeader want[] = {
      {"anthropic-ratelimit-unified-5h-utilization"}, {"anthropic-ratelimit-unified-5h-reset"},
      {"anthropic-ratelimit-unified-7d-utilization"}, {"anthropic-ratelimit-unified-7d-reset"},
  };
  HttpsRequest rq;
  rq.host = "api.anthropic.com";
  rq.method = "POST";
  rq.path = "/v1/messages";
  rq.headers = "Authorization: Bearer " + token +
               "\r\nanthropic-version: 2023-06-01\r\nanthropic-beta: oauth-2025-04-20\r\n"
               "Content-Type: application/json\r\nUser-Agent: claude-code/2.1.5\r\n";
  token = String();  // libera a copia antes da TLS
  // Sem o bloco de identidade do Claude Code no `system`, a API responde a tokens de
  // assinatura com 429 "Error" sem headers (anthropics/claude-code#103731).
  rq.body = String("{\"model\":\"") + CLAUDE_MODEL +
            "\",\"max_tokens\":1,\"thinking\":{\"type\":\"disabled\"},"
            "\"system\":[{\"type\":\"text\",\"text\":\"You are Claude Code, Anthropic's official CLI for Claude.\"}],"
            "\"messages\":[{\"role\":\"user\",\"content\":\".\"}]}";
  rq.want = want;
  rq.want_count = 4;
  rq.max_body = 300;

  HttpsResult rs;
  bool ok = https_send(rq, rs);
  board_log("[claude] HTTP %d em %u ms | 5h=%s 7d=%s | heap %u, bloco %u%s%s", rs.status,
            (unsigned)rs.total_ms, want[0].value.c_str(), want[2].value.c_str(), ESP.getFreeHeap(),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), ok ? "" : " | erro: ",
            rs.error.c_str());

  bool haveHeaders = !want[0].value.isEmpty() || !want[2].value.isEmpty();
  if (!ok && !haveHeaders) {  // os headers de uso valem mesmo se a linha de status veio estranha
    publishFailure(PROV_CLAUDE, "sem conexao");
    return RETRY_MS;
  }
  // 429 (limite atingido) tambem traz os headers e vale como leitura.
  if (!haveHeaders) {
    if (rs.status == 401 || rs.status == 403) {
      publishFailure(PROV_CLAUDE, "token recusado");
      return AUTH_RETRY_MS;
    }
    char why[40];
    snprintf(why, sizeof(why), "sem dados de uso (HTTP %d)", rs.status);
    publishFailure(PROV_CLAUDE, why);
    return RETRY_MS;
  }

  ProviderUsage u = newUsage();
  setWin(u.win[0], "5h", want[0].value.isEmpty() ? -1.0f : want[0].value.toFloat() * 100, parseReset(want[1].value));
  setWin(u.win[1], "Semana", want[2].value.isEmpty() ? -1.0f : want[2].value.toFloat() * 100, parseReset(want[3].value));
  publishOk(PROV_CLAUDE, u);
  return INTERVAL_MS;
}

// A sonda do Claude ABRE uma janela de 5 h se nenhuma estiver aberta (visto pelo
// projeto clauth): consultar com a janela vencida comecaria a contar as 5 h do
// usuario antes de ele usar. Entao a consulta automatica so roda com a janela
// aberta (reset ainda no futuro) ou se nunca houve leitura; com ela vencida, so
// "Atualizar agora" consulta. O agente do PC (marco 7) pode avisar quando houver uso.
static bool claudeWindowClosed(const ProviderUsage &u) {
  uint32_t now;
  return u.updated_epoch && u.win[0].reset_epoch && epochNow(now) && u.win[0].reset_epoch <= now;
}

// ---- Codex -------------------------------------------------------------------

// Renova o access token. O refresh token GIRA a cada renovacao: o novo e gravado na NVS
// dentro do proprio callback, antes de qualquer outra coisa, para um desligamento no
// meio nao deixar o login perdido. false = sem resposta; `authFailed` = login invalido.
static bool codexRefresh(bool &authFailed) {
  authFailed = false;
  String refresh;
  {
    Guard g;
    refresh = slots[PROV_CODEX].token;
  }
  HttpsRequest rq;
  rq.host = AUTH_HOST;
  rq.method = "POST";
  rq.path = "/oauth/token";
  rq.http10 = true;
  rq.headers = "Content-Type: application/json\r\nUser-Agent: painel-cyd/0.1\r\n";
  rq.body = String("{\"client_id\":\"") + CODEX_CLIENT_ID +
            "\",\"grant_type\":\"refresh_token\",\"refresh_token\":\"" + refresh + "\"}";
  refresh = String();
  bool saved = false;
  rq.onBody = [&](Stream &s) {
    skipToJson(s);
    JsonDocument filter;
    filter["access_token"] = true;
    filter["refresh_token"] = true;
    filter["expires_in"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, s, DeserializationOption::Filter(filter))) return;
    const char *at = doc["access_token"];
    const char *rt = doc["refresh_token"];
    if (!at || !rt) return;
    uint32_t now = 0;
    epochNow(now);
    uint32_t exp = now + (doc["expires_in"] | 3600);
    nvsPut(NVS_ACCOUNTS, "codex", rt);  // primeiro o refresh token novo
    nvsPut(NVS_ACCOUNTS, "codex_at", at);
    Preferences prefs;
    prefs.begin(NVS_ACCOUNTS, false);
    prefs.putUInt("codex_exp", exp);
    prefs.end();
    Guard g;
    slots[PROV_CODEX].token = rt;
    slots[PROV_CODEX].access = at;
    slots[PROV_CODEX].accessExp = exp;
    saved = true;
  };
  HttpsResult rs;
  bool ok = https_send(rq, rs);
  board_log("[codex] renovacao: HTTP %d %s em %u ms | heap %u, bloco %u", rs.status, saved ? "ok" : "falhou",
            (unsigned)rs.total_ms, ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (saved) return true;
  authFailed = ok && (rs.status == 400 || rs.status == 401 || rs.status == 403);
  return false;
}

static const char *codexWindowName(int seconds) {
  if (seconds <= 6 * 3600) return "5h";
  if (seconds <= 8 * 86400) return "Semana";
  return "Mes";
}

static uint32_t fetchCodex() {
  uint32_t now = 0;
  epochNow(now);
  String access, account;
  uint32_t exp;
  {
    Guard g;
    access = slots[PROV_CODEX].access;
    exp = slots[PROV_CODEX].accessExp;
    account = slots[PROV_CODEX].accountId;
  }
  bool authFailed;
  if (access.isEmpty() || exp < now + REFRESH_MARGIN_S) {
    if (!codexRefresh(authFailed)) {
      publishFailure(PROV_CODEX, authFailed ? "login expirou" : "sem conexao");
      return authFailed ? AUTH_RETRY_MS : RETRY_MS;
    }
    Guard g;
    access = slots[PROV_CODEX].access;
  }

  struct Win {
    int seconds;
    float pct;
    uint32_t reset;
  } wins[2];
  int nwin = 0;
  HttpsRequest rq;
  rq.host = "chatgpt.com";
  rq.method = "GET";
  rq.path = "/backend-api/wham/usage";
  rq.http10 = true;
  rq.headers = "Authorization: Bearer " + access + "\r\nChatGPT-Account-Id: " + account +
               "\r\nAccept: application/json\r\nUser-Agent: painel-cyd/0.1\r\n";
  access = String();
  account = String();
  rq.onBody = [&](Stream &s) {
    skipToJson(s);
    JsonDocument filter;
    for (const char *w : {"primary_window", "secondary_window"}) {
      filter["rate_limit"][w]["used_percent"] = true;
      filter["rate_limit"][w]["limit_window_seconds"] = true;
      filter["rate_limit"][w]["reset_at"] = true;
    }
    JsonDocument doc;
    if (deserializeJson(doc, s, DeserializationOption::Filter(filter))) return;
    for (const char *w : {"primary_window", "secondary_window"}) {
      JsonObject o = doc["rate_limit"][w];
      if (o.isNull() || nwin >= 2) continue;
      wins[nwin++] = {o["limit_window_seconds"] | 0, o["used_percent"] | -1.0f, o["reset_at"] | 0u};
    }
  };
  HttpsResult rs;
  https_send(rq, rs);
  board_log("[codex] uso: HTTP %d em %u ms, %d janelas | heap %u, bloco %u", rs.status, (unsigned)rs.total_ms, nwin,
            ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  if (rs.status == 401 || rs.status == 403) {  // access token recusado: renova na proxima
    Guard g;
    slots[PROV_CODEX].accessExp = 0;
    publishFailure(PROV_CODEX, "token recusado");
    return 10 * 1000;
  }
  if (rs.status != 200 || nwin == 0) {
    char why[40];
    snprintf(why, sizeof(why), rs.status == 200 ? "sem janelas de uso" : "HTTP %d", rs.status);
    publishFailure(PROV_CODEX, why);
    return RETRY_MS;
  }
  if (nwin == 2 && wins[1].seconds < wins[0].seconds) std::swap(wins[0], wins[1]);
  ProviderUsage u = newUsage();
  for (int i = 0; i < nwin; i++) setWin(u.win[i], codexWindowName(wins[i].seconds), wins[i].pct, wins[i].reset);
  publishOk(PROV_CODEX, u);
  return INTERVAL_MS;
}

// ---- Login do Codex por codigo -----------------------------------------------

static SemaphoreHandle_t loginLock = xSemaphoreCreateMutex();
static CodexLogin login = {};
static String loginDeviceId, loginUserCode;
static uint32_t loginInterval = 5, loginNextPoll = 0, loginDeadline = 0;

static void loginFail(const char *why) {
  xSemaphoreTake(loginLock, portMAX_DELAY);
  login.state = LoginState::Failed;
  strlcpy(login.error, why, sizeof(login.error));
  xSemaphoreGive(loginLock);
  board_log("[codex] login falhou: %s", why);
}

static bool postJson(const char *path, const String &body, HttpsResult &rs) {
  HttpsRequest rq;
  rq.host = AUTH_HOST;
  rq.method = "POST";
  rq.path = path;
  rq.http10 = true;
  rq.headers = "Content-Type: application/json\r\nUser-Agent: painel-cyd/0.1\r\n";
  rq.body = body;
  rq.max_body = 1500;  // a resposta do token traz codigo, verificador e desafio PKCE
  return https_send(rq, rs);
}

// Troca o codigo de autorizacao por tokens e grava tudo (inclui o account id).
static void codexExchange(const String &authCode, const String &verifier) {
  HttpsRequest rq;
  rq.host = AUTH_HOST;
  rq.method = "POST";
  rq.path = "/oauth/token";
  rq.http10 = true;
  rq.headers = "Content-Type: application/x-www-form-urlencoded\r\nUser-Agent: painel-cyd/0.1\r\n";
  rq.body = "grant_type=authorization_code&code=" + urlEncode(authCode) +
            "&redirect_uri=" + urlEncode(String("https://") + AUTH_HOST + "/deviceauth/callback") +
            "&client_id=" + CODEX_CLIENT_ID + "&code_verifier=" + urlEncode(verifier);
  bool saved = false;
  rq.onBody = [&](Stream &s) {
    skipToJson(s);
    JsonDocument filter;
    filter["access_token"] = true;
    filter["refresh_token"] = true;
    filter["id_token"] = true;
    filter["expires_in"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, s, DeserializationOption::Filter(filter))) return;
    const char *at = doc["access_token"], *rt = doc["refresh_token"], *idt = doc["id_token"];
    if (!at || !rt) return;
    String account;
    if (!(idt && accountIdFromJwt(idt, account)) && !accountIdFromJwt(at, account)) return;
    uint32_t now = 0;
    epochNow(now);
    uint32_t exp = now + (doc["expires_in"] | 3600);
    nvsPut(NVS_ACCOUNTS, "codex", rt);
    nvsPut(NVS_ACCOUNTS, "codex_at", at);
    nvsPut(NVS_ACCOUNTS, "codex_id", account);
    Preferences prefs;
    prefs.begin(NVS_ACCOUNTS, false);
    prefs.putUInt("codex_exp", exp);
    prefs.end();
    Guard g;
    slots[PROV_CODEX].token = rt;
    slots[PROV_CODEX].access = at;
    slots[PROV_CODEX].accessExp = exp;
    slots[PROV_CODEX].accountId = account;
    ProviderUsage u = newUsage();
    u.source = UsageSource::Waiting;
    slots[PROV_CODEX].u = u;
    slots[PROV_CODEX].nextAt = 0;
    saved = true;
  };
  HttpsResult rs;
  https_send(rq, rs);
  board_log("[codex] troca do codigo: HTTP %d %s em %u ms", rs.status, saved ? "ok" : "falhou", (unsigned)rs.total_ms);
  if (saved) {
    xSemaphoreTake(loginLock, portMAX_DELAY);
    login.state = LoginState::Done;
    xSemaphoreGive(loginLock);
  } else {
    loginFail("troca do codigo falhou");
  }
}

// Um passo do login; chamado a cada segundo pela task.
static void loginStep() {
  xSemaphoreTake(loginLock, portMAX_DELAY);
  LoginState st = login.state;
  xSemaphoreGive(loginLock);
  if (st != LoginState::Requesting && st != LoginState::Waiting) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (st == LoginState::Requesting) {
    HttpsResult rs;
    postJson("/api/accounts/deviceauth/usercode", String("{\"client_id\":\"") + CODEX_CLIENT_ID + "\"}", rs);
    JsonDocument doc;
    if (rs.status != 200 || deserializeJson(doc, rs.body.substring(max(rs.body.indexOf('{'), 0)))) {
      loginFail(rs.status == 404 ? "login por codigo desligado" : "sem resposta da OpenAI");
      return;
    }
    const char *dev = doc["device_auth_id"];
    const char *uc = doc["user_code"] | doc["usercode"].as<const char *>();
    if (!dev || !uc) {
      loginFail("resposta inesperada");
      return;
    }
    JsonVariant iv = doc["interval"];
    loginInterval = iv.is<const char *>() ? atoi(iv.as<const char *>()) : (iv | 5);
    if (loginInterval < 3) loginInterval = 5;
    loginDeviceId = dev;
    loginUserCode = uc;
    loginDeadline = millis() + 15 * 60 * 1000;
    loginNextPoll = millis() + loginInterval * 1000;
    xSemaphoreTake(loginLock, portMAX_DELAY);
    login.state = LoginState::Waiting;
    strlcpy(login.code, uc, sizeof(login.code));
    snprintf(login.url, sizeof(login.url), "https://%s/codex/device", AUTH_HOST);
    xSemaphoreGive(loginLock);
    board_log("[codex] codigo emitido; esperando a confirmacao");
    return;
  }

  // Waiting
  if ((int32_t)(millis() - loginDeadline) >= 0) {
    loginFail("codigo expirou");
    return;
  }
  xSemaphoreTake(loginLock, portMAX_DELAY);
  login.seconds_left = (loginDeadline - millis()) / 1000;
  xSemaphoreGive(loginLock);
  if ((int32_t)(millis() - loginNextPoll) < 0) return;
  loginNextPoll = millis() + loginInterval * 1000;

  HttpsResult rs;
  postJson("/api/accounts/deviceauth/token",
           "{\"device_auth_id\":\"" + loginDeviceId + "\",\"user_code\":\"" + loginUserCode + "\"}", rs);
  if (rs.status == 403 || rs.status == 404 || rs.status == 0) return;  // ainda nao confirmado
  JsonDocument doc;
  if (rs.status != 200 || deserializeJson(doc, rs.body.substring(max(rs.body.indexOf('{'), 0)))) {
    loginFail("a OpenAI recusou o login");
    return;
  }
  const char *code = doc["authorization_code"], *ver = doc["code_verifier"];
  if (!code || !ver) {
    loginFail("resposta inesperada");
    return;
  }
  String authCode = code, verifier = ver;
  xSemaphoreTake(loginLock, portMAX_DELAY);
  login.state = LoginState::Exchanging;
  xSemaphoreGive(loginLock);
  codexExchange(authCode, verifier);
}

void codex_login_start() {
  xSemaphoreTake(loginLock, portMAX_DELAY);
  if (login.state == LoginState::Idle || login.state == LoginState::Done || login.state == LoginState::Failed) {
    login = {};
    login.state = LoginState::Requesting;
  }
  xSemaphoreGive(loginLock);
}

void codex_login_cancel() {
  xSemaphoreTake(loginLock, portMAX_DELAY);
  if (login.state != LoginState::Exchanging) login = {};
  xSemaphoreGive(loginLock);
}

CodexLogin codex_login_status() {
  xSemaphoreTake(loginLock, portMAX_DELAY);
  CodexLogin l = login;
  xSemaphoreGive(loginLock);
  return l;
}

// ---- Antigravity -------------------------------------------------------------

static bool agyRefresh(bool &authFailed) {
  authFailed = false;
  String refresh;
  {
    Guard g;
    refresh = slots[PROV_ANTIGRAVITY].token;
  }
  HttpsRequest rq;
  rq.host = "oauth2.googleapis.com";
  rq.method = "POST";
  rq.path = "/token";
  rq.http10 = true;
  rq.headers = "Content-Type: application/x-www-form-urlencoded\r\nUser-Agent: antigravity\r\n";
  rq.body = String("client_id=") + AGY_CLIENT_ID + "&client_secret=" + AGY_CLIENT_SECRET +
            "&grant_type=refresh_token&refresh_token=" + urlEncode(refresh);
  refresh = String();
  bool got = false;
  rq.onBody = [&](Stream &s) {
    skipToJson(s);
    JsonDocument filter;
    filter["access_token"] = true;
    filter["expires_in"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, s, DeserializationOption::Filter(filter))) return;
    const char *at = doc["access_token"];
    if (!at) return;
    uint32_t now = 0;
    epochNow(now);
    Guard g;
    slots[PROV_ANTIGRAVITY].access = at;  // o Google nao gira o refresh token
    slots[PROV_ANTIGRAVITY].accessExp = now + (doc["expires_in"] | 3600);
    got = true;
  };
  HttpsResult rs;
  bool ok = https_send(rq, rs);
  board_log("[agy] renovacao: HTTP %d %s em %u ms", rs.status, got ? "ok" : "falhou", (unsigned)rs.total_ms);
  if (got) return true;
  authFailed = ok && (rs.status == 400 || rs.status == 401);
  return false;
}

// O servidor so devolve o grupo "Gemini Models" (e modelos novos) a clientes que se
// anunciam como uma versao recente do IDE: com o UA generico vem so Claude/GPT.
static const char *AGY_UA = "antigravity/2.25.0 windows/amd64";

// Projeto da conta (necessario para o resumo de cota). Vem uma vez do loadCodeAssist e
// fica guardado: so muda se a conta mudar.
static bool agyEnsureProject(const String &access) {
  {
    Guard g;
    if (!slots[PROV_ANTIGRAVITY].accountId.isEmpty()) return true;
  }
  String project;
  HttpsRequest rq;
  rq.host = "daily-cloudcode-pa.googleapis.com";
  rq.method = "POST";
  rq.path = "/v1internal:loadCodeAssist";
  rq.http10 = true;
  rq.headers = "Authorization: Bearer " + access + "\r\nContent-Type: application/json\r\nUser-Agent: " + AGY_UA + "\r\n";
  rq.body = "{\"metadata\":{\"ideType\":\"ANTIGRAVITY\",\"ideVersion\":\"2.25.0\",\"platform\":\"WINDOWS_AMD64\",\"pluginType\":\"GEMINI\"}}";
  rq.onBody = [&](Stream &s) {
    skipToJson(s);
    JsonDocument filter;
    filter["cloudaicompanionProject"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, s, DeserializationOption::Filter(filter))) return;
    JsonVariant p = doc["cloudaicompanionProject"];
    if (p.is<const char *>()) project = p.as<const char *>();
    else if (p["id"].is<const char *>()) project = p["id"].as<const char *>();
  };
  HttpsResult rs;
  https_send(rq, rs);
  board_log("[agy] projeto: HTTP %d %s em %u ms", rs.status, project.isEmpty() ? "nao veio" : "ok", (unsigned)rs.total_ms);
  if (project.isEmpty()) return false;
  nvsPut(NVS_ACCOUNTS, "agy_proj", project);
  Guard g;
  slots[PROV_ANTIGRAVITY].accountId = project;
  return true;
}

static uint32_t fetchAntigravity() {
  uint32_t now = 0;
  epochNow(now);
  String access;
  uint32_t exp;
  {
    Guard g;
    access = slots[PROV_ANTIGRAVITY].access;
    exp = slots[PROV_ANTIGRAVITY].accessExp;
  }
  bool authFailed;
  if (access.isEmpty() || exp < now + REFRESH_MARGIN_S) {
    if (!agyRefresh(authFailed)) {
      publishFailure(PROV_ANTIGRAVITY, authFailed ? "login expirou" : "sem conexao");
      return authFailed ? AUTH_RETRY_MS : RETRY_MS;
    }
    Guard g;
    access = slots[PROV_ANTIGRAVITY].access;
  }
  if (!agyEnsureProject(access)) {
    publishFailure(PROV_ANTIGRAVITY, "sem projeto da conta");
    return RETRY_MS;
  }
  String project;
  {
    Guard g;
    project = slots[PROV_ANTIGRAVITY].accountId;
  }

  // Grupo "Gemini Models": um limite semanal e um de 5 h (a tela "View Usage" do IDE).
  // Cada bucket traz a fracao RESTANTE; ausente = esgotado (valor zero do proto3).
  float used[2] = {-1, -1};  // [0] = 5h, [1] = semana
  uint32_t reset[2] = {0, 0};
  int buckets = 0;
  String jsonInfo;
  HttpsRequest rq;
  rq.host = "daily-cloudcode-pa.googleapis.com";
  rq.method = "POST";
  rq.path = "/v1internal:retrieveUserQuotaSummary";
  rq.http10 = true;
  rq.headers = "Authorization: Bearer " + access + "\r\nContent-Type: application/json\r\nUser-Agent: " + AGY_UA + "\r\n";
  rq.body = "{\"project\":\"" + project + "\"}";
  access = String();
  project = String();
  rq.onBody = [&](Stream &s) {
    skipToJson(s);
    JsonDocument filter;
    filter["groups"][0]["displayName"] = true;
    filter["groups"][0]["buckets"][0]["window"] = true;
    filter["groups"][0]["buckets"][0]["remainingFraction"] = true;
    filter["groups"][0]["buckets"][0]["resetTime"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, s, DeserializationOption::Filter(filter));
    JsonArray groups = doc["groups"].as<JsonArray>();
    jsonInfo = String("json=") + err.c_str() + " grupos=" + groups.size();
    for (JsonObject g : groups) {
      const char *gname = g["displayName"] | "";
      if (strncmp(gname, "Gemini Models", 13)) continue;
      for (JsonObject b : g["buckets"].as<JsonArray>()) {
        const char *w = b["window"] | "";
        int idx = !strcmp(w, "5h") ? 0 : !strcmp(w, "weekly") ? 1 : -1;
        if (idx < 0) continue;
        used[idx] = (1.0f - (b["remainingFraction"] | 0.0f)) * 100.0f;
        reset[idx] = parseIsoUtc(b["resetTime"] | "");
        buckets++;
      }
    }
  };
  HttpsResult rs;
  https_send(rq, rs);
  board_log("[agy] cota: HTTP %d em %u ms, %d limites | 5h=%d%% semana=%d%% | %s | heap %u, bloco %u", rs.status,
            (unsigned)rs.total_ms, buckets, (int)used[0], (int)used[1], jsonInfo.c_str(), ESP.getFreeHeap(),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  if (rs.status == 401 || rs.status == 403) {
    Guard g;
    slots[PROV_ANTIGRAVITY].accessExp = 0;
    publishFailure(PROV_ANTIGRAVITY, "token recusado");
    return 10 * 1000;
  }
  if (rs.status != 200 || buckets == 0) {
    char why[40];
    snprintf(why, sizeof(why), rs.status == 200 ? "sem limites do Gemini" : "HTTP %d", rs.status);
    publishFailure(PROV_ANTIGRAVITY, why);
    return RETRY_MS;
  }
  ProviderUsage u = newUsage();
  if (used[0] >= 0) setWin(u.win[0], "5h", used[0], reset[0]);
  if (used[1] >= 0) setWin(u.win[1], "Semana", used[1], reset[1]);
  publishOk(PROV_ANTIGRAVITY, u);
  return INTERVAL_MS;
}

// ---- task --------------------------------------------------------------------

// Pagina escondida (modo IA separada) = provedor desativado: nao gasta TLS nem rede com ele.
static bool providerWanted(Provider p) {
  if (!ui_ia_separate()) return true;  // modo junto: uma pagina so, sem chave por provedor
  return ui_page_enabled(p == PROV_CLAUDE ? PG_CLAUDE : p == PROV_CODEX ? PG_CODEX : PG_AGY);
}

static bool ready() {
  uint32_t now;
  return WiFi.status() == WL_CONNECTED && epochNow(now);
}

static void task(void *) {
  bool forced[PROV_COUNT] = {};
  bool firstDone[PROV_COUNT] = {};  // ja fez a 1a consulta (do boot ou forcada)
  uint32_t wifiAt = 0, lastFirstAt = 0;
  for (;;) {
    if (!wifiAt && WiFi.status() == WL_CONNECTED) {
      uint32_t now = millis();
      wifiAt = now ? now : 1;
    }
    loginStep();  // o Spotify tem task propria ("spotify", spotify_esp32.cpp)
    if (refreshRequested) {
      refreshRequested = false;
      Guard g;
      for (int i = 0; i < PROV_COUNT; i++) {
        slots[i].nextAt = 0;
        forced[i] = true;
      }
    }
    for (int i = 0; i < PROV_COUNT; i++) {
      Provider p = (Provider)i;
      String token;
      uint32_t nextAt;
      ProviderUsage u;
      {
        Guard g;
        token = slots[p].token;
        nextAt = slots[p].nextAt;
        u = slots[p].u;
      }
      if (token.isEmpty() || !providerWanted(p)) continue;
      if (nextAt && (int32_t)(millis() - nextAt) < 0) continue;
      if (!ready()) {  // a interface ja mostra Waiting/Cache
        Guard g;
        slots[p].refreshing = false;  // nunca fica preso sem Wi-Fi/NTP
        continue;
      }
      if (!forced[p] && !firstDone[p]) {
        // Boot: o Spotify busca o token antes (heap ainda inteiro); depois os provedores um a cada 15 s.
        if (!spotify_boot_done() && (int32_t)(millis() - wifiAt) < (int32_t)BOOT_WAIT_MS) continue;
        if (lastFirstAt && (int32_t)(millis() - lastFirstAt) < (int32_t)FIRST_SPACING_MS) continue;
      }
      if (p == PROV_CLAUDE && !forced[p] && claudeWindowClosed(u)) continue;
      if (!forced[p]) {
        uint32_t now = millis();
        lastFirstAt = now ? now : 1;
      }
      firstDone[p] = true;
      forced[p] = false;
      if (p != PROV_CLAUDE) token = String();  // nao segurar o refresh token (~1 KB no Codex) durante a TLS
      https_take_memcert_fail();  // limpa qualquer resto antes desta consulta
      uint32_t delayMs = p == PROV_CLAUDE ? fetchClaude(token) : p == PROV_CODEX ? fetchCodex() : fetchAntigravity();
      if (https_take_memcert_fail() && delayMs < MEMFAIL_RETRY_MS) {
        // X509 FATAL / falta de memoria: repetir logo da o mesmo resultado e so ocupa a trava.
        board_log("[usage] %d: falha de memoria/certificado, proxima consulta em %u s", i, (unsigned)(MEMFAIL_RETRY_MS / 1000));
        delayMs = MEMFAIL_RETRY_MS;
      }
      Guard g;
      slots[p].nextAt = millis() + delayMs;
      slots[p].refreshing = false;
      // Escalona os provedores: cada TLS consome ~42 KB, nunca dois juntos.
    }
    // Folga minima da pilha (log so quando piora): base para reduzir TASK_STACK com seguranca.
    static UBaseType_t minFreeWords = ~0u;
    UBaseType_t hw = uxTaskGetStackHighWaterMark(nullptr);
    if (hw < minFreeWords) {
      minFreeWords = hw;
      board_log("[usage] pilha livre min %u B de %u", (unsigned)hw, (unsigned)TASK_STACK);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ---- API ---------------------------------------------------------------------

static void resetSlot(Provider p) {
  ProviderUsage u = newUsage();
  if (slots[p].token.isEmpty()) u.source = UsageSource::NoAccount;
  else u.source = loadCache(p, u) ? UsageSource::Cache : UsageSource::Waiting;
  slots[p].u = u;
  slots[p].nextAt = 0;
}

void usage_begin() {
  Preferences prefs;
  prefs.begin(NVS_ACCOUNTS, true);
  for (int i = 0; i < PROV_COUNT; i++) slots[i].token = prefs.getString(key((Provider)i), "");
  // O access token do Codex (JWT, ~2 KB) e gravado durante a 1a TLS, dentro do buraco que a reserva
  // deixa livre, e la ficaria para sempre encolhendo o maior bloco. Capacidade fixa ja no boot (antes
  // de https_reserve_init): as atribuicoes seguintes reaproveitam o buffer e nada se move.
  slots[PROV_CODEX].access.reserve(3072);
  slots[PROV_ANTIGRAVITY].access.reserve(768);
  slots[PROV_CODEX].access = prefs.getString("codex_at", "").c_str();  // copia (nao move): mantem a capacidade
  slots[PROV_CODEX].accessExp = prefs.getUInt("codex_exp", 0);
  slots[PROV_CODEX].accountId = prefs.getString("codex_id", "");
  slots[PROV_ANTIGRAVITY].accountId = prefs.getString("agy_proj", "");
  prefs.end();
  for (int i = 0; i < PROV_COUNT; i++) resetSlot((Provider)i);
  // Nucleo 0 (o do Wi-Fi): a TLS segurava o nucleo do loop e a placa atrasava as respostas pelo USB.
  // Prioridade da idle (0), nao 1: no nucleo 0 a idle e vigiada pelo watchdog e a TLS passa de 5 s
  // sem ceder; na mesma prioridade o escalonador reparte a CPU em fatias e a idle volta a rodar.
  xTaskCreatePinnedToCore(task, "usage", TASK_STACK, nullptr, tskIDLE_PRIORITY, nullptr, 0);
}

ProviderUsage usage_get(Provider p) { return snapshot(p); }

void usage_refresh_now() {
  Guard g;
  for (int i = 0; i < PROV_COUNT; i++)
    if (!slots[i].token.isEmpty()) slots[i].refreshing = true;
  refreshRequested = true;
}

bool usage_is_refreshing(Provider p) {
  Guard g;
  return slots[p].refreshing;
}

bool usage_has_account(Provider p) {
  Guard g;
  return !slots[p].token.isEmpty();
}

bool usage_set_account(Provider p, const char *token) {
  // O terminal quebra tokens longos em varias linhas: ao copiar, vem quebra de
  // linha/espaco no meio. Os tokens so tem letras, numeros, '-', '_', '/' e '.':
  // todo o resto e descartado.
  String t;
  unsigned removed = 0;
  for (const char *c = token; *c; c++) {
    if (isalnum((unsigned char)*c) || *c == '-' || *c == '_' || *c == '/' || *c == '.') t += *c;
    else removed++;
  }
  board_log("contas: token recebido com %u caracteres (%u descartados), inicio %.8s", (unsigned)t.length(), removed,
            t.c_str());
  bool valid = (p == PROV_CLAUDE && t.startsWith("sk-ant-") && t.length() >= 20 && t.length() <= 400) ||
               (p == PROV_ANTIGRAVITY && t.startsWith("1//") && t.length() >= 40 && t.length() <= 400);
  if (!valid) return false;
  nvsPut(NVS_ACCOUNTS, key(p), t);
  nvsDel(NVS_CACHE, key(p));  // o cache era de outra conta (ou de nenhuma)
  if (p == PROV_ANTIGRAVITY) nvsDel(NVS_ACCOUNTS, "agy_proj");
  Guard g;
  slots[p].token = t;
  if (p == PROV_ANTIGRAVITY) slots[p].accountId = "";
  slots[p].access = "";
  slots[p].accessExp = 0;
  resetSlot(p);
  return true;
}

void usage_forget_account(Provider p) {
  nvsDel(NVS_ACCOUNTS, key(p));
  nvsDel(NVS_CACHE, key(p));
  if (p == PROV_ANTIGRAVITY) nvsDel(NVS_ACCOUNTS, "agy_proj");
  if (p == PROV_CODEX) {
    nvsDel(NVS_ACCOUNTS, "codex_at");
    nvsDel(NVS_ACCOUNTS, "codex_id");
    nvsDel(NVS_ACCOUNTS, "codex_exp");
  }
  Guard g;
  slots[p].token = "";
  slots[p].access = "";
  slots[p].accessExp = 0;
  slots[p].accountId = "";
  resetSlot(p);
}
