// Gerenciador de Wi-Fi da placa. Tudo nao bloqueante: wifi_loop() roda no loop
// do Arduino, junto com o LVGL, entao nada aqui pode esperar segundos.

#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <time.h>

#include "board.h"
#include "https_client.h"
#include "wifi_mgr.h"

static const char *NVS_NS = "wifi";
static const char *TZ_INFO = "<-03>3";  // Brasilia, sem horario de verao
static constexpr uint32_t CONNECT_TIMEOUT_MS = 12000;
static constexpr uint32_t BEGIN_SETTLE_MS = 300;
// Logo apos o begin(), WiFi.status() ainda pode trazer a falha da tentativa ou do
// modo anterior (visto ao sair do portal): so aceita falha depois desse tempo.
static constexpr uint32_t CONNECT_GRACE_MS = 2000;
static constexpr uint32_t PORTAL_TIMEOUT_MS = 5 * 60 * 1000;
static constexpr uint32_t PORTAL_CLOSE_AFTER_SAVE_MS = 3000;
static constexpr int MIN_RSSI = -88;  // abaixo disso nem tenta
static const uint32_t BACKOFF_S[] = {30, 60, 300};

struct SavedNet {
  String ssid, pass;
};
static SavedNet saved[WIFI_MAX_SAVED];
static int savedCount = 0;

static WifiState state = WifiState::Off;
static uint32_t stateSince = 0;
static int candidates[WIFI_MAX_SAVED];
static int candCount = 0, candPos = 0;
static uint32_t retryAt = 0;  // 0 = sem nova tentativa agendada
static uint32_t beginAt = 0;  // != 0: WiFi.begin() agendado para esse instante
// Cada rede ganha 2 tentativas: a primeira associacao as vezes nao sai (visto com
// roteador dual-band de nome unico), e a segunda logo em seguida costuma conectar.
static constexpr int ATTEMPTS_PER_NET = 2;
static int attempt = 0;
static int backoffIdx = 0;
static String curSsid;
static bool ntpStarted = false;
static constexpr uint32_t NET_WATCH_MS = 5 * 60 * 1000;
static constexpr uint32_t NET_WATCH_FAILS = 3;
static bool netWatchFired = false;  // ja reconectou uma vez sem voltar a ter TLS ok

static DNSServer *dns = nullptr;
static WebServer *web = nullptr;
static char apSsid[20], apPass[12];
static uint32_t portalCloseAt = 0;
static bool portalSavedNew = false;

// ---- redes salvas (NVS) ------------------------------------------------------

static void loadSaved() {
  Preferences p;
  p.begin(NVS_NS, true);
  savedCount = min<int>(p.getUChar("count", 0), WIFI_MAX_SAVED);
  for (int i = 0; i < savedCount; i++) {
    char k[4];
    snprintf(k, sizeof(k), "s%d", i);
    saved[i].ssid = p.getString(k, "");
    snprintf(k, sizeof(k), "p%d", i);
    saved[i].pass = p.getString(k, "");
  }
  p.end();
}

static void storeSaved() {
  Preferences p;
  p.begin(NVS_NS, false);
  p.clear();
  p.putUChar("count", savedCount);
  for (int i = 0; i < savedCount; i++) {
    char k[4];
    snprintf(k, sizeof(k), "s%d", i);
    p.putString(k, saved[i].ssid);
    snprintf(k, sizeof(k), "p%d", i);
    p.putString(k, saved[i].pass);
  }
  p.end();
}

// Rede nova (ou senha nova) entra no topo da lista; a mais antiga sai se lotar.
static void addNetwork(const String &ssid, const String &pass) {
  for (int i = 0; i < savedCount; i++) {
    if (saved[i].ssid == ssid) {
      for (int j = i; j < savedCount - 1; j++) saved[j] = saved[j + 1];
      savedCount--;
      break;
    }
  }
  if (savedCount == WIFI_MAX_SAVED) savedCount--;
  for (int j = savedCount; j > 0; j--) saved[j] = saved[j - 1];
  saved[0] = {ssid, pass};
  savedCount++;
  storeSaved();
}

// ---- maquina de estados ------------------------------------------------------

static void setState(WifiState s) {
  state = s;
  stateSince = millis();
}

static void goOffline() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  if (savedCount == 0) {
    retryAt = 0;
  } else {
    uint32_t s = BACKOFF_S[min<int>(backoffIdx, 2)];
    retryAt = millis() + s * 1000;
    backoffIdx++;
    board_log("wifi: offline, nova tentativa em %u s", s);
  }
  setState(WifiState::Offline);
}

static constexpr uint32_t SCAN_SETTLE_MS = 300;   // radio recem-ligado recusa varredura
static constexpr uint32_t SCAN_RETRY_MS = 1500;
static int scanRetries = 0;
static uint32_t scanStartAt = 0;  // != 0: varredura agendada para esse instante
static uint32_t scanBegan = 0;

