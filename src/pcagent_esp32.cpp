// Cliente do agente do PC: consulta metricas e atalhos e envia o atalho tocado.
// HTTP simples (sem TLS: nada de ~42 KB de buffers) com token Bearer por painel.
// Roda na task "pcstep" (pc_step() a cada ~100 ms), separada da task de uso.

#include <ArduinoJson.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include "board.h"
#include "pcagent.h"
#include "usb_link.h"

static const char *NVS_NS = "pcagents";
static const char *NVS_SEL_NS = "pcsel";  // separado: saveAgents() limpa o "pcagents" inteiro
static constexpr uint32_t METRICS_EVERY_MS = 2000;
static constexpr uint32_t REDISCOVER_EVERY_MS = 30000;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 1200;

struct Agent {
  String name, host, ip, token;
  uint16_t port = 0;
};

static SemaphoreHandle_t lock = xSemaphoreCreateMutex();
struct Guard {
  Guard() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(lock); }
};

static Agent agents[PC_MAX_AGENTS];
static int nAgents = 0, active = 0, failures = 0;
static PcMetrics metricsState;
static char shortcuts[PC_MAX_SHORTCUTS][13];
static int nShortcuts = 0;
static bool shortcutsLoaded = false;
static volatile bool wantMetrics = false, wantShortcuts = false;
static volatile int pendingAction = -1;
static uint32_t nextMetrics = 0, lastRediscover = 0;
static bool mdnsStarted = false;

// PC que alimenta a placa pelo cabo: ele empurra as metricas (pc_usb_feed), a placa nao consulta.
struct UsbSource {
  bool seen = false;  // ja chegou ao menos uma leitura (o nome fica para a lista de escolha)
  char name[PC_NAME_MAX] = "";
  float cpu = -1, ram = -1, gpu = -1, vram = -1;
  int temp = -1;
  uint32_t lastMs = 0;
  char shortcuts[PC_MAX_SHORTCUTS][13];
  int nShortcuts = 0;
};
static UsbSource usb;
static char pinned[PC_NAME_MAX] = "";  // "" = automatico
static PcSource lastSrc = PC_SRC_NONE;

static void sanitizeName(const char *in, char *out, int cap) {
  int n = 0;
  for (; *in && n < cap - 1; in++)
    if ((uint8_t)*in >= 32 && (uint8_t)*in < 127) out[n++] = *in;
  out[n] = 0;
}

// ---- escolha da fonte (chamar com o lock) --------------------------------------

static bool usbAliveLocked() { return usb.seen && (uint32_t)(millis() - usb.lastMs) < PC_USB_TIMEOUT_MS; }

static int findAgent(const char *name) {
  for (int i = 0; i < nAgents; i++)
    if (agents[i].name.equalsIgnoreCase(name)) return i;
  return -1;
}

static PcSource pickSource(int *wifiIdx) {
  *wifiIdx = -1;
  if (pinned[0]) {  // fixado vale sempre, mesmo offline: nao cai para o resto da cadeia
    if (usbAliveLocked() && !strcasecmp(usb.name, pinned)) return PC_SRC_USB;
    int k = findAgent(pinned);
    if (k >= 0) {
      *wifiIdx = k;
      return PC_SRC_WIFI;
    }
    return PC_SRC_NONE;
  }
  if (usbAliveLocked()) return PC_SRC_USB;
  if (nAgents > 0) {
    *wifiIdx = active < nAgents ? active : 0;
    return PC_SRC_WIFI;
  }
  return PC_SRC_NONE;
}

// ---- NVS ---------------------------------------------------------------------

static void saveAgents() {
  Preferences p;
  p.begin(NVS_NS, false);
  p.clear();
  p.putUChar("n", nAgents);
  for (int i = 0; i < nAgents; i++) {
    char k[8];
    snprintf(k, sizeof(k), "n%d", i); p.putString(k, agents[i].name);
    snprintf(k, sizeof(k), "h%d", i); p.putString(k, agents[i].host);
    snprintf(k, sizeof(k), "i%d", i); p.putString(k, agents[i].ip);
    snprintf(k, sizeof(k), "t%d", i); p.putString(k, agents[i].token);
    snprintf(k, sizeof(k), "p%d", i); p.putUShort(k, agents[i].port);
  }
  p.end();
}

