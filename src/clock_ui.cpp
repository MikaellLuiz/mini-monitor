#include "clock_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "clock_font.h"
#include "spotify.h"
#include "theme.h"
#include "settings_ui.h"
#include "ui.h"
#include "usage.h"

static constexpr float DEG2RAD_PI = 3.14159265f;

// ---- opcoes (NVS) ------------------------------------------------------------

enum Style : uint8_t { ST_DIGITAL, ST_ANALOG, ST_MINIMAL, ST_RING, ST_COUNT };
static const char *STYLE_NAMES[ST_COUNT] = {"Digital", "Analogico", "Minimo", "Anel"};

static const char *K_STYLE = "clk_style";
static const char *K_24H = "clk_24h";
static const char *K_SEC = "clk_sec";
static const char *K_SHORT = "clk_short";
static const char *K_ACC = "clk_acc";

struct Cfg {
  uint8_t style, h24, sec, shortcuts, accent;
};
static Cfg active;  // lido uma vez por montagem: a NVS e lenta demais para o timer

static Cfg readCfg() {
  Cfg c;
  c.style = settings_get_u8(K_STYLE, ST_DIGITAL);
  if (c.style >= ST_COUNT) c.style = ST_DIGITAL;
  c.h24 = settings_get_u8(K_24H, 1);
  c.sec = settings_get_u8(K_SEC, 1);
  c.shortcuts = settings_get_u8(K_SHORT, 1);
  c.accent = settings_get_u8(K_ACC, 0);
  if (c.accent > 3) c.accent = 0;
  return c;
}

void clock_ui_summary(char *buf, int size) {
  Cfg c = readCfg();
  snprintf(buf, size, "%s - %s", STYLE_NAMES[c.style], c.h24 ? "24 h" : "12 h");
}

// Sem acentos: as fontes embutidas nao tem os glifos.
static const char *WDAYS[] = {"Domingo", "Segunda-feira", "Terca-feira", "Quarta-feira",
                              "Quinta-feira", "Sexta-feira", "Sabado"};
static const char *WDAYS3[] = {"DOM", "SEG", "TER", "QUA", "QUI", "SEX", "SAB"};
static const char *MONTHS[] = {"janeiro", "fevereiro", "marco",    "abril",   "maio",     "junho",
                               "julho",   "agosto",    "setembro", "outubro", "novembro", "dezembro"};
static const char *MONTHS3[] = {"JAN", "FEV", "MAR", "ABR", "MAI", "JUN", "JUL", "AGO", "SET", "OUT", "NOV", "DEZ"};
static const char *MONTHS3L[] = {"jan", "fev", "mar", "abr", "mai", "jun", "jul", "ago", "set", "out", "nov", "dez"};

// ---- pagina ------------------------------------------------------------------

static lv_obj_t *root;  // tudo do estilo atual; recriado quando o estilo muda
static lv_obj_t *lblWday, *lblTime, *lblSec, *lblDate, *lblWin, *lblMusic;
static lv_obj_t *face, *ringArc, *hand[3];
static lv_point_t handPts[3][2];
static int shownMin = -1, shownSec = -1;
static lv_timer_t *clockTimer;
static bool compactChips;  // cartoes estreitos (estilo analogico): texto mais curto

static void setIfChanged(lv_obj_t *l, const char *text) {
  if (l && strcmp(lv_label_get_text(l), text)) lv_label_set_text(l, text);
}

static void chipCb(lv_event_t *e) { ui_goto((PageId)(intptr_t)lv_event_get_user_data(e)); }

