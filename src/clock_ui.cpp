#include "clock_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "clock_font.h"
#include "settings_ui.h"
#include "spotify.h"
#include "theme.h"
#include "ui.h"
#include "usage.h"
#include "usage_ui.h"

static constexpr float DEG2RAD_PI = 3.14159265f;

// ---- opcoes (NVS) ------------------------------------------------------------

enum Style : uint8_t { ST_DIGITAL, ST_ANALOG, ST_MINIMAL, ST_RING, ST_COUNT };
enum SummaryMode : uint8_t { SUM_NONE = 0, SUM_IAS = 1, SUM_BOTH = 2 };

static const char *STYLE_NAMES[ST_COUNT] = {"Digital", "Analogico", "Minimo", "Anel"};
static const char *SUM_NAMES[3] = {"Nada", "IAs", "IA + musica"};
static const char *PROV_SHORT[PROV_COUNT] = {"Claude", "Codex", "AGY"};
static const PageId PROV_PAGE[PROV_COUNT] = {PG_CLAUDE, PG_CODEX, PG_AGY};

static const char *K_STYLE = "clk_style";
static const char *K_24H = "clk_24h";
static const char *K_SEC = "clk_sec";
static const char *K_SHORT = "clk_short";  // SummaryMode: 0 = Nada, 1 = IAs (padrao), 2 = IA + musica
static const char *K_ACC = "clk_acc";

struct Cfg {
  uint8_t style, h24, sec, summary, accent;
};
static Cfg active;

static Cfg readCfg() {
  Cfg c;
  c.style = settings_get_u8(K_STYLE, ST_DIGITAL);
  if (c.style >= ST_COUNT) c.style = ST_DIGITAL;
  c.h24 = settings_get_u8(K_24H, 1);
  c.sec = settings_get_u8(K_SEC, 1);
  c.summary = settings_get_u8(K_SHORT, SUM_IAS);
  if (c.summary > SUM_BOTH) c.summary = SUM_IAS;
  c.accent = settings_get_u8(K_ACC, 0);
  if (c.accent > 3) c.accent = 0;
  return c;
}

void clock_ui_summary(char *buf, int size) {
  Cfg c = readCfg();
  if (c.style == ST_MINIMAL) {
    snprintf(buf, size, "%s - %s", STYLE_NAMES[c.style], c.h24 ? "24 h" : "12 h");
  } else {
    snprintf(buf, size, "%s - %s - %s", STYLE_NAMES[c.style], c.h24 ? "24 h" : "12 h", SUM_NAMES[c.summary]);
  }
}

// Sem acentos: as fontes embutidas nao tem os glifos.
static const char *WDAYS[] = {"Domingo", "Segunda-feira", "Terca-feira", "Quarta-feira",
                              "Quinta-feira", "Sexta-feira", "Sabado"};
static const char *WDAYS_SHORT[] = {"Domingo", "Segunda", "Terca", "Quarta", "Quinta", "Sexta", "Sabado"};
static const char *WDAYS3[] = {"DOM", "SEG", "TER", "QUA", "QUI", "SEX", "SAB"};
static const char *MONTHS[] = {"janeiro", "fevereiro", "marco",    "abril",   "maio",     "junho",
                               "julho",   "agosto",    "setembro", "outubro", "novembro", "dezembro"};
static const char *MONTHS3[] = {"JAN", "FEV", "MAR", "ABR", "MAI", "JUN", "JUL", "AGO", "SET", "OUT", "NOV", "DEZ"};
static const char *MONTHS3L[] = {"jan", "fev", "mar", "abr", "mai", "jun", "jul", "ago", "set", "out", "nov", "dez"};

static uint32_t provColor(Provider p) {
  return p == PROV_CLAUDE ? th::CLAUDE : p == PROV_CODEX ? th::CODEX : th::AGY;
}

static uint32_t barColor(Provider p, float pct) {
  if (pct >= 90) return th::RED;
  if (pct >= 75) return th::AMBER;
  return provColor(p);
}

// ---- pagina ------------------------------------------------------------------

static lv_obj_t *root;
static lv_obj_t *lblWday, *lblTime, *lblSec, *lblDate;
static lv_obj_t *iaArc[PROV_COUNT], *iaPct[PROV_COUNT], *iaName[PROV_COUNT];
static lv_obj_t *bothIaArc, *bothIaLine1, *bothIaLine2, *bothSpLine1, *bothSpLine2;
static lv_obj_t *face, *ringArc, *hand[3];
static lv_point_t handPts[3][2];
static int shownMin = -1, shownSec = -1;
static lv_timer_t *clockTimer;

