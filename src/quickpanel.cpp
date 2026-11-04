#include "quickpanel.h"

#include <lvgl.h>
#include <stdio.h>

#include "board.h"
#include "settings_ui.h"
#include "theme.h"
#include "ui.h"
#include "wifi_ui.h"

static constexpr int PANEL_W = 320;
static constexpr int PANEL_H = 240;
static constexpr uint8_t BRIGHT_MIN = 10;
static constexpr uint8_t BRIGHT_DEFAULT = 200;

static const char *NVS_BRIGHT = "bright";

static lv_obj_t *panel;
static lv_obj_t *lblClock, *lblUptime, *lblBright, *subWifi;

static bool isOpen() { return lv_obj_get_y(panel) > -PANEL_H; }

static void animTo(int32_t target) {
  lv_anim_del(panel, nullptr);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, panel);
  lv_anim_set_values(&a, lv_obj_get_y(panel), target);
  lv_anim_set_time(&a, 180);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, [](void *o, int32_t v) { lv_obj_set_y((lv_obj_t *)o, v); });
  lv_anim_start(&a);
}

static void refresh();

void quickpanel_open() {
  refresh();  // ja abre com os valores certos, sem esperar o proximo segundo
  animTo(0);
}
static void closePanel() { animTo(-PANEL_H); }

// Os gestos dentro do painel param nele (GESTURE_BUBBLE desligado), entao
// deslizar para o lado aqui nao troca a pagina de baixo.
static void panelGestureCb(lv_event_t *) {
  lv_indev_t *indev = lv_indev_get_act();
  if (lv_indev_get_gesture_dir(indev) != LV_DIR_TOP) return;
  closePanel();
  lv_indev_wait_release(indev);
}

static void showBright(uint8_t v) { lv_label_set_text_fmt(lblBright, "%d%%", (v * 100 + 127) / 255); }

static void brightnessCb(lv_event_t *e) {
  uint8_t v = lv_slider_get_value(lv_event_get_target(e));
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
    board_set_brightness(v);
    showBright(v);
  } else {  // soltou: salva uma vez so, para nao gastar a flash a cada passo
    settings_set_u8(NVS_BRIGHT, v);
  }
}

// Tema: as telas pegam as cores ao serem montadas, entao trocar reinicia o painel.
static void themeCb(lv_event_t *e) {
  bool light = lv_event_get_user_data(e) != nullptr;
  if (light && !theme_palette_has_light(theme_palette())) return;
  if (light == theme_is_light()) return;
  theme_set_light(light);
  board_restart();
}

static void wifiCb(lv_event_t *) { wifi_ui_open(); }
static void settingsCb(lv_event_t *) { settings_ui_open(); }
static void handleCb(lv_event_t *) { closePanel(); }

static void refresh() {
  uint32_t s = board_millis() / 1000;
  if (s < 3600) lv_label_set_text_fmt(lblUptime, "ligado ha %u min", (unsigned)(s / 60));
  else lv_label_set_text_fmt(lblUptime, "ligado ha %uh %02umin", (unsigned)(s / 3600), (unsigned)((s / 60) % 60));
  int hh, mm;
  if (board_local_time(&hh, &mm)) lv_label_set_text_fmt(lblClock, "%02d:%02d", hh, mm);
  else lv_label_set_text(lblClock, "--:--");

  char buf[64];
  wifi_ui_summary(buf, sizeof(buf));
  lv_label_set_text(subWifi, buf);
}

static void refreshCb(lv_timer_t *) {
  if (isOpen()) refresh();
}

void quickpanel_apply_saved_brightness() {
  uint8_t v = settings_get_u8(NVS_BRIGHT, BRIGHT_DEFAULT);
  board_set_brightness(v < BRIGHT_MIN ? BRIGHT_MIN : v);
}

