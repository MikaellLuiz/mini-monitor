// Enlace USB com o programa do PC (ver include/usb_link.h).

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "auth.h"
#include "board.h"
#include "pcagent.h"
#include "usb_link.h"
#include "wifi_mgr.h"

static constexpr const char *PREFIX = "@PC ";
static constexpr int PREFIX_LEN = 4;
static constexpr int USB_LINE_MAX = 640;
static constexpr int READ_PER_LOOP = 384;  // nao segura o LVGL lendo demais de uma vez
static constexpr const char *FW_VERSION = "0.9.0";

static char line[USB_LINE_MAX];
static int lineLen = 0;
static bool skipping = false;  // linha longa demais: descarta ate o \n

static bool authed = false;
static uint32_t lastRx = 0;
static char sessionName[AUTH_NAME_MAX] = "";

void usb_link_begin() {}

bool usb_link_authed() { return authed; }

// Uma escrita so por linha: a UART segura o bloqueio durante o write, entao um log
// de outra task nao entra no meio.
static void sendDoc(JsonDocument &doc) {
  // serializeJson(doc, String) substitui o conteudo da String (nao acrescenta): o prefixo entra depois.
  String body;
  serializeJson(doc, body);
  String out = PREFIX;
  out += body;
  out += '\n';
  Serial.write((const uint8_t *)out.c_str(), out.length());
}

static void reply(int id, bool ok, const char *err = nullptr) {
  if (id < 0) return;
  JsonDocument r;
  r["id"] = id;
  r["ok"] = ok;
  if (!ok && err) r["err"] = err;
  sendDoc(r);
}

bool usb_link_send_action(int idx) {
  if (!authed) return false;
  JsonDocument r;
  r["evt"] = "action";
  r["idx"] = idx;
  sendDoc(r);
  return true;
}

// ---- comandos ----------------------------------------------------------------

static const char *wifiStateName(WifiState s) {
  switch (s) {
    case WifiState::Off: return "off";
    case WifiState::Scanning: return "scanning";
    case WifiState::Connecting: return "connecting";
    case WifiState::Connected: return "connected";
    case WifiState::Offline: return "offline";
    case WifiState::Portal: return "portal";
  }
  return "?";
}

static void cmdHello(int id) {
  JsonDocument r;
  r["id"] = id;
  r["ok"] = true;
  r["proto"] = USB_PROTOCOL;
  r["fw"] = FW_VERSION;
  r["has_pin"] = auth_has_pin();
  r["locked_s"] = auth_lock_remaining_s();
  r["authed"] = authed;
  if (authed) r["client"] = sessionName;
  sendDoc(r);
}

static void startSession(const char *name) {
  authed = true;
  snprintf(sessionName, sizeof(sessionName), "%s", name);
  board_log("usb: sessao de %s", name);
}

// Resposta de um PIN que nao serviu.
static void replyPinFailure(int id, AuthResult res, uint32_t retry) {
  JsonDocument r;
  r["id"] = id;
  r["ok"] = false;
  r["err"] = res == AuthResult::Locked ? "locked" : res == AuthResult::Wrong ? "pin" : "invalid";
  r["retry_s"] = retry;
  sendDoc(r);
}

static void replyWithNewToken(int id, const char *name) {
  JsonDocument r;
  r["id"] = id;
  char token[AUTH_TOKEN_CHARS + 1];
  if (!auth_issue_client(name, token)) {
    r["ok"] = false;
    r["err"] = auth_client_count() >= AUTH_MAX_CLIENTS ? "clients_full" : "name";
    sendDoc(r);
    return;
  }
  startSession(name);
  r["ok"] = true;
  r["token"] = token;
  sendDoc(r);
}

