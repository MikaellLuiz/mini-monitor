// Spotify na placa: consulta o player (/me/player), envia play/pause/proxima/anterior
// e baixa a capa de 64 px. Roda na task "spotify" (spotify_step() a cada segundo ou na hora de um comando),
// entao compartilha a regra de uma conexao HTTPS por vez. Nenhum token vai para o log.

#include <ArduinoJson.h>
#include <Arduino.h>
#include <Preferences.h>
#include <TJpg_Decoder.h>
#include <WiFi.h>

#include "board.h"
#include "https_client.h"
#include "spotify.h"

static const char *NVS_NS = "accounts";
static constexpr uint32_t POLL_VISIBLE_MS = 4000;
static constexpr uint32_t TOKEN_MARGIN_MS = 120 * 1000;
static constexpr size_t COVER_MAX_BYTES = 8192;

static SemaphoreHandle_t lock = xSemaphoreCreateMutex();
struct Guard {
  Guard() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(lock); }
};

static SpotifyState st;
static String clientId, refreshToken, accessTok;
static uint32_t accessValidUntil = 0;  // millis ate quando o access token serve (com margem)
static bool haveAccess = false;
static volatile bool visible = false;
static volatile SpotifyCmd pending = SpotifyCmd::None;
static uint32_t nextPoll = 0;
static TaskHandle_t spotTask = nullptr;
static uint32_t cmdTapMs = 0;  // millis do toque do comando pendente (medida de latencia)
static constexpr uint32_t SPOT_STACK = 8 * 1024;  // cobre o handshake TLS (accounts/api.spotify.com); 4 KB estourou o canario; ver log "[spotify] pilha"

// Task propria no nucleo 0, prioridade da idle (como a "usage": ver usage_esp32.cpp). Acorda a cada 1 s
// para o poll periodico, ou na hora por spotify_command().
static void spotifyTask(void *) {
  https_mark_interactive_task();  // esta task nao cede a trava a ninguem
  UBaseType_t minFreeWords = ~0u;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    spotify_step();
    UBaseType_t hw = uxTaskGetStackHighWaterMark(nullptr);
    if (hw < minFreeWords) {
      minFreeWords = hw;
      board_log("[spotify] pilha livre min %u B de %u", (unsigned)hw, (unsigned)SPOT_STACK);
    }
  }
}
static String coverUrl;  // capa ja carregada (ou em carga)
static uint16_t *coverBuf = nullptr;
static bool coverValid = false;
// Depois de play/pause o Spotify leva alguns segundos para refletir o novo estado: segura o
// estado otimista e ignora is_playing das leituras nesse intervalo (senao a tela volta atras
// e o toque seguinte repete o mesmo comando, que da 403).
static uint32_t holdPlayingUntil = 0;
static bool heldPlaying = false;

// ---- texto -------------------------------------------------------------------

// As fontes embutidas do LVGL so tem ASCII: converte UTF-8 latino para a letra base.
static void asciiFold(const char *in, char *out, size_t cap) {
  static const char LATIN1[] = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTBaaaaaaaceeeeiiiidnooooo/ouuuuyty";
  size_t o = 0;
  for (const uint8_t *p = (const uint8_t *)in; *p && o + 1 < cap;) {
    uint32_t cp;
    int n;
    if (*p < 0x80) cp = *p, n = 1;
    else if ((*p & 0xE0) == 0xC0) cp = *p & 0x1F, n = 2;
    else if ((*p & 0xF0) == 0xE0) cp = *p & 0x0F, n = 3;
    else cp = *p & 0x07, n = 4;
    for (int i = 1; i < n && p[i]; i++) cp = (cp << 6) | (p[i] & 0x3F);
    p += n;
    char c = '?';
    if (cp < 0x80) c = (char)cp;
    else if (cp >= 0xC0 && cp <= 0xFF) c = LATIN1[cp - 0xC0];
    else if (cp == 0x2018 || cp == 0x2019) c = '\'';
    else if (cp == 0x201C || cp == 0x201D) c = '"';
    else if (cp == 0x2013 || cp == 0x2014) c = '-';
    out[o++] = c;
  }
  out[o] = 0;
}

static void setError(const char *why) {
  Guard g;
  strlcpy(st.error, why, sizeof(st.error));
}

// ---- conta -------------------------------------------------------------------

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

static void nvsPut(const char *k, const String &v) {
  Preferences p;
  p.begin(NVS_NS, false);
  p.putString(k, v);
  p.end();
}