static constexpr uint32_t PCSTEP_STACK = 3584;  // medido: pico 1436 B (6144 - folga 4708) + ~2 KB de folga
static constexpr uint32_t PCSTEP_EVERY_MS = 100;  // atalho pendente sai em ate ~100 ms
void pc_step();
static void pcStepTask(void *) {
  uint32_t lastLog = 0;
  UBaseType_t minFree = ~0u;
  for (;;) {
    pc_step();
    UBaseType_t hw = uxTaskGetStackHighWaterMark(nullptr);  // folga minima da pilha; no ESP-IDF ja e em BYTES
    if (hw < minFree && millis() - lastLog > 5000) {  // loga so quando a folga piora (para ajustar PCSTEP_STACK)
      minFree = hw;
      lastLog = millis();
      board_log("[pcstep] pilha livre min %u B de %u", (unsigned)hw, (unsigned)PCSTEP_STACK);
    }
    vTaskDelay(pdMS_TO_TICKS(PCSTEP_EVERY_MS));
  }
}

void pc_begin() {
  Preferences p;
  p.begin(NVS_NS, true);
  nAgents = min<int>(p.getUChar("n", 0), PC_MAX_AGENTS);
  for (int i = 0; i < nAgents; i++) {
    char k[8];
    snprintf(k, sizeof(k), "n%d", i); agents[i].name = p.getString(k, "");
    snprintf(k, sizeof(k), "h%d", i); agents[i].host = p.getString(k, "");
    snprintf(k, sizeof(k), "i%d", i); agents[i].ip = p.getString(k, "");
    snprintf(k, sizeof(k), "t%d", i); agents[i].token = p.getString(k, "");
    snprintf(k, sizeof(k), "p%d", i); agents[i].port = p.getUShort(k, 0);
  }
  p.end();
  p.begin(NVS_SEL_NS, true);
  sanitizeName(p.getString("pin", "").c_str(), pinned, sizeof(pinned));
  p.end();
  metricsState.paired = nAgents > 0;
  metricsState.cpu = metricsState.ram = metricsState.gpu = metricsState.vram = -1;
  metricsState.temp = -1;
  if (pinned[0]) board_log("[pc] PC fixado: %s", pinned);
  // Task propria, no nucleo 1 (o da interface): o POST do atalho e as metricas nao esperam mais pelas
  // consultas de IA (TLS de 10-16 s numa falha) que dividiam a task "usage". Sem TLS: nao usa a trava.
  xTaskCreatePinnedToCore(pcStepTask, "pcstep", PCSTEP_STACK, nullptr, 1, nullptr, 1);
}

// ---- API para a interface ----------------------------------------------------

PcMetrics pc_get() {
  Guard g;
  PcMetrics m = metricsState;
  int k;
  PcSource s = pickSource(&k);
  m.source = s;
  m.pinned = pinned[0] != 0;
  if (s == PC_SRC_USB) {
    m.paired = m.online = true;
    strlcpy(m.name, usb.name, sizeof(m.name));
    m.cpu = usb.cpu;
    m.ram = usb.ram;
    m.gpu = usb.gpu;
    m.vram = usb.vram;
    m.temp = usb.temp;
    m.error[0] = 0;
    m.updated_ms = usb.lastMs;
  } else if (s == PC_SRC_WIFI) {
    m.paired = true;
    strlcpy(m.name, agents[k].name.c_str(), sizeof(m.name));
    if (k != active) m.online = false;  // trocando de PC: ainda nao consultou este
  } else {
    m.online = false;
    m.paired = pinned[0] != 0;  // fixado num PC que nao esta por perto: mostra offline
    if (pinned[0]) {
      strlcpy(m.name, pinned, sizeof(m.name));
      strlcpy(m.error, "PC offline", sizeof(m.error));
    }
  }
  return m;
}

int pc_shortcut_count() {
  Guard g;
  int k;
  return pickSource(&k) == PC_SRC_USB ? usb.nShortcuts : nShortcuts;
}

bool pc_shortcut_label(int i, char *out, int cap) {
  Guard g;
  int k;
  bool fromUsb = pickSource(&k) == PC_SRC_USB;
  int n = fromUsb ? usb.nShortcuts : nShortcuts;
  if (i < 0 || i >= n) return false;
  strlcpy(out, fromUsb ? usb.shortcuts[i] : shortcuts[i], cap);
  return true;
}