static void cmdAuth(int id, JsonObjectConst c) {
  const char *token = c["token"] | "";
  const char *pin = c["pin"] | "";
  const char *name = c["name"] | "";
  if (token[0]) {
    char who[AUTH_NAME_MAX];
    if (!auth_check_token(token, who, sizeof(who))) {
      reply(id, false, "token");
      return;
    }
    startSession(who);
    JsonDocument r;
    r["id"] = id;
    r["ok"] = true;
    r["client"] = who;
    sendDoc(r);
    return;
  }
  if (!auth_has_pin()) {
    reply(id, false, "no_pin");
    return;
  }
  uint32_t retry = 0;
  AuthResult res = auth_check_pin(pin, &retry);
  if (res != AuthResult::Ok) {
    replyPinFailure(id, res, retry);
    return;
  }
  if (!name[0]) {  // PIN certo sem nome: abre a sessao, mas nao registra o PC
    startSession("pin");
    reply(id, true);
    return;
  }
  replyWithNewToken(id, name);
}

static void cmdPinCreate(int id, JsonObjectConst c) {
  if (auth_has_pin()) {
    reply(id, false, "has_pin");
    return;
  }
  if (!auth_create_pin(c["pin"] | "")) {
    reply(id, false, "invalid");
    return;
  }
  replyWithNewToken(id, c["name"] | "PC");
}

static void cmdStatus(int id) {
  JsonDocument r;
  r["id"] = id;
  r["ok"] = true;
  r["fw"] = FW_VERSION;
  r["bright"] = board_get_brightness();
  WifiStatus w = wifi_status();
  JsonObject wf = r["wifi"].to<JsonObject>();
  wf["state"] = wifiStateName(w.state);
  wf["ssid"] = w.ssid;
  wf["ip"] = w.ip;
  wf["rssi"] = w.rssi;
  wf["saved"] = w.saved_count;
  PcMetrics m = pc_get();
  r["source"] = m.source == PC_SRC_USB ? "usb" : m.source == PC_SRC_WIFI ? "wifi" : "none";
  r["online"] = m.online;
  r["active"] = m.name;
  char pin[PC_NAME_MAX];
  r["pinned"] = pc_pinned_name(pin, sizeof(pin)) ? pin : "";
  JsonArray pcs = r["pcs"].to<JsonArray>();
  for (int i = 0; i < pc_choice_count(); i++) {
    char n[PC_NAME_MAX];
    uint8_t mask = 0;
    if (!pc_choice_name(i, n, sizeof(n), &mask)) break;
    JsonObject o = pcs.add<JsonObject>();
    o["name"] = String(n);  // copia: n some ao fim da volta
    o["usb"] = (mask & 1) != 0;
    o["wifi"] = (mask & 2) != 0;
  }
  JsonArray cl = r["clients"].to<JsonArray>();
  for (int i = 0; i < auth_client_count(); i++) {
    char n[AUTH_NAME_MAX];
    if (auth_client_name(i, n, sizeof(n))) cl.add(String(n));
  }
  board_heap_t h = board_heap();
  r["heap"] = h.free_bytes;
  sendDoc(r);
}

static void cmdWifiSet(int id, JsonObjectConst c) {
  String ssid = c["ssid"] | "";
  String pass = c["pass"] | "";
  ssid.trim();
  if (ssid.isEmpty() || ssid.length() > 32 || (pass.length() > 0 && pass.length() < 8) || pass.length() > 63) {
    reply(id, false, "wifi_invalid");
    return;
  }
  reply(id, wifi_add_network(ssid.c_str(), pass.c_str()), "wifi");
}

static void cmdMetrics(int id, JsonObjectConst c) {
  const char *name = c["name"] | "";
  float temp = c["temp"] | -1.0f;
  pc_usb_feed(name[0] ? name : sessionName, c["cpu"] | -1.0f, c["ram"] | -1.0f, c["gpu"] | -1.0f, c["vram"] | -1.0f,
              temp >= 0 ? (int)(temp + 0.5f) : -1);
  reply(id, true);
}