static void startScan() {
  if (savedCount == 0) {
    goOffline();
    return;
  }
  WiFi.mode(WIFI_STA);
  if (WiFi.status() == WL_CONNECTED) WiFi.disconnect();
  scanStartAt = millis() + SCAN_SETTLE_MS;
  setState(WifiState::Scanning);
}

static void tryCandidate(int pos) {
  candPos = pos;
  if (pos >= candCount) {
    goOffline();
    return;
  }
  attempt = 0;
  curSsid = saved[candidates[pos]].ssid;
  board_log("wifi: conectando a %s", curSsid.c_str());
  WiFi.mode(WIFI_STA);
  // O begin() sai no wifi_loop, depois de o radio assentar (mesmo motivo da varredura).
  beginAt = millis() + BEGIN_SETTLE_MS;
  setState(WifiState::Connecting);
}

// found: numero de redes, ou WIFI_SCAN_FAILED (-2) se a varredura nem comecou.
static void onScanDone(int found) {
  board_log("wifi: varredura = %d em %u ms", found, (unsigned)(millis() - scanBegan));
  if (found <= 0 && scanRetries < 2) {
    scanRetries++;
    WiFi.scanDelete();
    scanStartAt = millis() + SCAN_RETRY_MS;
    return;
  }
  scanRetries = 0;

  candCount = 0;
  for (int i = 0; i < savedCount; i++) {
    for (int j = 0; j < found; j++) {
      if (WiFi.SSID(j) == saved[i].ssid && WiFi.RSSI(j) >= MIN_RSSI) {
        candidates[candCount++] = i;
        break;
      }
    }
  }
  WiFi.scanDelete();
  board_log("wifi: %d redes visiveis, %d salvas entre elas", max(found, 0), candCount);
  // Nenhuma salva apareceu (varredura falha ou rede oculta): tenta todas direto,
  // na ordem de prioridade, antes de ficar offline.
  if (candCount == 0) {
    for (int i = 0; i < savedCount; i++) candidates[candCount++] = i;
    board_log("wifi: tentando as salvas direto");
  }
  tryCandidate(0);
}

static void onConnected() {
  setState(WifiState::Connected);
  backoffIdx = 0;
  retryAt = 0;
  board_log("wifi: conectado a %s, IP %s", curSsid.c_str(), WiFi.localIP().toString().c_str());
  if (!ntpStarted) {
    configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
    ntpStarted = true;
  }
}

// ---- portal ------------------------------------------------------------------

static String htmlEscape(const String &s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

static const char PAGE_HEAD[] PROGMEM =
    "<!doctype html><html lang=pt-BR><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Painel - Wi-Fi</title><style>"
    "body{font-family:system-ui,sans-serif;margin:16px;background:#101418;color:#eee}"
    "label{display:block;padding:12px;border:1px solid #333;border-radius:10px;margin:8px 0}"
    "input[type=text],input[type=password]{width:100%;padding:12px;font-size:16px;"
    "box-sizing:border-box;margin:8px 0;border-radius:8px;border:1px solid #444;background:#1e2329;color:#eee}"
    "button{width:100%;padding:14px;font-size:17px;background:#2196f3;color:#fff;border:0;border-radius:10px}"
    "small{color:#999}a{color:#64b5f6}</style>";

static void handleRoot() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {  // a varredura da abertura do portal nao pegou: tenta de novo
    WiFi.scanNetworks(true);
    n = WIFI_SCAN_RUNNING;
  }
  String html = FPSTR(PAGE_HEAD);
  if (n == WIFI_SCAN_RUNNING) {
    html += "<meta http-equiv=refresh content=3><h2>Procurando redes...</h2>";
    web->send(200, "text/html", html);
    return;
  }
  html += "<h2>Adicionar Wi-Fi ao painel</h2><form method=post action=/save>";
  // Lista sem repetir nome (a mesma rede aparece varias vezes em mesh/repetidor).
  for (int i = 0; i < max(n, 0); i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    bool dup = false;
    for (int j = 0; j < i && !dup; j++) dup = WiFi.SSID(j) == ssid;
    if (dup) continue;
    String esc = htmlEscape(ssid);
    html += "<label><input type=radio name=ssid value=\"" + esc + "\"> " + esc + " <small>" +
            String(WiFi.RSSI(i)) + " dBm" +
            (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? ", aberta" : "") + "</small></label>";
  }
  html += "<label><input type=radio name=ssid value=''> Outra rede:"
          "<input type=text name=other placeholder='Nome da rede' maxlength=32></label>"
          "<input type=password name=pass placeholder='Senha (vazio se for aberta)' maxlength=63>"
          "<button>Salvar e conectar</button></form>"
          "<p><small>So funciona com redes 2,4 GHz. <a href=/scan>Procurar de novo</a></small></p>";
  web->send(200, "text/html", html);
}

