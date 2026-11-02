// Pagina de contas na rede local (ver include/account_portal.h). Roda no loop
// principal enquanto a tela de conta estiver aberta.

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include "account_portal.h"
#include "board.h"
#include "pcagent.h"
#include "spotify.h"
#include "usage.h"

static constexpr int MAX_WRONG_CODES = 5;  // depois disso a pagina fecha

static WebServer *web = nullptr;
static AccountPortalInfo info = {};
static int wrongCodes = 0;

static const char PAGE_HEAD[] PROGMEM =
    "<!doctype html><html lang=pt-BR><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Painel - contas</title><style>"
    "body{font-family:system-ui,sans-serif;margin:16px;max-width:560px;background:#101418;color:#eee}"
    "input,textarea{width:100%;padding:12px;font-size:16px;box-sizing:border-box;margin:6px 0 14px;"
    "border-radius:8px;border:1px solid #444;background:#1e2329;color:#eee}"
    "textarea{height:90px;font-family:monospace}"
    "button{width:100%;padding:14px;font-size:17px;background:#2196f3;color:#fff;border:0;border-radius:10px}"
    "code{background:#1e2329;padding:2px 6px;border-radius:4px}small{color:#999}a{color:#64b5f6}</style>";

static void handleRoot() {
  String html = FPSTR(PAGE_HEAD);
  html += "<h2>Contas do painel</h2><form method=post action=/save>"
          "<label>Codigo mostrado no painel<input name=code inputmode=numeric maxlength=6 "
          "autocomplete=off required></label>"
          "<h3>Claude <small>(";
  html += usage_has_account(PROV_CLAUDE) ? "configurado" : "sem conta";
  html += ")</small></h3><p>No PC rode <code>claude setup-token</code> e cole o token "
          "(<code>sk-ant-oat01-...</code>).</p>"
          "<textarea name=claude autocomplete=off spellcheck=false></textarea>"
          "<h3>Antigravity <small>(";
  html += usage_has_account(PROV_ANTIGRAVITY) ? "configurado" : "sem conta";
  html += ")</small></h3><p>Mais facil: no PC rode <code>python tools/send_antigravity.py</code>, que envia "
          "direto. Ou cole o refresh token (<code>1//...</code>).</p>"
          "<textarea name=agy autocomplete=off spellcheck=false></textarea>"
          "<button>Salvar</button></form>"
          "<p><small>Os tokens ficam guardados so no painel e sao usados apenas para consultar o seu uso. "
          "O Codex entra pelo proprio painel, com um codigo.</small></p>";
  web->send(200, "text/html", html);
}

// Pareamento do agente do PC (tools/pc_agent/agent.py pair <codigo>): o IP vem da conexao.
static void handlePair() {
  if (web->arg("code") != info.code) {
    if (++wrongCodes >= MAX_WRONG_CODES) account_portal_stop();
    web->send(403, "text/plain", "codigo errado");
    return;
  }
  String ip = web->client().remoteIP().toString();
  if (!pc_pair(web->arg("name").c_str(), web->arg("host").c_str(), ip.c_str(), web->arg("port").toInt(),
               web->arg("token").c_str())) {
    web->send(400, "text/plain", "dados invalidos");
    return;
  }
  web->send(200, "text/plain", "ok");
}