void spotify_begin() {
  accessTok.reserve(512);  // capacidade fixa antes da reserva de heap: o token nao se move durante a TLS
  coverBuf = (uint16_t *)heap_caps_malloc(SPOTIFY_COVER_PX * SPOTIFY_COVER_PX * 2, MALLOC_CAP_8BIT);
  Preferences p;
  p.begin(NVS_NS, true);
  clientId = p.getString("spotify_client", "");
  refreshToken = p.getString("spotify", "");
  p.end();
  st.linked = !clientId.isEmpty() && !refreshToken.isEmpty();
  xTaskCreatePinnedToCore(spotifyTask, "spotify", SPOT_STACK, nullptr, tskIDLE_PRIORITY, &spotTask, 0);
}

bool spotify_set_account(const char *client, const char *rt) {
  String c, t;
  for (const char *p = client; *p; p++)
    if (isalnum((unsigned char)*p)) c += *p;
  for (const char *p = rt; *p; p++)
    if (isalnum((unsigned char)*p) || *p == '-' || *p == '_') t += *p;
  if (c.length() < 16 || c.length() > 64 || t.length() < 20 || t.length() > 400) return false;
  nvsPut("spotify_client", c);
  nvsPut("spotify", t);
  Guard g;
  clientId = c;
  refreshToken = t;
  haveAccess = false;
  accessTok = "";
  st = {};
  st.linked = true;
  coverUrl = "";
  coverValid = false;
  nextPoll = 0;
  board_log("[spotify] conta salva (client %.6s...)", c.c_str());
  return true;
}

void spotify_forget_account() {
  Preferences p;
  p.begin(NVS_NS, false);
  p.remove("spotify_client");
  p.remove("spotify");
  p.end();
  Guard g;
  clientId = refreshToken = accessTok = "";
  haveAccess = false;
  st = {};
  coverUrl = "";
  coverValid = false;
}

SpotifyState spotify_get() {
  Guard g;
  return st;
}

const uint16_t *spotify_cover() {
  Guard g;
  return coverValid ? coverBuf : nullptr;
}

void spotify_command(SpotifyCmd c) {
  {
    Guard g;
    if (!st.linked || pending != SpotifyCmd::None) return;
    pending = c;
    cmdTapMs = millis();
    st.busy = true;
    if (c == SpotifyCmd::PlayPause) st.playing = !st.playing;  // resposta imediata na tela
  }
  https_interactive(true);  // as consultas de uso em retentativa cedem a trava ate o comando terminar
  if (spotTask) xTaskNotifyGive(spotTask);  // acorda a task na hora, sem esperar o ciclo de 1 s
}

void spotify_refresh_now() {
  nextPoll = 0;
  if (spotTask) xTaskNotifyGive(spotTask);
}

void spotify_set_visible(bool v) {
  if (v && !visible) nextPoll = 0;
  visible = v;
}

// ---- HTTP --------------------------------------------------------------------

// Garante um access token valido. false = sem rede/recusado (error preenchido).
static bool ensureAccess() {
  if (haveAccess && (int32_t)(millis() - accessValidUntil) < 0) return true;
  String cid, rt;
  {
    Guard g;
    cid = clientId;
    rt = refreshToken;
  }
  HttpsRequest rq;
  rq.host = "accounts.spotify.com";
  rq.method = "POST";
  rq.path = "/api/token";
  rq.http10 = true;
  rq.headers = "Content-Type: application/x-www-form-urlencoded\r\nUser-Agent: painel-cyd/0.1\r\n";
  rq.body = "grant_type=refresh_token&client_id=" + cid + "&refresh_token=" + urlEncode(rt);
  bool got = false;
  rq.onBody = [&](Stream &s) {
    for (int i = 0; i < 24 && s.available() && s.peek() != '{'; i++) s.read();
    JsonDocument filter;
    filter["access_token"] = true;
    filter["expires_in"] = true;
    filter["refresh_token"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, s, DeserializationOption::Filter(filter))) return;
    const char *at = doc["access_token"];
    if (!at) return;
    uint32_t exp = doc["expires_in"] | 3600;
    if (const char *nrt = doc["refresh_token"]) {  // o Spotify as vezes devolve um novo: grava ja
      nvsPut("spotify", nrt);
      Guard g;
      refreshToken = nrt;
    }
    Guard g;
    accessTok = at;
    accessValidUntil = millis() + (exp * 1000UL) - TOKEN_MARGIN_MS;
    haveAccess = true;
    got = true;
  };
  HttpsResult rs;
  bool ok = https_send(rq, rs);
  board_log("[spotify] token: HTTP %d %s em %u ms", rs.status, got ? "ok" : "falhou", (unsigned)rs.total_ms);
  if (got) return true;
  setError(ok && (rs.status == 400 || rs.status == 401) ? "login expirou" : "sem conexao");
  return false;
}

