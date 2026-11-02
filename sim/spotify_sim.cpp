// Spotify falso do simulador: estados escolhidos pelo roteiro ("spotify <estado>").

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "sim.h"
#include "spotify.h"

static SpotifyState st;
static uint16_t cover[SPOTIFY_COVER_PX * SPOTIFY_COVER_PX];
static bool haveCover = false;

static void makeCover() {
  for (int y = 0; y < SPOTIFY_COVER_PX; y++)
    for (int x = 0; x < SPOTIFY_COVER_PX; x++) {
      lv_color_t c = lv_color_make(40 + x * 3, 90 + y, 200 - x * 2);
      cover[y * SPOTIFY_COVER_PX + x] = c.full;
    }
  haveCover = true;
}

void spotify_begin() {
  st.linked = true;
  st.active = true;
  st.playing = true;
  snprintf(st.title, sizeof(st.title), "I Don't Want To Miss A Thing - Pop Mix");
  snprintf(st.artist, sizeof(st.artist), "Aerosmith");
  snprintf(st.device, sizeof(st.device), "S26 Ultra de Mikael");
  st.duration_ms = 4 * 60 * 1000 + 27 * 1000;
  st.progress_ms = 83 * 1000;
  st.cover_seq = 1;
  makeCover();
}

SpotifyState spotify_get() {
  st.fetched_at_ms = board_millis();  // o progresso nao avanca sozinho no roteiro
  return st;
}
const uint16_t *spotify_cover() { return haveCover ? cover : nullptr; }
void spotify_command(SpotifyCmd c) {
  if (c == SpotifyCmd::PlayPause) st.playing = !st.playing;
  board_log("[sim] spotify: comando %d", (int)c);
}
void spotify_set_visible(bool) {}
void spotify_refresh_now() {}
bool spotify_set_account(const char *, const char *) { return st.linked = true; }
void spotify_forget_account() { st = {}; }
void spotify_step() {}

// Roteiro: spotify <playing|paused|idle|none|error>
bool spotify_sim_set_state(const char *name) {
  if (!strcmp(name, "playing")) { st.linked = st.active = st.playing = true; st.error[0] = 0; }
  else if (!strcmp(name, "paused")) { st.linked = st.active = true; st.playing = false; st.error[0] = 0; }
  else if (!strcmp(name, "idle")) { st.linked = true; st.active = st.playing = false; st.error[0] = 0; }
  else if (!strcmp(name, "none")) { st.linked = false; }
  else if (!strcmp(name, "error")) { st.linked = st.active = true; snprintf(st.error, sizeof(st.error), "comando recusado (403)"); }
  else return false;
  return true;
}