static void cmdShortcuts(int id, JsonObjectConst c) {
  const char *labels[PC_MAX_SHORTCUTS];
  int n = 0;
  for (JsonVariantConst v : c["labels"].as<JsonArrayConst>()) {
    if (n >= PC_MAX_SHORTCUTS) break;
    labels[n++] = v.as<const char *>() ? v.as<const char *>() : "";
  }
  pc_usb_shortcuts(labels, n);
  reply(id, true);
}

static void cmdReset(int id, JsonObjectConst c) {
  // Mesmo com a sessao aberta, pede o PIN de novo: apaga tudo e nao tem volta.
  uint32_t retry = 0;
  AuthResult res = auth_check_pin(c["pin"] | "", &retry);
  if (res != AuthResult::Ok) {
    replyPinFailure(id, res, retry);
    return;
  }
  reply(id, true);
  Serial.flush();
  board_factory_reset();
}

static void handle(JsonObjectConst c) {
  const char *cmd = c["cmd"] | "";
  int id = c["id"].is<int>() ? c["id"].as<int>() : -1;

  if (!strcmp(cmd, "hello")) return cmdHello(id);
  if (!strcmp(cmd, "auth")) return cmdAuth(id, c);
  if (!strcmp(cmd, "pin_create")) return cmdPinCreate(id, c);
  if (!authed) return reply(id, false, "auth");

  if (!strcmp(cmd, "status")) return cmdStatus(id);
  if (!strcmp(cmd, "metrics")) return cmdMetrics(id, c);
  if (!strcmp(cmd, "shortcuts")) return cmdShortcuts(id, c);
  if (!strcmp(cmd, "wifi_set")) return cmdWifiSet(id, c);
  if (!strcmp(cmd, "reset")) return cmdReset(id, c);
  if (!strcmp(cmd, "brightness")) {
    int v = c["v"] | -1;
    if (v < 10 || v > 255) return reply(id, false, "range");
    board_set_brightness((uint8_t)v);
    settings_set_u8("bright", (uint8_t)v);
    return reply(id, true);
  }
  if (!strcmp(cmd, "pc_select")) {
    const char *n = c["name"] | "";
    pc_select(!strcmp(n, "auto") ? "" : n);
    return reply(id, true);
  }
  if (!strcmp(cmd, "revoke")) return reply(id, auth_revoke_client(c["name"] | ""), "not_found");
  if (!strcmp(cmd, "restart")) {
    reply(id, true);
    Serial.flush();
    delay(100);
    return board_restart();
  }
  if (!strcmp(cmd, "bye")) {
    authed = false;
    return reply(id, true);
  }
  reply(id, false, "unknown");
}

static void handleLine(char *text) {
  if (strncmp(text, PREFIX, PREFIX_LEN)) return;  // log ou lixo: ignora
  JsonDocument doc;
  if (deserializeJson(doc, text + PREFIX_LEN) || !doc.is<JsonObject>()) {
    JsonDocument r;
    r["ok"] = false;
    r["err"] = "json";
    sendDoc(r);
    return;
  }
  lastRx = millis();
  handle(doc.as<JsonObjectConst>());
}

void usb_link_loop() {
  if (authed && (uint32_t)(millis() - lastRx) > USB_SESSION_TIMEOUT_MS) {
    authed = false;
    board_log("usb: sessao expirou");
  }
  for (int n = 0; n < READ_PER_LOOP && Serial.available(); n++) {
    int ch = Serial.read();
    if (ch < 0) break;
    if (ch == '\n') {
      if (!skipping && lineLen > 0) {
        if (line[lineLen - 1] == '\r') lineLen--;
        line[lineLen] = 0;
        handleLine(line);
      }
      lineLen = 0;
      skipping = false;
    } else if (!skipping) {
      if (lineLen < USB_LINE_MAX - 1) line[lineLen++] = (char)ch;
      else skipping = true;  // linha longa demais
    }
  }
}
