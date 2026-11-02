#include "theme.h"

#include "board.h"

namespace th {
#define X(name, dark, light) uint32_t name = dark;
TH_TOKENS(X)
#undef X
}  // namespace th

static const char *NVS_THEME = "theme";  // 1 = claro
static bool lightTheme = false;

bool theme_is_light() { return lightTheme; }
void theme_set_light(bool light) { settings_set_u8(NVS_THEME, light ? 1 : 0); }

uint32_t theme_accent(int idx) {
  static const uint32_t DARK[4] = {0x8AB4F8, 0xD97757, 0x6DD58C, 0x9AA0A6};
  static const uint32_t LIGHT[4] = {0x0B57D0, 0xB5532F, 0x1E8E3E, 0x5B636D};
  if (idx < 0 || idx > 3) idx = 0;
  return lightTheme ? LIGHT[idx] : DARK[idx];
}

// ---- estilos compartilhados ---------------------------------------------------
// Um `lv_style_t` global usado por muitos objetos custa quase nada no pool do LVGL (so uma
// entrada por objeto); `lv_obj_set_style_*` cria uma copia local por objeto, e com dezenas de
// objetos por tela isso estourava o pool de 56 KB (o painel reiniciava ao abrir as
// Configuracoes). Por isso os componentes abaixo usam estes estilos, e so a cor que varia de
// objeto para objeto (ui_dot, ui_card) fica local.

static lv_style_t sCard, sDot, sRow, sRowPr, sDivider, sIconLbl, sIconBadge, sTitle14, sTitle20, sMuted12, sMuted14;
static lv_style_t sGear, sGearPr;
static lv_style_t sTonal, sTonalPr, sMini, sMiniPr, sDanger, sDangerPr, sOverlay, sBackBtn, sBackPr, sBackArrow;