static void handleSave() {
  String html = FPSTR(PAGE_HEAD);
  if (web->arg("code") != info.code) {
    if (++wrongCodes >= MAX_WRONG_CODES) {
      html += "<h2>Codigo errado demais</h2><p>A pagina foi fechada. Abra de novo pelo painel.</p>";
      web->send(403, "text/html", html);
      board_log("contas: codigo errado %d vezes, fechando a pagina", wrongCodes);
      account_portal_stop();
      return;
    }
    html += "<h2>Codigo errado</h2><p>Confira o codigo na tela do painel.</p><p><a href=/>Voltar</a></p>";
    web->send(403, "text/html", html);
    return;
  }
  if (web->arg("spotify_rt").length()) {  // enviado por tools/spotify_login.py
    if (spotify_set_account(web->arg("spotify_client").c_str(), web->arg("spotify_rt").c_str())) {
      board_log("contas: Spotify salvo");
      html += "<h2>Spotify salvo</h2><p>Pode fechar esta pagina.</p>";
      web->send(200, "text/html", html);
    } else {
      html += "<h2>Dados do Spotify invalidos</h2>";
      web->send(400, "text/html", html);
    }
    return;
  }
  Provider which = PROV_COUNT;
  bool bad = false;
  if (web->arg("claude").length()) {
    if (usage_set_account(PROV_CLAUDE, web->arg("claude").c_str())) which = PROV_CLAUDE;
    else bad = true;
  }
  if (web->arg("agy").length()) {
    if (usage_set_account(PROV_ANTIGRAVITY, web->arg("agy").c_str())) which = PROV_ANTIGRAVITY;
    else bad = true;
  }
  if (which == PROV_COUNT || bad) {
    html += "<h2>Token nao reconhecido</h2><p>Claude comeca com <code>sk-ant-</code>; Antigravity com "
            "<code>1//</code>. Copie de novo.</p><p><a href=/>Voltar</a></p>";
    web->send(400, "text/html", html);
    return;
  }
  info.saved = true;
  info.saved_provider = which;
  info.saved_attempts = usage_get(which).attempts;
  usage_refresh_now();
  board_log("contas: token salvo (%s)", which == PROV_CLAUDE ? "Claude" : "Antigravity");
  // A pagina acompanha a primeira consulta e mostra o resultado: nao precisa
  // voltar ao painel para saber se o token funcionou.
  html += "<h2>Token salvo</h2><div id=r><p>Verificando...</p></div><script>"
          "const start=" + String(info.saved_attempts) + ";const prov='" + (which == PROV_CLAUDE ? "claude" : "agy") + "';"
          "async function poll(){try{const s=await(await fetch('/status?p='+prov)).json();"
          "if(s.attempts>start){document.getElementById('r').innerHTML=s.ok"
          "?`<h3>Funcionou!</h3><p>${s.a_name}: ${s.a}% (${s.a_reset})<br>${s.b_name}: ${s.b}% (${s.b_reset})</p>"
          "<p>Pode fechar esta pagina.</p>`"
          ":`<h3>Nao funcionou</h3><p>${s.error}</p><p><a href=/>Tentar de novo</a></p>`;return;}}catch(e){}"
          "setTimeout(poll,1500);}poll();</script>";
  web->send(200, "text/html", html);
}

static void handleStatus() {
  Provider p = web->arg("p") == "agy" ? PROV_ANTIGRAVITY : PROV_CLAUDE;
  ProviderUsage u = usage_get(p);
  char ra[16] = "", rb[16] = "";
  if (u.win[0].reset_epoch) board_format_time(u.win[0].reset_epoch, ra, sizeof(ra));
  if (u.win[1].reset_epoch) board_format_time(u.win[1].reset_epoch, rb, sizeof(rb));
  char json[300];
  snprintf(json, sizeof(json),
           "{\"attempts\":%u,\"ok\":%s,\"a_name\":\"%s\",\"a\":%d,\"a_reset\":\"%s\",\"b_name\":\"%s\",\"b\":%d,"
           "\"b_reset\":\"%s\",\"error\":\"%s\"}",
           u.attempts, u.source == UsageSource::Direct ? "true" : "false", u.win[0].name, (int)(u.win[0].pct + 0.5f), ra,
           u.win[1].name, (int)(u.win[1].pct + 0.5f), rb, u.error);
  web->send(200, "application/json", json);
}

bool account_portal_start() {
  if (web) {  // fechamento ainda pendente no loop: so reabre
    info.active = true;
    return true;
  }
  if (WiFi.status() != WL_CONNECTED) return false;
  snprintf(info.code, sizeof(info.code), "%06u", (unsigned)(esp_random() % 1000000));
  snprintf(info.url, sizeof(info.url), "http://%s", WiFi.localIP().toString().c_str());
  wrongCodes = 0;
  info.saved = false;
  web = new WebServer(80);
  web->on("/", HTTP_GET, handleRoot);
  web->on("/save", HTTP_POST, handleSave);
  web->on("/pair", HTTP_POST, handlePair);
  web->on("/status", HTTP_GET, handleStatus);
  web->onNotFound([] {
    web->sendHeader("Location", "/", true);
    web->send(302, "text/plain", "");
  });
  web->begin();
  info.active = true;
  board_log("contas: pagina aberta em %s", info.url);
  return true;
}

void account_portal_stop() {
  if (!web) return;
  // Pode ser chamado de dentro de um handler: so marca, o loop apaga.
  info.active = false;
}

AccountPortalInfo account_portal_info() { return info; }

void account_portal_loop() {
  if (!web) return;
  web->handleClient();
  if (!info.active) {
    web->stop();
    delete web;
    web = nullptr;
    board_log("contas: pagina fechada");
  }
}