void pc_shortcut_press(int i) {
  if (i < 0 || i >= PC_MAX_SHORTCUTS) return;
  bool viaUsb;
  {
    Guard g;
    int k;
    viaUsb = pickSource(&k) == PC_SRC_USB;
  }
  // USB: uma linha serial so, escrita aqui mesmo (o toque roda no loop, o mesmo nucleo do usb_link_loop):
  // nao espera task nenhuma. Wi-Fi: o POST HTTP fica com a task "pcstep".
  if (viaUsb) usb_link_send_action(i);
  else if (pendingAction < 0) pendingAction = i;
}

void pc_refresh_now() { nextMetrics = 0; }

void pc_set_visible(bool metricsPage, bool shortcutsPage) {
  if (metricsPage && !wantMetrics) nextMetrics = 0;
  if (shortcutsPage && !wantShortcuts) shortcutsLoaded = false;  // recarrega os rotulos ao entrar
  wantMetrics = metricsPage;
  wantShortcuts = shortcutsPage;
}

int pc_paired_count() {
  Guard g;
  return nAgents;
}

void pc_forget_all() {
  Guard g;
  nAgents = 0;
  active = 0;
  nShortcuts = 0;
  shortcutsLoaded = false;
  metricsState = {};
  metricsState.cpu = metricsState.ram = metricsState.gpu = metricsState.vram = -1;
  metricsState.temp = -1;
  saveAgents();
  pinned[0] = 0;  // o PC fixado podia ser um dos removidos
  Preferences p;
  p.begin(NVS_SEL_NS, false);
  p.remove("pin");
  p.end();
}

// ---- escolha do PC -----------------------------------------------------------

static constexpr int MAX_CHOICES = PC_MAX_AGENTS + 2;

// Lista sem repetir nome: Wi-Fi, o do USB e o fixado. Chamar com o lock.
static int collectChoices(char names[][PC_NAME_MAX], uint8_t masks[]) {
  int n = 0;
  auto add = [&](const char *name, uint8_t mask) {
    if (!name[0]) return;
    for (int i = 0; i < n; i++)
      if (!strcasecmp(names[i], name)) {
        masks[i] |= mask;
        return;
      }
    if (n >= MAX_CHOICES) return;
    strlcpy(names[n], name, PC_NAME_MAX);
    masks[n++] = mask;
  };
  for (int i = 0; i < nAgents; i++) add(agents[i].name.c_str(), 2);
  if (usb.seen) add(usb.name, usbAliveLocked() ? 1 : 0);
  add(pinned, 0);
  return n;
}

int pc_choice_count() {
  Guard g;
  char names[MAX_CHOICES][PC_NAME_MAX];
  uint8_t masks[MAX_CHOICES];
  return collectChoices(names, masks);
}

bool pc_choice_name(int i, char *out, int cap, uint8_t *src_mask) {
  Guard g;
  char names[MAX_CHOICES][PC_NAME_MAX];
  uint8_t masks[MAX_CHOICES];
  int n = collectChoices(names, masks);
  if (i < 0 || i >= n) return false;
  strlcpy(out, names[i], cap);
  if (src_mask) *src_mask = masks[i];
  return true;
}

bool pc_pinned_name(char *out, int cap) {
  Guard g;
  if (!pinned[0]) return false;
  strlcpy(out, pinned, cap);
  return true;
}

void pc_select(const char *name) {
  Guard g;
  char clean[PC_NAME_MAX] = "";
  if (name) sanitizeName(name, clean, sizeof(clean));
  if (!strcmp(clean, pinned)) return;
  strlcpy(pinned, clean, sizeof(pinned));
  Preferences p;
  p.begin(NVS_SEL_NS, false);
  if (pinned[0]) p.putString("pin", pinned);
  else p.remove("pin");
  p.end();
  // A fonte pode mudar: o pc_step reage a isso na proxima volta.
  failures = 0;
  nextMetrics = 0;
  shortcutsLoaded = false;
  metricsState.online = false;
  board_log("[pc] %s", pinned[0] ? pinned : "automatico");
}

// ---- fonte USB ---------------------------------------------------------------

static float cleanPct(float v) { return (v >= 0 && v <= 100) ? v : -1; }