static void initStyles() {
  lv_style_init(&sCard);
  lv_style_set_radius(&sCard, 10);
  lv_style_set_bg_opa(&sCard, LV_OPA_COVER);
  lv_style_set_text_color(&sCard, thc(th::TEXT));

  lv_style_init(&sDot);
  lv_style_set_radius(&sDot, LV_RADIUS_CIRCLE);
  lv_style_set_bg_opa(&sDot, LV_OPA_COVER);

  lv_style_init(&sRow);
  lv_style_set_radius(&sRow, 10);
  lv_style_init(&sRowPr);
  lv_style_set_bg_color(&sRowPr, thc(th::ROW_PR));
  lv_style_set_bg_opa(&sRowPr, LV_OPA_COVER);
  lv_style_init(&sDivider);
  lv_style_set_border_side(&sDivider, LV_BORDER_SIDE_TOP);
  lv_style_set_border_width(&sDivider, 1);
  lv_style_set_border_color(&sDivider, thc(th::LINE));

  lv_style_init(&sIconLbl);
  lv_style_set_text_font(&sIconLbl, &lv_font_montserrat_12);
  lv_style_set_text_color(&sIconLbl, thc(th::ON_TONAL));
  lv_style_init(&sIconBadge);
  lv_style_set_radius(&sIconBadge, LV_RADIUS_CIRCLE);
  lv_style_set_bg_opa(&sIconBadge, LV_OPA_COVER);
  lv_style_set_bg_color(&sIconBadge, thc(th::TONAL));
  lv_style_set_text_align(&sIconBadge, LV_TEXT_ALIGN_CENTER);
  lv_style_set_pad_top(&sIconBadge, 5);  // (24 - altura da linha de 12 px) / 2
  lv_style_init(&sTitle14);
  lv_style_set_text_font(&sTitle14, &lv_font_montserrat_14);
  lv_style_set_text_color(&sTitle14, thc(th::TEXT));
  lv_style_init(&sTitle20);
  lv_style_set_text_font(&sTitle20, &lv_font_montserrat_20);
  lv_style_set_text_color(&sTitle20, thc(th::TEXT));
  lv_style_init(&sMuted12);
  lv_style_set_text_font(&sMuted12, &lv_font_montserrat_12);
  lv_style_set_text_color(&sMuted12, thc(th::MUTED));
  lv_style_init(&sMuted14);
  lv_style_set_text_font(&sMuted14, &lv_font_montserrat_14);
  lv_style_set_text_color(&sMuted14, thc(th::MUTED));

  lv_style_init(&sTonal);
  lv_style_set_radius(&sTonal, LV_RADIUS_CIRCLE);
  lv_style_set_bg_opa(&sTonal, LV_OPA_COVER);
  lv_style_set_bg_color(&sTonal, thc(th::TONAL));
  lv_style_set_text_color(&sTonal, thc(th::ON_TONAL));
  lv_style_set_text_font(&sTonal, &lv_font_montserrat_14);
  lv_style_set_pad_hor(&sTonal, 14);
  lv_style_init(&sTonalPr);
  lv_style_set_bg_color(&sTonalPr, thc(th::TONAL_PR));
  lv_style_init(&sMini);
  lv_style_set_radius(&sMini, LV_RADIUS_CIRCLE);
  lv_style_set_bg_opa(&sMini, LV_OPA_COVER);
  lv_style_set_bg_color(&sMini, thc(th::MINI));
  lv_style_set_text_color(&sMini, thc(th::ON_MINI));
  lv_style_set_text_font(&sMini, &lv_font_montserrat_12);
  lv_style_set_pad_hor(&sMini, 12);
  lv_style_init(&sMiniPr);
  lv_style_set_bg_color(&sMiniPr, thc(th::MINI_PR));
  lv_style_init(&sGear);  // engrenagem das paginas: um rotulo clicavel so (sem botao+rotulo)
  lv_style_set_radius(&sGear, LV_RADIUS_CIRCLE);
  lv_style_set_bg_opa(&sGear, LV_OPA_COVER);
  lv_style_set_bg_color(&sGear, thc(th::MINI));
  lv_style_set_text_color(&sGear, thc(th::ON_MINI));
  lv_style_set_text_font(&sGear, &lv_font_montserrat_14);
  lv_style_set_text_align(&sGear, LV_TEXT_ALIGN_CENTER);
  lv_style_set_pad_top(&sGear, 6);  // (28 - altura da linha de 14 px) / 2
  lv_style_init(&sGearPr);
  lv_style_set_bg_color(&sGearPr, thc(th::MINI_PR));
  lv_style_init(&sDanger);
  lv_style_set_bg_color(&sDanger, thc(th::DANGER));
  lv_style_set_text_color(&sDanger, thc(th::ON_DANGER));
  lv_style_init(&sDangerPr);
  lv_style_set_bg_color(&sDangerPr, thc(th::DANGER_PR));

  lv_style_init(&sOverlay);
  lv_style_set_bg_color(&sOverlay, thc(th::BG));
  lv_style_set_bg_opa(&sOverlay, LV_OPA_COVER);
  lv_style_set_text_color(&sOverlay, thc(th::TEXT));
  lv_style_init(&sBackBtn);
  lv_style_set_radius(&sBackBtn, LV_RADIUS_CIRCLE);
  lv_style_init(&sBackPr);
  lv_style_set_bg_color(&sBackPr, thc(th::ROW_PR));
  lv_style_set_bg_opa(&sBackPr, LV_OPA_COVER);
  lv_style_init(&sBackArrow);
  lv_style_set_text_font(&sBackArrow, &lv_font_montserrat_16);
  lv_style_set_text_color(&sBackArrow, thc(th::TEXT));
}

void theme_init() {
  lightTheme = settings_get_u8(NVS_THEME, 0) != 0;
#define X(name, dark, light) th::name = lightTheme ? light : dark;
  TH_TOKENS(X)
#undef X
  lv_theme_default_init(lv_disp_get_default(), thc(th::ACCENT), thc(th::CODEX), !lightTheme, &lv_font_montserrat_14);
  lv_obj_set_style_bg_color(lv_scr_act(), thc(th::BG), 0);
  lv_obj_set_style_text_color(lv_scr_act(), thc(th::TEXT), 0);
  initStyles();
}