// Cartao de atalho: icone, texto curto e seta. Leva para a pagina `target`.
static lv_obj_t *makeChip(int x, int y, int w, int h, bool music, PageId target) {
  lv_obj_t *c = ui_card(root, x, y, w, h);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(c, thc(th::ROW_PR), LV_STATE_PRESSED);
  lv_obj_add_event_cb(c, chipCb, LV_EVENT_CLICKED, (void *)(intptr_t)target);
  if (music) {
    lv_obj_t *ic = lv_label_create(c);
    lv_label_set_text(ic, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ic, thc(th::GREEN), 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 10, 0);
  } else {
    lv_obj_t *ic = ui_dot(c, 8, th::CLAUDE);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 11, 0);
  }
  lv_obj_t *l = lv_label_create(c);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_size(l, w - 48, 16);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(l, thc(th::ON_MINI), 0);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 26, 0);
  lv_label_set_text(l, "");
  lv_obj_t *arrow = ui_muted(c, LV_SYMBOL_RIGHT);
  lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -9, 0);
  return l;
}

static void placeTime() {  // hora e segundos lado a lado, centrados na tela
  lv_obj_update_layout(lblTime);
  int tw = lv_obj_get_width(lblTime);
  int sw = lblSec && active.sec ? 32 : 0;
  lv_obj_set_x(lblTime, (320 - tw - sw) / 2);
  if (lblSec) {
    lv_obj_set_x(lblSec, (320 - tw - sw) / 2 + tw + 6);
    lv_obj_set_y(lblSec, lv_obj_get_y(lblTime) + 22);
  }
}