static void handleScan() {
  WiFi.scanNetworks(true);
  web->sendHeader("Location", "/", true);
  web->send(302, "text/plain", "");
}

static void handleSave() {
  String ssid = web->arg("ssid");
  if (ssid.isEmpty()) ssid = web->arg("other");
  ssid.trim();
  String pass = web->arg("pass");
  String html = FPSTR(PAGE_HEAD);
  if (ssid.isEmpty() || ssid.length() > 32 || (pass.length() > 0 && pass.length() < 8) ||
      pass.length() > 63) {
    html += "<h2>Confira os dados</h2><p>Escolha uma rede. A senha precisa ter de 8 a 63 "
            "caracteres (ou ficar vazia, se a rede for aberta).</p><p><a href=/>Voltar</a></p>";
    web->send(200, "text/html", html);
    return;
  }
  addNetwork(ssid, pass);
  html += "<h2>Salvo!</h2><p>O painel vai se conectar a <b>" + htmlEscape(ssid) +
          "</b>.</p><p>Pode voltar o celular para o Wi-Fi de sempre.</p>";
  web->send(200, "text/html", html);
  portalSavedNew = true;
  portalCloseAt = millis() + PORTAL_CLOSE_AFTER_SAVE_MS;
}

// Qualquer outro endereco (testes de "portal cativo" do Android/iOS/Windows)
// vai para a pagina: e isso que faz o celular abrir o portal sozinho.
static void handleOther() {
  web->sendHeader("Location", "http://192.168.4.1/", true);
  web->send(302, "text/plain", "");
}

void wifi_portal_start() {
  if (state == WifiState::Portal) return;
  uint64_t mac = ESP.getEfuseMac();
  snprintf(apSsid, sizeof(apSsid), "Painel-%02X%02X", (unsigned)((mac >> 32) & 0xFF),
           (unsigned)((mac >> 40) & 0xFF));
  for (int i = 0; i < 8; i++) apPass[i] = '0' + esp_random() % 10;
  apPass[8] = 0;

  // AP_STA mantem a conexao atual (se houver) enquanto o portal esta aberto.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid, apPass);
  dns = new DNSServer();
  dns->setErrorReplyCode(DNSReplyCode::NoError);
  dns->start(53, "*", WiFi.softAPIP());
  web = new WebServer(80);
  web->on("/", HTTP_GET, handleRoot);
  web->on("/scan", HTTP_GET, handleScan);
  web->on("/save", HTTP_POST, handleSave);
  web->onNotFound(handleOther);
  web->begin();
  WiFi.scanNetworks(true);

  portalSavedNew = false;
  portalCloseAt = millis() + PORTAL_TIMEOUT_MS;
  setState(WifiState::Portal);
  board_log("wifi: portal aberto em %s", apSsid);
}

void wifi_portal_stop() {
  if (state != WifiState::Portal) return;
  web->stop();
  delete web;
  web = nullptr;
  dns->stop();
  delete dns;
  dns = nullptr;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  board_log("wifi: portal fechado");

  // Sai do estado Portal antes de tudo: wifi_connect_saved() ignora pedidos
  // durante o portal, e o wifi_loop() usaria dns/web ja apagados.
  setState(WifiState::Off);
  backoffIdx = 0;
  if (portalSavedNew) {
    wifi_connect_saved(0);
  } else if (WiFi.status() == WL_CONNECTED) {
    setState(WifiState::Connected);
  } else {
    startScan();
  }
}

// ---- API ---------------------------------------------------------------------

void wifi_begin() {
  WiFi.persistent(false);  // as credenciais ficam so na nossa lista, nao no NVS do core
  WiFi.setAutoReconnect(false);
  WiFi.setHostname("painel-cyd");
  // Apaga a rede que o proprio ESP-IDF possa ter guardado (ex.: firmware de fabrica):
  // com ela, o radio tenta conectar sozinho ao ligar e recusa a varredura.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);
  loadSaved();
  board_log("wifi: %d redes salvas", savedCount);
  startScan();
}