// ---- componentes --------------------------------------------------------------

bool ui_mem_ok(uint32_t bytes) {
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  bool ok = mon.free_biggest_size >= bytes;
  if (!ok) board_log("[ui] sem memoria no pool do LVGL: maior bloco %u B, preciso de %u B", (unsigned)mon.free_biggest_size, (unsigned)bytes);
  return ok;
}

lv_obj_t *ui_title(lv_obj_t *parent, const char *text) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_add_style(l, &sTitle20, 0);
  lv_obj_set_pos(l, 12, 8);
  return l;
}

lv_obj_t *ui_muted(lv_obj_t *parent, const char *text, const lv_font_t *font) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text);
  if (font == &lv_font_montserrat_12) {
    lv_obj_add_style(l, &sMuted12, 0);
  } else if (font == &lv_font_montserrat_14) {
    lv_obj_add_style(l, &sMuted14, 0);
  } else {
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, thc(th::MUTED), 0);
  }
  return l;
}

lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, w, h);
  lv_obj_set_pos(c, x, y);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_style(c, &sCard, 0);
  lv_obj_set_style_bg_color(c, thc(color), 0);
  return c;
}

lv_obj_t *ui_bar(lv_obj_t *parent, int w, int h, uint32_t color) {
  lv_obj_t *bar = lv_bar_create(parent);
  lv_obj_set_size(bar, w, h);
  lv_bar_set_value(bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, thc(th::TRACK), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bar, thc(color), LV_PART_INDICATOR);
  return bar;
}

static lv_obj_t *pillBtn(lv_obj_t *parent, int x, int y, int w, int h, lv_style_t *base, lv_style_t *pressed,
                         const char *text, lv_event_cb_t cb, void *user) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w > 0 ? w : LV_SIZE_CONTENT, h);
  lv_obj_add_style(b, base, 0);
  lv_obj_add_style(b, pressed, LV_STATE_PRESSED);
  lv_obj_set_ext_click_area(b, 4);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
  return l;
}

lv_obj_t *ui_tonal_btn(lv_obj_t *parent, int x, int y, int w, const char *text, lv_event_cb_t cb, void *user,
                       int h) {
  return pillBtn(parent, x, y, w, h, &sTonal, &sTonalPr, text, cb, user);
}

lv_obj_t *ui_mini_btn(lv_obj_t *parent, int x, int y, const char *text, lv_event_cb_t cb, void *user) {
  return pillBtn(parent, x, y, 0, 26, &sMini, &sMiniPr, text, cb, user);
}

lv_obj_t *ui_icon_btn(lv_obj_t *parent, int x, int y, const char *symbol, lv_event_cb_t cb, void *user) {
  lv_obj_t *l = pillBtn(parent, x, y, 32, 32, &sMini, &sMiniPr, symbol, cb, user);
  lv_obj_t *b = lv_obj_get_parent(l);
  lv_obj_set_style_pad_hor(b, 0, 0);
  lv_obj_set_style_text_font(b, &lv_font_montserrat_14, 0);
  lv_obj_set_ext_click_area(b, 8);
  return b;
}