void pc_usb_feed(const char *name, float cpu, float ram, float gpu, float vram, int temp) {
  Guard g;
  char clean[PC_NAME_MAX];
  sanitizeName(name ? name : "", clean, sizeof(clean));
  if (!clean[0]) strlcpy(clean, "PC (USB)", sizeof(clean));
  if (strcmp(clean, usb.name)) {  // outro PC no cabo: os atalhos do anterior nao valem
    usb.nShortcuts = 0;
    strlcpy(usb.name, clean, sizeof(usb.name));
  }
  usb.seen = true;
  usb.cpu = cleanPct(cpu);
  usb.ram = cleanPct(ram);
  usb.gpu = cleanPct(gpu);
  usb.vram = cleanPct(vram);
  usb.temp = (temp >= 0 && temp <= 150) ? temp : -1;
  usb.lastMs = millis();
}

void pc_usb_shortcuts(const char *const *labels, int n) {
  Guard g;
  usb.nShortcuts = 0;
  for (int i = 0; i < n && usb.nShortcuts < PC_MAX_SHORTCUTS; i++) {
    char tmp[13];
    snprintf(tmp, sizeof(tmp), "%s", labels[i] ? labels[i] : "");
    for (char *c = tmp; *c; c++)
      if ((uint8_t)*c >= 0x80 || (uint8_t)*c < 32) *c = '?';  // as fontes embutidas so tem ASCII
    strlcpy(usb.shortcuts[usb.nShortcuts++], tmp, sizeof(usb.shortcuts[0]));
  }
}

bool pc_usb_alive() {
  Guard g;
  return usbAliveLocked();
}

bool pc_pair(const char *name, const char *host, const char *ip, int port, const char *token) {
  String n, h, t;
  for (const char *p = name; *p && n.length() < 23; p++) n += (*p >= 32 && *p < 127) ? *p : '?';
  for (const char *p = host; *p && h.length() < 40; p++)
    if (isalnum((unsigned char)*p) || *p == '-' || *p == '_') h += *p;
  for (const char *p = token; *p; p++)
    if (isxdigit((unsigned char)*p)) t += *p;
  if (n.isEmpty() || t.length() < 16 || t.length() > 64 || port < 1 || port > 65535 || !strlen(ip)) return false;
  Guard g;
  int slot = -1;
  for (int i = 0; i < nAgents; i++)
    if (agents[i].name == n) slot = i;
  if (slot < 0) slot = nAgents < PC_MAX_AGENTS ? nAgents++ : PC_MAX_AGENTS - 1;  // lotou: troca o ultimo
  agents[slot].name = n;
  agents[slot].host = h;
  agents[slot].ip = ip;
  agents[slot].token = t;
  agents[slot].port = (uint16_t)port;
  active = slot;
  failures = 0;
  nextMetrics = 0;
  shortcutsLoaded = false;
  metricsState.paired = true;
  saveAgents();
  board_log("[pc] pareado com %s (%s:%d)", n.c_str(), ip, port);
  return true;
}

// ---- HTTP --------------------------------------------------------------------