void wifi_loop() {
  switch (state) {
    case WifiState::Scanning: {
      if (scanStartAt) {
        if ((int32_t)(millis() - scanStartAt) < 0) break;
        scanStartAt = 0;
        scanBegan = millis();
        if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) onScanDone(WIFI_SCAN_FAILED);
        break;
      }
      int n = WiFi.scanComplete();
      if (n != WIFI_SCAN_RUNNING) onScanDone(n);
      break;
    }
    case WifiState::Connecting: {
      if (beginAt) {
        if ((int32_t)(millis() - beginAt) < 0) break;
        beginAt = 0;
        const SavedNet &n = saved[candidates[candPos]];
        WiFi.begin(n.ssid.c_str(), n.pass.c_str());
        stateSince = millis();
        break;
      }
      wl_status_t st = WiFi.status();
      uint32_t elapsed = millis() - stateSince;
      bool failed = (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) && elapsed > CONNECT_GRACE_MS;
      if (st == WL_CONNECTED) {
        onConnected();
      } else if (failed || elapsed > CONNECT_TIMEOUT_MS) {
        board_log("wifi: falhou em %s (status %d, %u ms)", curSsid.c_str(), (int)st, (unsigned)elapsed);
        WiFi.disconnect();
        if (++attempt < ATTEMPTS_PER_NET) {
          board_log("wifi: tentando %s de novo", curSsid.c_str());
          beginAt = millis() + BEGIN_SETTLE_MS;
        } else {
          tryCandidate(candPos + 1);
        }
      }
      break;
    }
    case WifiState::Connected: {
      if (WiFi.status() != WL_CONNECTED) {
        board_log("wifi: conexao caiu");
        startScan();
        break;
      }
      // Vigia: Wi-Fi "conectado" mas a rede nao serve (visto >1 h sem TLS). Referencia = ultimo sucesso ou, se
      // nao houve desde a conexao, o instante da conexao. So age com >= 3 falhas nao-memoria (ocioso nao conta).
      uint32_t ref = stateSince, okAt = https_last_ok_ms();
      if (okAt && (int32_t)(okAt - ref) > 0) {
        ref = okAt;
        netWatchFired = false;
      }
      if ((int32_t)(millis() - ref) > (int32_t)NET_WATCH_MS && https_nonmem_fails() >= NET_WATCH_FAILS) {
        https_net_fails_reset();
        if (netWatchFired) {
          board_log("wifi: sem trafego de novo apos reconectar, reiniciando");
          board_restart();
        }
        netWatchFired = true;
        board_log("wifi: sem trafego ha 5 min, reconectando");
        startScan();  // disconnect + varredura + conexao pelo caminho normal
      }
      break;
    }
    case WifiState::Offline:
      if (retryAt && (int32_t)(millis() - retryAt) >= 0) startScan();
      break;
    case WifiState::Portal:
      dns->processNextRequest();
      web->handleClient();
      if ((int32_t)(millis() - portalCloseAt) >= 0) wifi_portal_stop();
      break;
    case WifiState::Off:
      break;
  }
}

WifiStatus wifi_status() {
  WifiStatus s = {};
  s.state = state;
  strlcpy(s.ssid, curSsid.c_str(), sizeof(s.ssid));
  if (state == WifiState::Connected) {
    s.rssi = WiFi.RSSI();
    strlcpy(s.ip, WiFi.localIP().toString().c_str(), sizeof(s.ip));
  }
  if (state == WifiState::Offline && retryAt) {
    int32_t left = (int32_t)(retryAt - millis());
    s.retry_in_s = left > 0 ? (left + 999) / 1000 : 0;
  }
  s.saved_count = savedCount;
  strlcpy(s.ap_ssid, apSsid, sizeof(s.ap_ssid));
  strlcpy(s.ap_pass, apPass, sizeof(s.ap_pass));
  return s;
}

bool wifi_saved_name(int idx, char *out, int out_size) {
  if (idx < 0 || idx >= savedCount) return false;
  strlcpy(out, saved[idx].ssid.c_str(), out_size);
  return true;
}

bool wifi_add_network(const char *ssid, const char *pass) {
  size_t sl = ssid ? strlen(ssid) : 0, pl = pass ? strlen(pass) : 0;
  if (sl == 0 || sl > 32 || (pl > 0 && pl < 8) || pl > 63) return false;
  addNetwork(ssid, pass ? pass : "");
  portalSavedNew = false;
  wifi_connect_now();
  return true;
}

void wifi_connect_now() {
  if (state == WifiState::Portal) return;
  backoffIdx = 0;
  startScan();
}

void wifi_connect_saved(int idx) {
  if (idx < 0 || idx >= savedCount || state == WifiState::Portal) return;
  WiFi.disconnect();
  candidates[0] = idx;
  candCount = 1;
  backoffIdx = 0;
  tryCandidate(0);
}

void wifi_forget(int idx) {
  if (idx < 0 || idx >= savedCount) return;
  bool wasCurrent = saved[idx].ssid == curSsid && state == WifiState::Connected;
  for (int j = idx; j < savedCount - 1; j++) saved[j] = saved[j + 1];
  savedCount--;
  storeSaved();
  if (wasCurrent) {
    curSsid = "";
    startScan();
  }
}

void wifi_move_up(int idx) {
  if (idx <= 0 || idx >= savedCount) return;
  SavedNet t = saved[idx - 1];
  saved[idx - 1] = saved[idx];
  saved[idx] = t;
  storeSaved();
}