static void setIfChanged(lv_obj_t *l, const char *text) {
  if (l && strcmp(lv_label_get_text(l), text)) lv_label_set_text(l, text);
}

static void chipCb(lv_event_t *e) { ui_goto((PageId)(intptr_t)lv_event_get_user_data(e)); }

static void placeTime() {
  lv_obj_update_layout(lblTime);
  int tw = lv_obj_get_width(lblTime);
  int sw = lblSec && active.sec ? 32 : 0;
  lv_obj_set_x(lblTime, (320 - tw - sw) / 2);
  if (lblSec) {
    lv_obj_set_x(lblSec, (320 - tw - sw) / 2 + tw + 6);
    lv_obj_set_y(lblSec, lv_obj_get_y(lblTime) + 22);
  }
}

// Constroi o resumo horizontal (Digital e Anel): 3 mini-aneis de IA ou 2 cartoes (IA + musica).
static void buildHorizSummary(int y) {
  if (active.summary == SUM_IAS) {
    Provider ord[PROV_COUNT];
    usage_ui_get_order(ord);
    for (int i = 0; i < PROV_COUNT; i++) {
      Provider p = ord[i];
      lv_obj_t *c = ui_card(root, 12 + i * 101, y, 94, 38);
      lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_bg_color(c, thc(th::ROW_PR), LV_STATE_PRESSED);
      lv_obj_add_event_cb(c, chipCb, LV_EVENT_CLICKED, (void *)(intptr_t)PROV_PAGE[p]);

      iaArc[i] = lv_arc_create(c);
      lv_obj_set_size(iaArc[i], 24, 24);
      lv_obj_align(iaArc[i], LV_ALIGN_LEFT_MID, 7, 0);
      lv_arc_set_rotation(iaArc[i], 270);
      lv_arc_set_bg_angles(iaArc[i], 0, 360);
      lv_arc_set_range(iaArc[i], 0, 100);
      lv_obj_remove_style(iaArc[i], nullptr, LV_PART_KNOB);
      lv_obj_clear_flag(iaArc[i], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_arc_width(iaArc[i], 4, 0);
      lv_obj_set_style_arc_color(iaArc[i], thc(th::TRACK), 0);
      lv_obj_set_style_arc_width(iaArc[i], 4, LV_PART_INDICATOR);
      lv_obj_set_style_arc_color(iaArc[i], thc(provColor(p)), LV_PART_INDICATOR);

      iaName[i] = ui_muted(c, PROV_SHORT[p]);
      lv_obj_set_pos(iaName[i], 37, 4);

      iaPct[i] = lv_label_create(c);
      lv_obj_set_style_text_font(iaPct[i], &lv_font_montserrat_14, 0);
      lv_obj_set_pos(iaPct[i], 37, 18);
      lv_label_set_text(iaPct[i], "--");
    }
  } else if (active.summary == SUM_BOTH) {
    Provider p = usage_ui_current_provider();
    lv_obj_t *c1 = ui_card(root, 12, y, 145, 38);
    lv_obj_add_flag(c1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c1, thc(th::ROW_PR), LV_STATE_PRESSED);
    lv_obj_add_event_cb(c1, chipCb, LV_EVENT_CLICKED, (void *)(intptr_t)PROV_PAGE[p]);

    bothIaArc = lv_arc_create(c1);
    lv_obj_set_size(bothIaArc, 24, 24);
    lv_obj_align(bothIaArc, LV_ALIGN_LEFT_MID, 8, 0);
    lv_arc_set_rotation(bothIaArc, 270);
    lv_arc_set_bg_angles(bothIaArc, 0, 360);
    lv_arc_set_range(bothIaArc, 0, 100);
    lv_obj_remove_style(bothIaArc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(bothIaArc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(bothIaArc, 4, 0);
    lv_obj_set_style_arc_color(bothIaArc, thc(th::TRACK), 0);
    lv_obj_set_style_arc_width(bothIaArc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(bothIaArc, thc(provColor(p)), LV_PART_INDICATOR);

    bothIaLine1 = lv_label_create(c1);
    lv_obj_set_style_text_font(bothIaLine1, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(bothIaLine1, LV_LABEL_LONG_DOT);
    lv_obj_set_size(bothIaLine1, 98, 15);
    lv_obj_set_pos(bothIaLine1, 38, 4);
    lv_label_set_text(bothIaLine1, "");

    bothIaLine2 = ui_muted(c1, "");
    lv_label_set_long_mode(bothIaLine2, LV_LABEL_LONG_DOT);
    lv_obj_set_size(bothIaLine2, 98, 15);
    lv_obj_set_pos(bothIaLine2, 38, 20);

    lv_obj_t *c2 = ui_card(root, 163, y, 145, 38);
    lv_obj_add_flag(c2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c2, thc(th::ROW_PR), LV_STATE_PRESSED);
    lv_obj_add_event_cb(c2, chipCb, LV_EVENT_CLICKED, (void *)(intptr_t)PG_SPOTIFY);

    lv_obj_t *spBadge = ui_dot(c2, 24, th::OK_BG);
    lv_obj_align(spBadge, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t *spIc = lv_label_create(spBadge);
    lv_label_set_text(spIc, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(spIc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(spIc, thc(th::GREEN), 0);
    lv_obj_center(spIc);

    bothSpLine1 = lv_label_create(c2);
    lv_obj_set_style_text_font(bothSpLine1, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(bothSpLine1, LV_LABEL_LONG_DOT);
    lv_obj_set_size(bothSpLine1, 98, 15);
    lv_obj_set_pos(bothSpLine1, 38, 4);
    lv_label_set_text(bothSpLine1, "");

    bothSpLine2 = ui_muted(c2, "");
    lv_label_set_long_mode(bothSpLine2, LV_LABEL_LONG_DOT);
    lv_obj_set_size(bothSpLine2, 98, 15);
    lv_obj_set_pos(bothSpLine2, 38, 20);
  }
}

static void buildDigital() {
  bool hasSum = active.summary != SUM_NONE;
  lblWday = ui_muted(root, "", &lv_font_montserrat_14);
  lv_obj_set_pos(lblWday, 14, 12);
  lblTime = lv_label_create(root);
  lv_obj_set_style_text_font(lblTime, &font_clock_big, 0);
  lv_obj_set_pos(lblTime, 0, hasSum ? 18 : 34);
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
  lv_obj_set_pos(lblDate, 0, hasSum ? 116 : 136);
  lv_label_set_text(lblDate, "");
  buildHorizSummary(174);
  placeTime();
}

static void buildMinimal() {
  lblTime = lv_label_create(root);
  lv_obj_set_style_text_font(lblTime, &font_clock_huge, 0);
  lv_obj_set_pos(lblTime, 0, 26);
  lv_label_set_text(lblTime, "--:--");
  lblDate = lv_label_create(root);
  lv_obj_set_style_text_font(lblDate, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lblDate, thc(th::MUTED), 0);
  lv_obj_set_style_text_letter_space(lblDate, 2, 0);
  lv_obj_set_width(lblDate, 320);
  lv_obj_set_style_text_align(lblDate, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(lblDate, 0, 162);
  lv_label_set_text(lblDate, "");
  lblSec = nullptr;
  placeTime();
}

static void buildRing() {
  bool hasSum = active.summary != SUM_NONE;
  int sz = hasSum ? 152 : 168;
  ringArc = lv_arc_create(root);
  lv_obj_set_size(ringArc, sz, sz);
  lv_obj_set_pos(ringArc, (320 - sz) / 2, hasSum ? 12 : 26);
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
  lv_obj_align_to(lblWday, ringArc, LV_ALIGN_CENTER, 0, -34);
  lblTime = lv_label_create(root);
  lv_obj_set_style_text_font(lblTime, &lv_font_montserrat_48, 0);
  lv_label_set_text(lblTime, "--:--");
  lblDate = lv_label_create(root);
  lv_obj_set_style_text_font(lblDate, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblDate, thc(th::ON_MINI), 0);
  lv_label_set_text(lblDate, "");
  lv_obj_align_to(lblTime, ringArc, LV_ALIGN_CENTER, 0, -2);
  lv_obj_align_to(lblDate, ringArc, LV_ALIGN_CENTER, 0, 32);
  lblSec = nullptr;
  buildHorizSummary(174);
}

static const int FACE = 168, CX = 84;

static void buildAnalog() {
  face = ui_card(root, 10, 26, FACE, FACE);
  lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
  static lv_point_t tick[12][2];
  for (int i = 0; i < 12; i++) {
    float a = i * 30 * DEG2RAD_PI / 180.f;
    int r1 = (i % 3 == 0) ? 62 : 68, r2 = 76;
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
    handPts[i][1] = {CX, CX - 38};
    lv_line_set_points(hand[i], handPts[i], 2);
  }
  if (!active.sec) lv_obj_add_flag(hand[2], LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *hub = ui_dot(face, 10, theme_accent(active.accent));
  lv_obj_align(hub, LV_ALIGN_CENTER, 0, 0);

  lblWday = ui_muted(root, "", &lv_font_montserrat_14);
  lv_label_set_long_mode(lblWday, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lblWday, 120);
  lv_obj_set_pos(lblWday, 190, 18);

  lblDate = lv_label_create(root);
  lv_obj_set_style_text_font(lblDate, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lblDate, thc(th::ON_MINI), 0);
  lv_label_set_long_mode(lblDate, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lblDate, 120);
  lv_obj_set_pos(lblDate, 190, 38);
  lv_label_set_text(lblDate, "");

  lblTime = lv_label_create(root);
  lv_obj_set_style_text_font(lblTime, &lv_font_montserrat_28, 0);
  lv_obj_set_pos(lblTime, 190, 60);
  lv_label_set_text(lblTime, "--:--");
  lblSec = nullptr;

  if (active.summary == SUM_IAS) {
    Provider ord[PROV_COUNT];
    usage_ui_get_order(ord);
    for (int i = 0; i < PROV_COUNT; i++) {
      Provider p = ord[i];
      lv_obj_t *c = ui_card(root, 190, 98 + i * 36, 120, 32);
      lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_bg_color(c, thc(th::ROW_PR), LV_STATE_PRESSED);
      lv_obj_add_event_cb(c, chipCb, LV_EVENT_CLICKED, (void *)(intptr_t)PROV_PAGE[p]);

      iaArc[i] = lv_arc_create(c);
      lv_obj_set_size(iaArc[i], 20, 20);
      lv_obj_align(iaArc[i], LV_ALIGN_LEFT_MID, 7, 0);
      lv_arc_set_rotation(iaArc[i], 270);
      lv_arc_set_bg_angles(iaArc[i], 0, 360);
      lv_arc_set_range(iaArc[i], 0, 100);
      lv_obj_remove_style(iaArc[i], nullptr, LV_PART_KNOB);
      lv_obj_clear_flag(iaArc[i], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_arc_width(iaArc[i], 3, 0);
      lv_obj_set_style_arc_color(iaArc[i], thc(th::TRACK), 0);
      lv_obj_set_style_arc_width(iaArc[i], 3, LV_PART_INDICATOR);
      lv_obj_set_style_arc_color(iaArc[i], thc(provColor(p)), LV_PART_INDICATOR);

      iaName[i] = ui_muted(c, PROV_SHORT[p]);
      lv_obj_align(iaName[i], LV_ALIGN_LEFT_MID, 33, 0);

      iaPct[i] = lv_label_create(c);
      lv_obj_set_style_text_font(iaPct[i], &lv_font_montserrat_14, 0);
      lv_obj_align(iaPct[i], LV_ALIGN_RIGHT_MID, -8, 0);
      lv_label_set_text(iaPct[i], "--");
    }
  } else if (active.summary == SUM_BOTH) {
    Provider p = usage_ui_current_provider();
    lv_obj_t *c1 = ui_card(root, 190, 108, 120, 40);
    lv_obj_add_flag(c1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c1, thc(th::ROW_PR), LV_STATE_PRESSED);
    lv_obj_add_event_cb(c1, chipCb, LV_EVENT_CLICKED, (void *)(intptr_t)PROV_PAGE[p]);

    lv_obj_t *d = ui_dot(c1, 8, provColor(p));
    lv_obj_set_pos(d, 9, 8);
    bothIaLine1 = lv_label_create(c1);
    lv_obj_set_style_text_font(bothIaLine1, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(bothIaLine1, LV_LABEL_LONG_DOT);
    lv_obj_set_size(bothIaLine1, 88, 15);
    lv_obj_set_pos(bothIaLine1, 22, 4);
    lv_label_set_text(bothIaLine1, "");

    bothIaLine2 = ui_muted(c1, "");
    lv_label_set_long_mode(bothIaLine2, LV_LABEL_LONG_DOT);
    lv_obj_set_size(bothIaLine2, 98, 15);
    lv_obj_set_pos(bothIaLine2, 9, 21);

    lv_obj_t *c2 = ui_card(root, 190, 156, 120, 40);
    lv_obj_add_flag(c2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c2, thc(th::ROW_PR), LV_STATE_PRESSED);
    lv_obj_add_event_cb(c2, chipCb, LV_EVENT_CLICKED, (void *)(intptr_t)PG_SPOTIFY);

    lv_obj_t *ic = lv_label_create(c2);
    lv_label_set_text(ic, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ic, thc(th::GREEN), 0);
    lv_obj_set_pos(ic, 9, 5);

    bothSpLine1 = lv_label_create(c2);
    lv_obj_set_style_text_font(bothSpLine1, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(bothSpLine1, LV_LABEL_LONG_DOT);
    lv_obj_set_size(bothSpLine1, 86, 15);
    lv_obj_set_pos(bothSpLine1, 24, 4);
    lv_label_set_text(bothSpLine1, "");

    bothSpLine2 = ui_muted(c2, "");
    lv_label_set_long_mode(bothSpLine2, LV_LABEL_LONG_DOT);
    lv_obj_set_size(bothSpLine2, 102, 15);
    lv_obj_set_pos(bothSpLine2, 9, 21);
  }
}

static void moveHand(int i, float deg, int len) {
  float a = deg * DEG2RAD_PI / 180.f;
  handPts[i][1] = {(lv_coord_t)(CX + len * sinf(a)), (lv_coord_t)(CX - len * cosf(a))};
  lv_line_set_points(hand[i], handPts[i], 2);
}

static void updateSummary() {
  if (active.style == ST_MINIMAL || active.summary == SUM_NONE) return;

  if (active.summary == SUM_IAS) {
    Provider ord[PROV_COUNT];
    usage_ui_get_order(ord);
    for (int i = 0; i < PROV_COUNT; i++) {
      if (!iaPct[i]) continue;
      Provider p = ord[i];
      ProviderUsage u = usage_get(p);
      bool values = (u.source == UsageSource::Direct || u.source == UsageSource::Cache) && u.win[0].pct >= 0;
      uint32_t now;
      bool closed = values && u.win[0].reset_epoch && board_epoch_now(&now) && u.win[0].reset_epoch <= now;
      float pct = closed ? 0.0f : (values ? u.win[0].pct : 0.0f);
      char buf[12];
      if (values) snprintf(buf, sizeof(buf), "%d%%", (int)(pct + 0.5f));
      else snprintf(buf, sizeof(buf), "--");
      setIfChanged(iaPct[i], buf);
      if (iaArc[i]) {
        lv_arc_set_value(iaArc[i], values ? (int)(pct + 0.5f) : 0);
        lv_obj_set_style_arc_color(iaArc[i], thc(barColor(p, pct)), LV_PART_INDICATOR);
      }
    }
    return;
  }

  // SUM_BOTH: primeira IA da coluna + Spotify
  Provider p = usage_ui_current_provider();
  ProviderUsage u = usage_get(p);
  bool values = (u.source == UsageSource::Direct || u.source == UsageSource::Cache) && u.win[0].pct >= 0;
  uint32_t now;
  bool closed = values && u.win[0].reset_epoch && board_epoch_now(&now) && u.win[0].reset_epoch <= now;
  float pct = closed ? 0.0f : (values ? u.win[0].pct : 0.0f);

  if (bothIaArc) {
    lv_arc_set_value(bothIaArc, values ? (int)(pct + 0.5f) : 0);
    lv_obj_set_style_arc_color(bothIaArc, thc(barColor(p, pct)), LV_PART_INDICATOR);
  }
  char l1[40], l2[40];
  if (u.source == UsageSource::NoAccount) {
    snprintf(l1, sizeof(l1), "%s", PROV_SHORT[p]);
    snprintf(l2, sizeof(l2), "Sem conta");
  } else if (values) {
    snprintf(l1, sizeof(l1), "%s - %d%%", PROV_SHORT[p], (int)(pct + 0.5f));
    if (!closed && u.win[0].reset_epoch) {
      char t[16];
      board_format_time(u.win[0].reset_epoch, t, sizeof(t));
      snprintf(l2, sizeof(l2), "zera %s", t);
    } else {
      snprintf(l2, sizeof(l2), "janela livre");
    }
  } else {
    snprintf(l1, sizeof(l1), "%s", PROV_SHORT[p]);
    snprintf(l2, sizeof(l2), u.source == UsageSource::Failed ? "Falhou" : "Aguardando");
  }
  setIfChanged(bothIaLine1, l1);
  setIfChanged(bothIaLine2, l2);

  SpotifyState s = spotify_get();
  if (!s.linked) {
    setIfChanged(bothSpLine1, "Spotify");
    setIfChanged(bothSpLine2, "Sem conta");
  } else if (s.active && s.title[0]) {
    setIfChanged(bothSpLine1, s.title);
    setIfChanged(bothSpLine2, s.artist[0] ? s.artist : "Tocando");
  } else {
    setIfChanged(bothSpLine1, "Nada tocando");
    setIfChanged(bothSpLine2, "Spotify");
  }
}

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
    setIfChanged(lblWday, active.style == ST_ANALOG || active.style == ST_RING ? "Sem hora" : "Aguardando a hora da rede");
    setIfChanged(lblDate, "");
  } else if (dt.minute != shownMin || force) {
    shownMin = dt.minute;
    if (active.h24) snprintf(buf, sizeof(buf), "%02d:%02d", hour, dt.minute);
    else snprintf(buf, sizeof(buf), "%d:%02d", hour, dt.minute);
    setIfChanged(lblTime, buf);

    int mIdx = (dt.month + 11) % 12;
    int wIdx = dt.wday % 7;
    if (active.style == ST_MINIMAL) {
      if (ampm[0]) snprintf(buf, sizeof(buf), "%s - %d %s - %s", WDAYS3[wIdx], dt.mday, MONTHS3[mIdx], ampm);
      else snprintf(buf, sizeof(buf), "%s - %d %s", WDAYS3[wIdx], dt.mday, MONTHS3[mIdx]);
    } else if (active.style == ST_ANALOG || active.style == ST_RING) {
      // No Analogico (coluna de 120 px) e no Anel (corda interna de 114 px), abrevia o mes
      // para nunca extrapolar nem cruzar o arco.
      snprintf(buf, sizeof(buf), "%d de %s", dt.mday, MONTHS3L[mIdx]);
    } else {
      // Digital: abrevia meses longos se necessario, ou usa o nome quando cabe com folga.
      const char *mName = strlen(MONTHS[mIdx]) > 7 ? MONTHS3L[mIdx] : MONTHS[mIdx];
      snprintf(buf, sizeof(buf), "%d de %s", dt.mday, mName);
    }
    setIfChanged(lblDate, buf);

    if (lblWday) {
      if (active.style == ST_ANALOG || active.style == ST_RING) {
        if (ampm[0]) snprintf(buf, sizeof(buf), "%s - %s", WDAYS_SHORT[wIdx], ampm);
        else snprintf(buf, sizeof(buf), "%s", WDAYS_SHORT[wIdx]);
      } else {
        if (ampm[0]) snprintf(buf, sizeof(buf), "%s - %s", WDAYS[wIdx], ampm);
        else snprintf(buf, sizeof(buf), "%s", WDAYS[wIdx]);
      }
      setIfChanged(lblWday, buf);
    }

    if (active.style == ST_DIGITAL || active.style == ST_MINIMAL) placeTime();
    if (active.style == ST_RING) {
      lv_obj_align_to(lblTime, ringArc, LV_ALIGN_CENTER, 0, -2);
      lv_obj_align_to(lblWday, ringArc, LV_ALIGN_CENTER, 0, -34);
      lv_obj_align_to(lblDate, ringArc, LV_ALIGN_CENTER, 0, 32);
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
      moveHand(0, (dt.hour % 12 + dt.minute / 60.f) * 30.f, 38);
      moveHand(1, (dt.minute + dt.second / 60.f) * 6.f, 58);
      moveHand(2, dt.second * 6.f, 66);
    }
  }
  updateSummary();
}

static void refreshCb(lv_timer_t *) { update(false); }

static void clearSummaryPtrs() {
  memset(iaArc, 0, sizeof(iaArc));
  memset(iaPct, 0, sizeof(iaPct));
  memset(iaName, 0, sizeof(iaName));
  bothIaArc = bothIaLine1 = bothIaLine2 = bothSpLine1 = bothSpLine2 = nullptr;
}

static void buildStyle() {
  active = readCfg();
  if (!root) return;
  lv_obj_clean(root);
  lblWday = lblTime = lblSec = lblDate = face = ringArc = nullptr;
  hand[0] = hand[1] = hand[2] = nullptr;
  clearSummaryPtrs();
  shownMin = shownSec = -1;
  switch (active.style) {
    case ST_ANALOG: buildAnalog(); break;
    case ST_MINIMAL: buildMinimal(); break;
    case ST_RING: buildRing(); break;
    default: buildDigital(); break;
  }
  update(true);
}

void clock_ui_unbuild_page() {
  if (clockTimer) lv_timer_del(clockTimer);
  clockTimer = nullptr;
  root = nullptr;
  lblWday = lblTime = lblSec = lblDate = face = ringArc = nullptr;
  hand[0] = hand[1] = hand[2] = nullptr;
  clearSummaryPtrs();
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
}

// ---- opcoes do Relogio -------------------------------------------------------

static lv_obj_t *optScreen, *tileCard[ST_COUNT], *tileName[ST_COUNT], *swatch[4];
static lv_obj_t *secTitle, *secSub, *swSec, *sumTitle, *sumSub, *segSum[3];

static void styleSelection() {
  if (!optScreen) return;
  for (int i = 0; i < ST_COUNT; i++) {
    bool on = i == active.style;
    lv_obj_set_style_bg_color(tileCard[i], thc(on ? th::CARD_HI : th::CARD), 0);
    lv_obj_set_style_border_width(tileCard[i], on ? 2 : 0, 0);
    lv_obj_set_style_border_color(tileCard[i], thc(th::ACCENT), 0);
    lv_obj_set_style_text_color(tileName[i], thc(on ? th::ON_TONAL : th::MUTED), 0);
  }
  for (int i = 0; i < 4; i++) lv_obj_set_style_outline_width(swatch[i], i == active.accent ? 2 : 0, 0);

  bool min = active.style == ST_MINIMAL;
  lv_obj_set_style_text_color(secTitle, thc(min ? th::OFF_TEXT : th::TEXT), 0);
  lv_label_set_text(secSub, min ? "O Minimo nao mostra segundos" : "Visivel no relogio");
  if (min) {
    lv_obj_clear_state(swSec, LV_STATE_CHECKED);
    lv_obj_clear_flag(swSec, LV_OBJ_FLAG_CLICKABLE);
  } else {
    if (active.sec) lv_obj_add_state(swSec, LV_STATE_CHECKED);
    else lv_obj_clear_state(swSec, LV_STATE_CHECKED);
    lv_obj_add_flag(swSec, LV_OBJ_FLAG_CLICKABLE);
  }

  lv_obj_set_style_text_color(sumTitle, thc(min ? th::OFF_TEXT : th::TEXT), 0);
  lv_label_set_text(sumSub, min ? "O Minimo fica so com a hora" : "");
  static const SummaryMode MODES[3] = {SUM_IAS, SUM_BOTH, SUM_NONE};
  for (int i = 0; i < 3; i++) {
    lv_obj_t *b = lv_obj_get_parent(segSum[i]);
    bool on = !min && active.summary == MODES[i];
    lv_obj_set_style_bg_color(b, thc(on ? th::TONAL : th::MINI), 0);
    lv_obj_set_style_text_color(b, thc(min ? th::OFF_TEXT : (on ? th::ON_TONAL : th::MUTED)), 0);
    if (min) lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  }
}

static void tileCb(lv_event_t *e) {
  settings_set_u8(K_STYLE, (uint8_t)(intptr_t)lv_event_get_user_data(e));
  buildStyle();
  styleSelection();
}

static void sw24Cb(lv_event_t *e) {
  settings_set_u8(K_24H, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0);
  buildStyle();
}

static void swSecCb(lv_event_t *e) {
  if (active.style == ST_MINIMAL) return;
  settings_set_u8(K_SEC, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0);
  buildStyle();
}

static void sumSegCb(lv_event_t *e) {
  if (active.style == ST_MINIMAL) return;
  settings_set_u8(K_SHORT, (uint8_t)(intptr_t)lv_event_get_user_data(e));
  buildStyle();
  styleSelection();
}

static void swatchCb(lv_event_t *e) {
  settings_set_u8(K_ACC, (uint8_t)(intptr_t)lv_event_get_user_data(e));
  buildStyle();
  styleSelection();
}

static void optBackCb(lv_event_t *) {
  lv_obj_del_async(optScreen);
  optScreen = nullptr;
  settings_ui_child_closed();
}

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
  if (optScreen || !ui_mem_ok(12 * 1024)) return;
  active = readCfg();
  optScreen = ui_overlay();
  ui_back_header(optScreen, "Relogio", optBackCb);

  lv_obj_t *scroll = lv_obj_create(optScreen);
  lv_obj_remove_style_all(scroll);
  lv_obj_set_size(scroll, 320, 194);
  lv_obj_set_pos(scroll, 0, 44);
  lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_bottom(scroll, 12, 0);

  for (int i = 0; i < ST_COUNT; i++) {
    tileCard[i] = ui_card(scroll, 10 + i * 76, 0, 70, 54);
    lv_obj_add_flag(tileCard[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tileCard[i], tileCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    tilePreview(tileCard[i], i);
    tileName[i] = lv_label_create(tileCard[i]);
    lv_label_set_text(tileName[i], STYLE_NAMES[i]);
    lv_obj_set_style_text_font(tileName[i], &lv_font_montserrat_12, 0);
    lv_obj_align(tileName[i], LV_ALIGN_BOTTOM_MID, 0, -4);
  }

  lv_obj_t *list = ui_list(scroll, 10, 60, 300);

  UiRow r24 = ui_list_row(list, nullptr, "Formato 24 horas", nullptr, nullptr, nullptr, 34);
  lv_obj_t *sw24 = lv_switch_create(r24.row);
  lv_obj_set_size(sw24, 40, 22);
  lv_obj_align(sw24, LV_ALIGN_RIGHT_MID, -10, 0);
  lv_obj_set_style_bg_color(sw24, thc(th::TRACK), 0);
  lv_obj_set_style_bg_color(sw24, thc(th::FILL), LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(sw24, thc(th::ON_TONAL), LV_PART_KNOB | LV_STATE_CHECKED);
  lv_obj_set_ext_click_area(sw24, 8);
  if (active.h24) lv_obj_add_state(sw24, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw24, sw24Cb, LV_EVENT_VALUE_CHANGED, nullptr);

  UiRow rSec = ui_list_row(list, nullptr, "Mostrar segundos", "Visivel no relogio", nullptr, nullptr, 38);
  secTitle = rSec.title;
  secSub = rSec.sub;
  swSec = lv_switch_create(rSec.row);
  lv_obj_set_size(swSec, 40, 22);
  lv_obj_align(swSec, LV_ALIGN_RIGHT_MID, -10, 0);
  lv_obj_set_style_bg_color(swSec, thc(th::TRACK), 0);
  lv_obj_set_style_bg_color(swSec, thc(th::FILL), LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(swSec, thc(th::ON_TONAL), LV_PART_KNOB | LV_STATE_CHECKED);
  lv_obj_set_ext_click_area(swSec, 8);
  lv_obj_add_event_cb(swSec, swSecCb, LV_EVENT_VALUE_CHANGED, nullptr);

  UiRow rSum = ui_list_row(list, nullptr, "Resumo embaixo", nullptr, nullptr, nullptr, 56);
  sumTitle = rSum.title;
  lv_obj_align(sumTitle, LV_ALIGN_TOP_LEFT, 10, 4);
  sumSub = ui_muted(rSum.row, "");
  lv_obj_align(sumSub, LV_ALIGN_TOP_RIGHT, -10, 6);
  segSum[0] = ui_tonal_btn(rSum.row, 10, 24, 74, "IAs", sumSegCb, (void *)(intptr_t)SUM_IAS, 26);
  segSum[1] = ui_tonal_btn(rSum.row, 90, 24, 118, "IA + musica", sumSegCb, (void *)(intptr_t)SUM_BOTH, 26);
  segSum[2] = ui_tonal_btn(rSum.row, 214, 24, 76, "Nada", sumSegCb, (void *)(intptr_t)SUM_NONE, 26);

  UiRow acc = ui_list_row(list, nullptr, "Cor de destaque", nullptr, nullptr, nullptr, 40);
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