// GET/POST simples. status 0 = sem conexao. O corpo vem ate max bytes.
static int http(const Agent &a, const char *method, const String &path, String &body, size_t max = 900) {
  WiFiClient c;
  IPAddress ip;
  if (!ip.fromString(a.ip) || !c.connect(ip, a.port, CONNECT_TIMEOUT_MS)) return 0;
  c.setTimeout(1500);
  c.print(String(method) + " " + path + " HTTP/1.0\r\nHost: " + a.ip + "\r\nAuthorization: Bearer " + a.token +
          "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  String line = c.readStringUntil('\n');
  int sp = line.indexOf(' ');
  int status = sp > 0 ? line.substring(sp + 1).toInt() : 0;
  while (c.connected() || c.available()) {
    String h = c.readStringUntil('\n');
    if (h == "\r" || h.isEmpty()) break;
  }
  body = "";
  while ((c.connected() || c.available()) && body.length() < max) {
    int ch = c.read();
    if (ch < 0) {
      if (!c.connected()) break;
      delay(2);
      continue;
    }
    body += (char)ch;
  }
  c.stop();
  return status;
}

// Procura o agente por mDNS e atualiza o IP guardado (o IP do PC pode mudar).
static void rediscover() {
  lastRediscover = millis();
  if (!mdnsStarted) return;
  int n = MDNS.queryService("painelcyd", "tcp");
  for (int i = 0; i < n; i++) {
    String host = MDNS.hostname(i);
    Guard g;
    for (int k = 0; k < nAgents; k++) {
      if (agents[k].host.equalsIgnoreCase(host) || agents[k].name.equalsIgnoreCase(host)) {
        String ip = MDNS.IP(i).toString();
        if (ip != agents[k].ip || MDNS.port(i) != agents[k].port) {
          board_log("[pc] %s mudou de endereco: %s:%d", agents[k].name.c_str(), ip.c_str(), MDNS.port(i));
          agents[k].ip = ip;
          agents[k].port = MDNS.port(i);
          saveAgents();
        }
      }
    }
  }
}

static void markFailure(const char *why) {
  Guard g;
  metricsState.online = false;
  strlcpy(metricsState.error, why, sizeof(metricsState.error));
  if (++failures >= 2 && nAgents > 1 && !pinned[0]) {  // tenta o proximo PC pareado (fixado: nao troca)
    active = (active + 1) % nAgents;
    failures = 0;
    shortcutsLoaded = false;
  }
}

static void pollMetrics() {
  Agent a;
  {
    Guard g;
    a = agents[active];
  }
  String body;
  int st = http(a, "GET", "/metrics", body);
  if (st == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      Guard g;
      failures = 0;
      metricsState.online = true;
      metricsState.error[0] = 0;
      metricsState.cpu = doc["cpu"] | -1.0f;
      metricsState.ram = doc["ram"] | -1.0f;
      metricsState.gpu = doc["gpu"] | -1.0f;
      metricsState.vram = doc["vram"] | -1.0f;
      metricsState.temp = doc["temp"].is<float>() ? (int)(doc["temp"].as<float>() + 0.5f) : -1;
      metricsState.updated_ms = millis();
      return;
    }
  }
  markFailure(st == 401 ? "token recusado" : st == 0 ? "PC offline" : "resposta invalida");
  if (st == 0 && millis() - lastRediscover > REDISCOVER_EVERY_MS) rediscover();
}

static void loadShortcuts() {
  Agent a;
  {
    Guard g;
    a = agents[active];
  }
  String body;
  int st = http(a, "GET", "/shortcuts", body, 1200);
  if (st != 200) return;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  Guard g;
  nShortcuts = 0;
  for (JsonObject s : doc.as<JsonArray>()) {
    if (nShortcuts >= PC_MAX_SHORTCUTS) break;
    const char *l = s["label"] | "";
    char tmp[13];
    snprintf(tmp, sizeof(tmp), "%s", l);
    for (char *c = tmp; *c; c++)
      if ((uint8_t)*c >= 0x80) *c = '?';  // as fontes embutidas so tem ASCII
    strlcpy(shortcuts[nShortcuts++], tmp, sizeof(shortcuts[0]));
  }
  shortcutsLoaded = true;
}

void pc_step() {
  PcSource src;
  {
    Guard g;
    int k;
    src = pickSource(&k);
    if (src == PC_SRC_WIFI && (lastSrc != PC_SRC_WIFI || k != active)) {  // passou a valer este PC
      active = k;
      failures = 0;
      nextMetrics = 0;
      shortcutsLoaded = false;
      metricsState.online = false;
      metricsState.error[0] = 0;
    }
    lastSrc = src;
  }
  if (src == PC_SRC_USB) {  // o PC empurra as metricas; so falta levar o atalho tocado
    if (pendingAction >= 0) {
      int idx = pendingAction;
      pendingAction = -1;
      usb_link_send_action(idx);
    }
    return;
  }
  if (src == PC_SRC_NONE) pendingAction = -1;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!mdnsStarted) mdnsStarted = MDNS.begin("painel-cyd");  // painel-cyd.local
  if (src != PC_SRC_WIFI) return;

  if (pendingAction >= 0) {
    int idx = pendingAction;
    pendingAction = -1;
    Agent a;
    {
      Guard g;
      a = agents[active];
    }
    String body;
    int st = http(a, "POST", String("/action/") + idx, body, 200);
    board_log("[pc] atalho %d: HTTP %d", idx, st);
    if (st == 0 && millis() - lastRediscover > 5000) rediscover();
    return;
  }
  if (wantShortcuts && !shortcutsLoaded) {
    loadShortcuts();
    if (!shortcutsLoaded) shortcutsLoaded = true;  // sem agente: nao repete a cada segundo
    return;
  }
  if (wantMetrics && (int32_t)(millis() - nextMetrics) >= 0) {
    pollMetrics();
    nextMetrics = millis() + METRICS_EVERY_MS;
  }
}