// Botao do seletor Escuro/Claro: o do tema atual fica em destaque.
static void themeButton(lv_obj_t *parent, int x, int w, const char *text, bool light) {
  bool disabled = light && !theme_palette_has_light(theme_palette());
  bool on = !disabled && (theme_is_light() == light);
  lv_obj_t *l = ui_tonal_btn(parent, x, 0, w, text, themeCb, light ? (void *)1 : nullptr, 28);
  lv_obj_t *b = lv_obj_get_parent(l);
  lv_obj_set_style_bg_color(b, thc(on ? th::TONAL_PR : th::MINI), 0);
  lv_obj_set_style_text_color(b, thc(disabled ? th::OFF_TEXT : (on ? th::ON_TONAL : th::MUTED)), 0);
  if (disabled) lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
}

void quickpanel_init() {
  panel = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(panel);
  lv_obj_set_size(panel, PANEL_W, PANEL_H);
  lv_obj_set_pos(panel, 0, -PANEL_H);
  lv_obj_set_style_bg_color(panel, thc(th::BG), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(panel, thc(th::TEXT), 0);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(panel, panelGestureCb, LV_EVENT_GESTURE, nullptr);

  lblClock = lv_label_create(panel);
  lv_label_set_text(lblClock, "--:--");
  lv_obj_set_style_text_font(lblClock, &lv_font_montserrat_28, 0);
  lv_obj_set_pos(lblClock, 12, 2);

  lblUptime = ui_muted(panel, "");
  lv_obj_align(lblUptime, LV_ALIGN_TOP_RIGHT, -12, 14);

  // Brilho: barra larga em pilula, sem pegador (estilo Android).
  lv_obj_t *slider = lv_slider_create(panel);
  lv_obj_set_size(slider, 300, 28);
  lv_obj_set_pos(slider, 10, 38);
  lv_obj_remove_style(slider, nullptr, LV_PART_KNOB);
  lv_obj_set_style_radius(slider, 14, 0);
  lv_obj_set_style_bg_color(slider, thc(th::MINI), 0);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(slider, 14, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, thc(th::FILL), LV_PART_INDICATOR);
  lv_obj_set_ext_click_area(slider, 8);
  lv_slider_set_range(slider, BRIGHT_MIN, 255);
  lv_slider_set_value(slider, board_get_brightness(), LV_ANIM_OFF);
  lv_obj_add_event_cb(slider, brightnessCb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(slider, brightnessCb, LV_EVENT_RELEASED, nullptr);
  lv_obj_t *lblB = lv_label_create(slider);
  lv_label_set_text(lblB, LV_SYMBOL_EYE_OPEN "  Brilho");
  lv_obj_set_style_text_color(lblB, thc(th::ON_FILL), 0);
  lv_obj_align(lblB, LV_ALIGN_LEFT_MID, 14, 0);
  lblBright = lv_label_create(slider);
  lv_obj_set_style_text_color(lblBright, thc(th::ON_MINI), 0);
  lv_obj_align(lblBright, LV_ALIGN_RIGHT_MID, -14, 0);
  showBright(board_get_brightness());

  // Tema: dois botoes lado a lado.
  lv_obj_t *seg = lv_obj_create(panel);
  lv_obj_remove_style_all(seg);
  lv_obj_set_size(seg, 300, 28);
  lv_obj_set_pos(seg, 10, 72);
  themeButton(seg, 0, 146, LV_SYMBOL_EYE_CLOSE " Escuro", false);
  themeButton(seg, 154, 146, LV_SYMBOL_EYE_OPEN " Claro", true);

  lv_obj_t *list = ui_list(panel, 10, 108, 300);
  subWifi = ui_list_row(list, LV_SYMBOL_WIFI, "Wi-Fi", "", wifiCb, nullptr, 36).sub;
  ui_list_row(list, LV_SYMBOL_SETTINGS, "Configuracoes", "Contas, tela e sistema", settingsCb, nullptr, 36);

  lv_obj_t *handle = lv_obj_create(panel);
  lv_obj_remove_style_all(handle);
  lv_obj_set_size(handle, PANEL_W, 18);
  lv_obj_set_pos(handle, 0, PANEL_H - 18);
  lv_obj_add_flag(handle, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(handle, handleCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *pill = ui_dot(handle, 0, th::HANDLE);
  lv_obj_set_size(pill, 40, 4);
  lv_obj_center(pill);

  lv_timer_create(refreshCb, 1000, nullptr);
}