static void buildDigital() {
  lblWday = ui_muted(root, "", &lv_font_montserrat_14);
  lv_obj_set_pos(lblWday, 14, 10);
  lblTime = lv_label_create(root);
  lv_obj_set_style_text_font(lblTime, &font_clock_big, 0);
  lv_obj_set_pos(lblTime, 0, 14);
  lv_label_set_text(lblTime, "--:--");
  lblSec = lv_label_create(root);
  lv_obj_set_style_text_font(lblSec, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lblSec, thc(theme_accent(active.accent)), 0);
  lv_label_set_text(lblSec, "");
  if (!active.sec) lv_obj_add_flag(lblSec, LV_OBJ_FLAG_HIDDEN);
  lblDate = lv_label_create(root);
  lv_obj_set_style_text_font(lblDate, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lblDate, thc(th::ON_MINI), 0);
  lv_obj_set_width(lblDate, 320);
  lv_obj_set_style_text_align(lblDate, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(lblDate, 0, 112);
  lv_label_set_text(lblDate, "");
  if (active.shortcuts) {
    lblWin = makeChip(14, 166, 143, 34, false, PG_USAGE);
    lblMusic = makeChip(163, 166, 143, 34, true, PG_SPOTIFY);
  }
  placeTime();
}

static void buildMinimal() {
  lblTime = lv_label_create(root);
  lv_obj_set_style_text_font(lblTime, &font_clock_huge, 0);
  lv_obj_set_pos(lblTime, 0, 22);
  lv_label_set_text(lblTime, "--:--");
  lblDate = lv_label_create(root);
  lv_obj_set_style_text_font(lblDate, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lblDate, thc(th::MUTED), 0);
  lv_obj_set_style_text_letter_space(lblDate, 2, 0);
  lv_obj_set_width(lblDate, 320);
  lv_obj_set_style_text_align(lblDate, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(lblDate, 0, 160);
  lv_label_set_text(lblDate, "");
  lblSec = nullptr;
  placeTime();
}

static void buildRing() {
  ringArc = lv_arc_create(root);
  lv_obj_set_size(ringArc, 158, 158);
  lv_obj_set_pos(ringArc, 81, 6);
  lv_arc_set_rotation(ringArc, 270);
  lv_arc_set_bg_angles(ringArc, 0, 360);
  lv_arc_set_range(ringArc, 0, 60);
  lv_obj_remove_style(ringArc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(ringArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(ringArc, 8, 0);
  lv_obj_set_style_arc_color(ringArc, thc(th::TRACK), 0);
  lv_obj_set_style_arc_width(ringArc, 8, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ringArc, thc(theme_accent(active.accent)), LV_PART_INDICATOR);
  lv_arc_set_value(ringArc, 60);

  lblWday = ui_muted(root, "");
  lv_obj_align_to(lblWday, ringArc, LV_ALIGN_CENTER, 0, -38);
  lblTime = lv_label_create(root);
  lv_obj_set_style_text_font(lblTime, &lv_font_montserrat_48, 0);
  lv_label_set_text(lblTime, "--:--");
  lblDate = lv_label_create(root);
  lv_obj_set_style_text_font(lblDate, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblDate, thc(th::ON_MINI), 0);
  lv_label_set_text(lblDate, "");
  lv_obj_align_to(lblTime, ringArc, LV_ALIGN_CENTER, 0, -4);
  lv_obj_align_to(lblDate, ringArc, LV_ALIGN_CENTER, 0, 34);
  lblSec = nullptr;
  if (active.shortcuts) {
    lblWin = makeChip(14, 174, 143, 34, false, PG_USAGE);
    lblMusic = makeChip(163, 174, 143, 34, true, PG_SPOTIFY);
  }
}

static const int FACE = 176, CX = 88;

static void buildAnalog() {
  face = ui_card(root, 8, 12, FACE, FACE);
  lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
  // Marcas das horas (as de 12, 3, 6 e 9 sao maiores).
  static lv_point_t tick[12][2];
  for (int i = 0; i < 12; i++) {
    float a = i * 30 * DEG2RAD_PI / 180.f;
    int r1 = (i % 3 == 0) ? 64 : 71, r2 = 80;
    tick[i][0] = {(lv_coord_t)(CX + r1 * sinf(a)), (lv_coord_t)(CX - r1 * cosf(a))};
    tick[i][1] = {(lv_coord_t)(CX + r2 * sinf(a)), (lv_coord_t)(CX - r2 * cosf(a))};
    lv_obj_t *t = lv_line_create(face);
    lv_line_set_points(t, tick[i], 2);
    lv_obj_set_style_line_width(t, (i % 3 == 0) ? 3 : 2, 0);
    lv_obj_set_style_line_color(t, thc(th::MUTED), 0);
    lv_obj_set_style_line_rounded(t, true, 0);
  }
  static const int W[3] = {5, 3, 2};
  for (int i = 0; i < 3; i++) {
    hand[i] = lv_line_create(face);
    lv_obj_set_style_line_width(hand[i], W[i], 0);
    lv_obj_set_style_line_rounded(hand[i], true, 0);
    lv_obj_set_style_line_color(hand[i], thc(i == 2 ? theme_accent(active.accent) : th::TEXT), 0);
    handPts[i][0] = {CX, CX};
    handPts[i][1] = {CX, CX - 40};
    lv_line_set_points(hand[i], handPts[i], 2);
  }
  if (!active.sec) lv_obj_add_flag(hand[2], LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *hub = ui_dot(face, 10, theme_accent(active.accent));
  lv_obj_align(hub, LV_ALIGN_CENTER, 0, 0);

  lblWday = ui_muted(root, "", &lv_font_montserrat_14);
  lv_obj_set_pos(lblWday, 198, 18);
  lblDate = lv_label_create(root);
  lv_obj_set_style_text_font(lblDate, &lv_font_montserrat_20, 0);
  lv_obj_set_pos(lblDate, 198, 38);
  lv_label_set_text(lblDate, "");
  lblTime = lv_label_create(root);
  lv_obj_set_style_text_font(lblTime, &lv_font_montserrat_28, 0);
  lv_obj_set_pos(lblTime, 198, 70);
  lv_label_set_text(lblTime, "--:--");
  lblSec = nullptr;
  if (active.shortcuts) {
    lblWin = makeChip(198, 118, 114, 34, false, PG_USAGE);
    lblMusic = makeChip(198, 158, 114, 34, true, PG_SPOTIFY);
  }
}

static void moveHand(int i, float deg, int len) {
  float a = deg * DEG2RAD_PI / 180.f;
  handPts[i][1] = {(lv_coord_t)(CX + len * sinf(a)), (lv_coord_t)(CX - len * cosf(a))};
  lv_line_set_points(hand[i], handPts[i], 2);
}

static void updateChips() {
  if (!lblWin) return;
  char buf[64];
  ProviderUsage u = usage_get(PROV_CLAUDE);
  bool values = u.source == UsageSource::Direct || u.source == UsageSource::Cache;
  uint32_t now;
  if (u.source == UsageSource::NoAccount) snprintf(buf, sizeof(buf), compactChips ? "Sem conta" : "Claude sem conta");
  else if (values && u.win[0].reset_epoch && board_epoch_now(&now) && u.win[0].reset_epoch > now) {
    char t[16];
    board_format_time(u.win[0].reset_epoch, t, sizeof(t));
    snprintf(buf, sizeof(buf), compactChips ? "%s - %s" : "%s reinicia %s", u.win[0].name, t);
  } else if (values) snprintf(buf, sizeof(buf), "%s livre", u.win[0].name);
  else snprintf(buf, sizeof(buf), "Claude --");
  setIfChanged(lblWin, buf);

  SpotifyState s = spotify_get();
  if (!s.linked) snprintf(buf, sizeof(buf), "Spotify sem conta");
  else if (s.active && s.title[0]) snprintf(buf, sizeof(buf), "%s", s.title);
  else snprintf(buf, sizeof(buf), "Nada tocando");
  setIfChanged(lblMusic, buf);
}

// force: redesenha mesmo com a pagina fora da tela (apos mudar uma opcao).
static void update(bool force) {
  if (!root) return;
  if (!force && (!ui_page_shown(PG_CLOCK) || ui_is_animating())) return;

  board_datetime_t dt;
  bool ok = board_local_datetime(&dt);
  char buf[48];
  int hour = ok ? dt.hour : 0;
  const char *ampm = "";
  if (ok && !active.h24) {
    ampm = hour >= 12 ? "PM" : "AM";
    hour = hour % 12 == 0 ? 12 : hour % 12;
  }

  if (!ok) {
    setIfChanged(lblTime, "--:--");
    setIfChanged(lblSec, "");
    setIfChanged(lblWday, "Aguardando a hora da rede");
    setIfChanged(lblDate, "");
  } else if (dt.minute != shownMin || force) {
    shownMin = dt.minute;
    if (active.h24) snprintf(buf, sizeof(buf), "%02d:%02d", hour, dt.minute);
    else snprintf(buf, sizeof(buf), "%d:%02d", hour, dt.minute);
    setIfChanged(lblTime, buf);
    if (active.style == ST_MINIMAL) {
      snprintf(buf, sizeof(buf), "%s - %d %s", WDAYS3[dt.wday % 7], dt.mday, MONTHS3[(dt.month + 11) % 12]);
    } else if (active.style == ST_ANALOG) {
      snprintf(buf, sizeof(buf), "%d %s", dt.mday, MONTHS3L[(dt.month + 11) % 12]);
    } else {
      snprintf(buf, sizeof(buf), "%d de %s", dt.mday, MONTHS[(dt.month + 11) % 12]);
    }
    setIfChanged(lblDate, buf);
    if (ampm[0]) snprintf(buf, sizeof(buf), "%s - %s", WDAYS[dt.wday % 7], ampm);
    else snprintf(buf, sizeof(buf), "%s", WDAYS[dt.wday % 7]);
    setIfChanged(lblWday, buf);
    if (active.style == ST_DIGITAL || active.style == ST_MINIMAL) placeTime();
    if (active.style == ST_RING) {
      lv_obj_align_to(lblTime, ringArc, LV_ALIGN_CENTER, 0, -4);
      lv_obj_align_to(lblWday, ringArc, LV_ALIGN_CENTER, 0, -38);
      lv_obj_align_to(lblDate, ringArc, LV_ALIGN_CENTER, 0, 34);
    }
  }
  if (ok && (dt.second != shownSec || force)) {
    shownSec = dt.second;
    if (lblSec && active.sec) {
      snprintf(buf, sizeof(buf), "%02d", dt.second);
      setIfChanged(lblSec, buf);
    }
    if (ringArc) lv_arc_set_value(ringArc, active.sec ? dt.second + 1 : 60);
    if (face) {
      moveHand(0, (dt.hour % 12 + dt.minute / 60.f) * 30.f, 40);
      moveHand(1, (dt.minute + dt.second / 60.f) * 6.f, 62);
      moveHand(2, dt.second * 6.f, 70);
    }
  }
  updateChips();
}

static void refreshCb(lv_timer_t *) { update(false); }

static void buildStyle() {
  active = readCfg();
  if (!root) return;  // pagina desmontada (opcoes abertas de outra pagina): a montagem le tudo de novo
  lv_obj_clean(root);
  lblWday = lblTime = lblSec = lblDate = lblWin = lblMusic = face = ringArc = nullptr;
  hand[0] = hand[1] = hand[2] = nullptr;
  shownMin = shownSec = -1;
  compactChips = active.style == ST_ANALOG;
  switch (active.style) {
    case ST_ANALOG: buildAnalog(); break;
    case ST_MINIMAL: buildMinimal(); break;
    case ST_RING: buildRing(); break;
    default: buildDigital(); break;
  }
  update(true);
}

static void gearCb(lv_event_t *) { clock_ui_open_settings(); }

// Paginas sob demanda: o ui.cpp apaga os filhos da tile (lv_obj_clean) depois disto.
void clock_ui_unbuild_page() {
  if (clockTimer) lv_timer_del(clockTimer);
  clockTimer = nullptr;
  root = nullptr;
  lblWday = lblTime = lblSec = lblDate = lblWin = lblMusic = face = ringArc = nullptr;
  hand[0] = hand[1] = hand[2] = nullptr;
  shownMin = shownSec = -1;
}

void clock_ui_build_page(lv_obj_t *tile) {
  root = lv_obj_create(tile);
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, 320, 240);
  lv_obj_set_pos(root, 0, 0);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  buildStyle();
  clockTimer = lv_timer_create(refreshCb, 250, nullptr);
  // Engrenagem no canto de baixo a direita (a esquerda e a barra de status, no meio os pontos).
  ui_gear_btn(tile, gearCb);
}

// ---- opcoes do Relogio -------------------------------------------------------

static lv_obj_t *optScreen, *tileCard[ST_COUNT], *tileName[ST_COUNT], *swatch[4];

static void styleSelection() {
  for (int i = 0; i < ST_COUNT; i++) {
    bool on = i == active.style;
    lv_obj_set_style_border_width(tileCard[i], on ? 2 : 0, 0);
    lv_obj_set_style_border_color(tileCard[i], thc(th::ACCENT), 0);
    lv_obj_set_style_text_color(tileName[i], thc(on ? th::ACCENT : th::MUTED), 0);
  }
  for (int i = 0; i < 4; i++) lv_obj_set_style_outline_width(swatch[i], i == active.accent ? 2 : 0, 0);
}

static void tileCb(lv_event_t *e) {
  settings_set_u8(K_STYLE, (uint8_t)(intptr_t)lv_event_get_user_data(e));
  buildStyle();
  styleSelection();
}

static void switchCb(lv_event_t *e) {
  int which = (int)(intptr_t)lv_event_get_user_data(e);
  const char *keys[3] = {K_24H, K_SEC, K_SHORT};
  settings_set_u8(keys[which], lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0);
  buildStyle();
}

static void swatchCb(lv_event_t *e) {
  settings_set_u8(K_ACC, (uint8_t)(intptr_t)lv_event_get_user_data(e));
  buildStyle();
  styleSelection();
}

static void optBackCb(lv_event_t *) {
  lv_obj_del_async(optScreen);  // async: o botao clicado e filho dela
  optScreen = nullptr;
  settings_ui_child_closed();
}

// Miniatura de cada estilo, desenhada com formas simples.
static void tilePreview(lv_obj_t *tile, int style) {
  static lv_point_t hA[2] = {{13, 13}, {13, 4}}, hB[2] = {{13, 13}, {20, 15}};
  if (style == ST_DIGITAL || style == ST_MINIMAL) {
    lv_obj_t *l = lv_label_create(tile);
    lv_label_set_text(l, "14:32");
    lv_obj_set_style_text_font(l, style == ST_DIGITAL ? &lv_font_montserrat_16 : &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, thc(th::TEXT), 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, style == ST_DIGITAL ? 10 : 12);
  } else if (style == ST_ANALOG) {
    lv_obj_t *c = ui_dot(tile, 28, th::TRACK);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t *a = lv_line_create(c);
    lv_line_set_points(a, hA, 2);
    lv_obj_set_style_line_width(a, 2, 0);
    lv_obj_set_style_line_color(a, thc(th::TEXT), 0);
    lv_obj_t *b = lv_line_create(c);
    lv_line_set_points(b, hB, 2);
    lv_obj_set_style_line_width(b, 2, 0);
    lv_obj_set_style_line_color(b, thc(th::TEXT), 0);
  } else {
    lv_obj_t *arc = lv_arc_create(tile);
    lv_obj_set_size(arc, 28, 28);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 6);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_value(arc, 70);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 4, 0);
    lv_obj_set_style_arc_width(arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, thc(th::TRACK), 0);
    lv_obj_set_style_arc_color(arc, thc(th::ACCENT), LV_PART_INDICATOR);
  }
}

void clock_ui_open_settings() {
  if (optScreen || !ui_mem_ok(12 * 1024)) return;  // ~7,7 KB medidos (x1,5)
  active = readCfg();
  optScreen = ui_overlay();
  ui_back_header(optScreen, "Relogio", optBackCb);

  lv_obj_t *scroll = lv_obj_create(optScreen);
  lv_obj_remove_style_all(scroll);
  lv_obj_set_size(scroll, 320, 192);
  lv_obj_set_pos(scroll, 0, 46);
  lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);

  for (int i = 0; i < ST_COUNT; i++) {
    tileCard[i] = ui_card(scroll, 10 + i * 76, 0, 70, 58);
    lv_obj_add_flag(tileCard[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tileCard[i], tileCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    tilePreview(tileCard[i], i);
    tileName[i] = lv_label_create(tileCard[i]);
    lv_label_set_text(tileName[i], STYLE_NAMES[i]);
    lv_obj_set_style_text_font(tileName[i], &lv_font_montserrat_12, 0);
    lv_obj_align(tileName[i], LV_ALIGN_BOTTOM_MID, 0, -4);
  }

  lv_obj_t *list = ui_list(scroll, 10, 66, 300);
  static const char *LABELS[3] = {"Formato 24 horas", "Mostrar segundos", "Atalhos (IA e musica)"};
  const uint8_t *vals[3] = {&active.h24, &active.sec, &active.shortcuts};
  for (int i = 0; i < 3; i++) {
    UiRow r = ui_list_row(list, nullptr, LABELS[i], nullptr, nullptr, nullptr, 34);
    lv_obj_t *sw = lv_switch_create(r.row);
    lv_obj_set_size(sw, 40, 22);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(sw, thc(th::TRACK), 0);
    lv_obj_set_style_bg_color(sw, thc(th::FILL), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, thc(th::ON_TONAL), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_ext_click_area(sw, 8);
    if (*vals[i]) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, switchCb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
  }
  UiRow acc = ui_list_row(list, nullptr, "Cor de destaque", nullptr, nullptr, nullptr, 34);
  for (int i = 0; i < 4; i++) {
    swatch[i] = ui_dot(acc.row, 20, theme_accent(i));
    lv_obj_align(swatch[i], LV_ALIGN_RIGHT_MID, -10 - (3 - i) * 28, 0);
    lv_obj_add_flag(swatch[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(swatch[i], 3);
    lv_obj_set_style_outline_color(swatch[i], thc(th::TEXT), 0);
    lv_obj_set_style_outline_pad(swatch[i], 2, 0);
    lv_obj_add_event_cb(swatch[i], swatchCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  styleSelection();
}