static bool sendCommand(SpotifyCmd c, bool wasPlaying) {
  const char *method = "POST", *path = "/v1/me/player/next";
  if (c == SpotifyCmd::Prev) path = "/v1/me/player/previous";
  if (c == SpotifyCmd::PlayPause) {
    method = "PUT";
    path = wasPlaying ? "/v1/me/player/pause" : "/v1/me/player/play";
  }
  HttpsRequest rq;
  rq.host = "api.spotify.com";
  rq.method = method;
  rq.path = path;
  rq.http10 = true;
  rq.headers = "Authorization: Bearer " + accessTok + "\r\nContent-Length: 0\r\nUser-Agent: painel-cyd/0.1\r\n";
  HttpsResult rs;
  https_send(rq, rs);
  board_log("[spotify] comando %s %s: HTTP %d em %u ms", method, path + 3, rs.status, (unsigned)rs.total_ms);
  if (rs.status == 204 || rs.status == 200) {
    setError("");
    return true;
  }
  if (rs.status == 401) {
    haveAccess = false;
    setError("token vencido");
  } else if (rs.status == 403 && c == SpotifyCmd::PlayPause) {
    // "Restriction violated": o comando nao cabe no estado real (ex.: pausar o que ja
    // estava pausado por outro aparelho). O estado mostrado pode ter ate ~4 s: nao e
    // erro, e o proximo poll (logo em seguida) corrige a tela.
    board_log("[spotify] 403 em play/pause (estado desatualizado?): %.100s", rs.body.c_str());
    setError("");
    return true;
  } else if (rs.status == 403) {
    board_log("[spotify] 403: %.100s", rs.body.c_str());
    setError("comando recusado (403)");
  } else if (rs.status == 404) {
    setError("sem dispositivo ativo");
  } else if (rs.status == 429) {
    setError("muitas requisicoes");
  } else {
    char why[40];
    snprintf(why, sizeof(why), "HTTP %d", rs.status);
    setError(why);
  }
  return false;
}

// ---- capa --------------------------------------------------------------------

static bool jpgOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (!coverBuf) return false;
  for (uint16_t row = 0; row < h; row++) {
    int dy = y + row;
    if (dy >= SPOTIFY_COVER_PX) break;
    for (uint16_t col = 0; col < w; col++) {
      int dx = x + col;
      if (dx < SPOTIFY_COVER_PX) coverBuf[dy * SPOTIFY_COVER_PX + dx] = bitmap[row * w + col];
    }
  }
  return true;
}