lv_obj_t *ui_gear_btn(lv_obj_t *parent, lv_event_cb_t cb) {
  lv_obj_t *g = lv_label_create(parent);
  lv_label_set_text(g, LV_SYMBOL_SETTINGS);
  lv_obj_add_style(g, &sGear, 0);
  lv_obj_add_style(g, &sGearPr, LV_STATE_PRESSED);
  lv_obj_set_size(g, 28, 28);
  lv_obj_set_pos(g, 284, 208);
  lv_obj_add_flag(g, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(g, 6);
  lv_obj_add_event_cb(g, cb, LV_EVENT_CLICKED, nullptr);
  return g;
}

void ui_btn_danger(lv_obj_t *label) {
  lv_obj_t *b = lv_obj_get_parent(label);
  lv_obj_add_style(b, &sDanger, 0);
  lv_obj_add_style(b, &sDangerPr, LV_STATE_PRESSED);
}

lv_obj_t *ui_list(lv_obj_t *parent, int x, int y, int w) {
  lv_obj_t *c = ui_card(parent, x, y, w, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  return c;
}

UiRow ui_list_row(lv_obj_t *list, const char *icon, const char *title, const char *sub, lv_event_cb_t cb,
                  void *user, int h) {
  UiRow r;
  bool first = lv_obj_get_child_cnt(list) == 0;
  r.row = lv_obj_create(list);
  lv_obj_remove_style_all(r.row);
  lv_obj_set_size(r.row, LV_PCT(100), h);
  lv_obj_clear_flag(r.row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_style(r.row, &sRow, 0);
  lv_obj_add_style(r.row, &sRowPr, LV_STATE_PRESSED);
  if (!first) lv_obj_add_style(r.row, &sDivider, 0);
  if (cb) {
    lv_obj_add_flag(r.row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r.row, cb, LV_EVENT_CLICKED, user);
  }

  int x = 10;
  if (icon) {
    // Circulo e simbolo num objeto so: um rotulo de 24 px com fundo redondo.
    lv_obj_t *ic = lv_label_create(r.row);
    lv_label_set_text(ic, icon);
    lv_obj_add_style(ic, &sIconLbl, 0);
    lv_obj_add_style(ic, &sIconBadge, 0);
    lv_obj_set_size(ic, 24, 24);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 10, 0);
    x = 44;
  }

  r.title = lv_label_create(r.row);
  lv_label_set_text(r.title, title);
  lv_obj_add_style(r.title, &sTitle14, 0);
  r.sub = nullptr;
  if (sub) {
    lv_obj_align(r.title, LV_ALIGN_LEFT_MID, x, -8);
    r.sub = ui_muted(r.row, sub);
    lv_label_set_long_mode(r.sub, LV_LABEL_LONG_DOT);
    lv_obj_set_size(r.sub, 200, 15);
    lv_obj_align(r.sub, LV_ALIGN_LEFT_MID, x, 9);
  } else {
    lv_obj_align(r.title, LV_ALIGN_LEFT_MID, x, 0);
  }

  r.trail = ui_muted(r.row, cb ? LV_SYMBOL_RIGHT : "", &lv_font_montserrat_12);
  lv_obj_align(r.trail, LV_ALIGN_RIGHT_MID, -12, 0);
  return r;
}

lv_obj_t *ui_overlay() {
  lv_obj_t *o = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, 320, 240);
  lv_obj_set_pos(o, 0, 0);
  lv_obj_add_style(o, &sOverlay, 0);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);  // toque no fundo nao chega as paginas
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  return o;
}

lv_obj_t *ui_back_header(lv_obj_t *overlay, const char *title, lv_event_cb_t backCb) {
  lv_obj_t *b = lv_btn_create(overlay);
  lv_obj_remove_style_all(b);
  lv_obj_set_size(b, 38, 38);
  lv_obj_set_pos(b, 4, 2);
  lv_obj_add_style(b, &sBackBtn, 0);
  lv_obj_add_style(b, &sBackPr, LV_STATE_PRESSED);
  lv_obj_set_ext_click_area(b, 6);
  lv_obj_add_event_cb(b, backCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *arrow = lv_label_create(b);
  lv_label_set_text(arrow, LV_SYMBOL_LEFT);
  lv_obj_add_style(arrow, &sBackArrow, 0);
  lv_obj_center(arrow);

  lv_obj_t *t = lv_label_create(overlay);
  lv_label_set_text(t, title);
  lv_obj_add_style(t, &sTitle20, 0);
  lv_obj_set_pos(t, 44, 10);
  return t;
}

lv_obj_t *ui_dot(lv_obj_t *parent, int size, uint32_t color) {
  lv_obj_t *d = lv_obj_create(parent);
  lv_obj_remove_style_all(d);
  lv_obj_set_size(d, size, size);
  lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_style(d, &sDot, 0);
  lv_obj_set_style_bg_color(d, thc(color), 0);
  return d;
}