static void fetchCover(const String &url) {
  int hostStart = url.indexOf("://");
  if (hostStart < 0 || !coverBuf) return;
  hostStart += 3;
  int pathStart = url.indexOf('/', hostStart);
  if (pathStart < 0) return;
  String host = url.substring(hostStart, pathStart), path = url.substring(pathStart);

  // O buffer de download so existe durante a TLS desta capa (alocado no onBody, depois do handshake, e
  // liberado logo apos o decode): 8 KB permanentes tiravam heap das TLS dos outros servicos.
  uint8_t *data = nullptr;
  size_t len = 0;
  HttpsHeader want[] = {{"content-length"}};
  HttpsRequest rq;
  rq.host = host.c_str();
  rq.method = "GET";
  rq.path = path.c_str();
  rq.http10 = true;
  rq.headers = "User-Agent: painel-cyd/0.1\r\n";
  rq.want = want;
  rq.want_count = 1;
  rq.onBody = [&](Stream &s) {
    size_t expect = want[0].value.toInt();
    if (expect == 0 || expect > COVER_MAX_BYTES) expect = COVER_MAX_BYTES;
    data = (uint8_t *)heap_caps_malloc(expect, MALLOC_CAP_8BIT);
    if (!data) {
      board_log("[spotify] capa: sem memoria para %u bytes", (unsigned)expect);
      return;
    }
    len = s.readBytes(data, expect);
  };
  HttpsResult rs;
  https_send(rq, rs);
  bool ok = false;
  if (rs.status == 200 && data && len > 100 && data[0] == 0xFF && data[1] == 0xD8) {
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(true);  // ordem de bytes do LVGL com LV_COLOR_16_SWAP
    TJpgDec.setCallback(jpgOut);
    Guard g;
    ok = TJpgDec.drawJpg(0, 0, data, len) == 0;
    if (ok) {
      coverValid = true;
      st.cover_seq++;
    }
  }
  free(data);
  board_log("[spotify] capa: HTTP %d, %u bytes, %s | heap %u, bloco %u", rs.status, (unsigned)len,
            ok ? "ok" : "falhou", ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// ---- consulta ----------------------------------------------------------------

static void poll() {
  HttpsHeader want[] = {{"retry-after"}};
  String newCover;
  struct {
    bool is_playing = false;
    uint32_t progress = 0, duration = 0;
    char title[64] = "", artist[48] = "", device[28] = "";
  } r;
  bool parsed = false;

  HttpsRequest rq;
  rq.host = "api.spotify.com";
  rq.method = "GET";
  rq.path = "/v1/me/player";
  rq.http10 = true;
  rq.headers = "Authorization: Bearer " + accessTok + "\r\nUser-Agent: painel-cyd/0.1\r\n";
  rq.want = want;
  rq.want_count = 1;
  rq.onBody = [&](Stream &s) {
    for (int i = 0; i < 24 && s.available() && s.peek() != '{'; i++) s.read();
    JsonDocument filter;
    filter["is_playing"] = true;
    filter["progress_ms"] = true;
    filter["device"]["name"] = true;
    filter["item"]["name"] = true;
    filter["item"]["duration_ms"] = true;
    filter["item"]["artists"][0]["name"] = true;
    filter["item"]["album"]["images"][0]["url"] = true;
    filter["item"]["album"]["images"][0]["width"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, s, DeserializationOption::Filter(filter))) return;
    r.is_playing = doc["is_playing"] | false;
    if ((int32_t)(millis() - holdPlayingUntil) < 0) r.is_playing = heldPlaying;
    r.progress = doc["progress_ms"] | 0u;
    r.duration = doc["item"]["duration_ms"] | 0u;
    asciiFold(doc["item"]["name"] | "", r.title, sizeof(r.title));
    asciiFold(doc["item"]["artists"][0]["name"] | "", r.artist, sizeof(r.artist));
    asciiFold(doc["device"]["name"] | "", r.device, sizeof(r.device));
    int best = 100000;  // a menor capa (64 px)
    for (JsonObject im : doc["item"]["album"]["images"].as<JsonArray>()) {
      int w = im["width"] | 100000;
      if (w < best) {
        best = w;
        newCover = (const char *)(im["url"] | "");
      }
    }
    parsed = true;
  };
  HttpsResult rs;
  https_send(rq, rs);
  board_log("[spotify] player: HTTP %d em %u ms | heap %u, bloco %u", rs.status, (unsigned)rs.total_ms,
            ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  uint32_t wait = POLL_VISIBLE_MS;
  if (rs.status == 200 && parsed) {
    Guard g;
    st.active = true;
    st.playing = r.is_playing;
    st.progress_ms = r.progress;
    st.duration_ms = r.duration;
    st.fetched_at_ms = millis();
    strlcpy(st.title, r.title, sizeof(st.title));
    strlcpy(st.artist, r.artist, sizeof(st.artist));
    strlcpy(st.device, r.device, sizeof(st.device));
    st.error[0] = 0;
  } else if (rs.status == 204) {
    Guard g;
    st.active = false;
    st.playing = false;
    st.title[0] = st.artist[0] = st.device[0] = 0;
    st.error[0] = 0;
    wait = 8000;
  } else if (rs.status == 401) {
    haveAccess = false;
    wait = 500;
  } else if (rs.status == 429) {
    uint32_t retry = want[0].value.toInt();
    wait = (retry ? retry : 30) * 1000UL;
    setError("muitas requisicoes");
  } else {
    char why[40];
    snprintf(why, sizeof(why), rs.status ? "HTTP %d" : "sem conexao", rs.status);
    setError(why);
    wait = 10000;
  }
  nextPoll = millis() + wait;

  if (rs.status == 200 && parsed && !newCover.isEmpty() && newCover != coverUrl) {
    coverUrl = newCover;
    fetchCover(newCover);
  }
}

void spotify_step() {
  if (WiFi.status() != WL_CONNECTED) return;
  {
    Guard g;
    if (!st.linked) return;
  }
  SpotifyCmd c = pending;
  if (c == SpotifyCmd::None && (!visible || (int32_t)(millis() - nextPoll) < 0)) return;
  if (!ensureAccess()) {
    nextPoll = millis() + 15000;
    Guard g;
    st.busy = false;
    pending = SpotifyCmd::None;
    https_interactive(false);
    return;
  }
  if (c != SpotifyCmd::None) {
    bool wasPlaying;
    {
      Guard g;
      wasPlaying = !st.playing;  // spotify_command() ja inverteu o estado otimista
    }
    // Latencia medida: do toque (spotify_command) ate aqui, logo antes do pedido HTTPS do comando.
    board_log("[spotify] comando %d: toque->envio %u ms", (int)c, (unsigned)(millis() - cmdTapMs));
    bool ok = sendCommand(c, wasPlaying);
    https_interactive(false);
    board_log("[spotify] comando %d: toque->resposta %u ms (%s)", (int)c, (unsigned)(millis() - cmdTapMs), ok ? "ok" : "falhou");
    {
      Guard g;
      st.busy = false;
      pending = SpotifyCmd::None;
      if (c == SpotifyCmd::PlayPause) {
        heldPlaying = st.playing;  // o valor otimista que spotify_command() ja colocou
        holdPlayingUntil = millis() + 3500;
      }
    }
    nextPoll = millis() + (ok ? 800 : 0);  // relê o player logo, para mostrar o estado real
    return;
  }
  poll();
}
