#include "ui.h"

#include <lvgl.h>
#include <stdlib.h>

#include "board.h"
#include "clock_ui.h"
#include "pc_ui.h"
#include "quickpanel.h"
#include "spotify_ui.h"
#include "theme.h"
#include "usage_ui.h"
#include "wifi_ui.h"

static constexpr int SCR_W = 320;
// Deslizar foi escolhido na placa entre deslizar, esmaecer e troca seca: com a
// varredura a 111 Hz, o rasgo (sem pino TE) ficou o menos perceptivel.
static constexpr uint32_t SLIDE_MS = 220;

static constexpr uint32_t FADE_MS = 180;

static const char *NVS_HIDDEN = "pg_hide";  // bit por PageId: 1 = escondida
static const char *NVS_IA_SEP = "ia_sep";   // 1 = uma pagina por IA
static const char *NVS_BUF_LN = "buf_ln";   // linhas do buffer de desenho: 8, 12 ou 16
static const char *NVS_TRANS = "trans";     // transicao entre paginas (TRANS_*)

static const char *PAGE_NAMES[PG_COUNT] = {"Relogio", "Uso de IA", "Claude", "Codex", "Antigravity",
                                           "Spotify", "Atalhos",   "PC"};

static lv_obj_t *tileview;
static PageId pages[PG_COUNT];  // paginas montadas, na ordem
static int pageCount = 0;
static lv_obj_t *dots[PG_COUNT];
static lv_obj_t *tiles[PG_COUNT];
static bool mounted[PG_COUNT];  // conteudo da tile montado (so a atual e as vizinhas)
static int curPage = 0;
static bool animating = false;

// ---- configuracao ------------------------------------------------------------

const char *ui_page_name(PageId id) { return PAGE_NAMES[id]; }
bool ui_ia_separate() { return settings_get_u8(NVS_IA_SEP, 0) != 0; }
void ui_set_ia_separate(bool on) { settings_set_u8(NVS_IA_SEP, on ? 1 : 0); }

int ui_buf_lines() {
  int n = settings_get_u8(NVS_BUF_LN, 8);
  return (n == 12 || n == 16) ? n : 8;
}
void ui_set_buf_lines(int n) { settings_set_u8(NVS_BUF_LN, (n == 12 || n == 16) ? n : 8); }

int ui_transition() {
  int t = settings_get_u8(NVS_TRANS, TRANS_SLIDE);
  return t <= TRANS_NONE ? t : TRANS_SLIDE;
}
void ui_set_transition(int t) { settings_set_u8(NVS_TRANS, t); }

bool ui_page_applies(PageId id) {
  bool sep = ui_ia_separate();
  if (id == PG_USAGE) return !sep;
  if (id == PG_CLAUDE || id == PG_CODEX || id == PG_AGY) return sep;
  return true;
}

bool ui_page_enabled(PageId id) { return !(settings_get_u8(NVS_HIDDEN, 0) & (1u << id)); }

void ui_set_page_enabled(PageId id, bool on) {
  uint8_t hidden = settings_get_u8(NVS_HIDDEN, 0);
  hidden = on ? hidden & ~(1u << id) : hidden | (1u << id);
  settings_set_u8(NVS_HIDDEN, hidden);
}

// ---- navegacao ---------------------------------------------------------------

static void updateDots() {
  for (int i = 0; i < pageCount; i++) {
    bool on = i == curPage;
    lv_obj_set_size(dots[i], on ? 16 : 6, 6);
    lv_obj_set_style_bg_color(dots[i], thc(on ? th::DOT_ON : th::DOT), 0);
  }
}

static void buildPage(PageId id, lv_obj_t *tile);
static void unbuildPage(PageId id);

// Paginas sob demanda: o pool do LVGL e pequeno, entao so a atual e as vizinhas (para o deslize
// ter conteudo) ficam montadas. Desmonta primeiro, para o pico de memoria ser o menor possivel.
static void mountPage(int i) {
  if (mounted[i]) return;
  mounted[i] = true;
  buildPage(pages[i], tiles[i]);
}
static void unmountPage(int i) {
  if (!mounted[i]) return;
  mounted[i] = false;
  unbuildPage(pages[i]);  // timers e ponteiros do modulo
  lv_obj_clean(tiles[i]);
}
static void syncPages(void *) {
  if (animating) return;  // o fim da proxima animacao chama de novo
  for (int i = 0; i < pageCount; i++)
    if (abs(i - curPage) > 1) unmountPage(i);
  for (int i = 0; i < pageCount; i++)
    if (abs(i - curPage) <= 1) mountPage(i);
}

static void goToPage(int idx) {
  if (animating || idx < 0 || idx >= pageCount || idx == curPage) return;
  bool far = abs(idx - curPage) > 1;
  curPage = idx;
  updateDots();
  if (far) {  // salto longo (atalho do Relogio): sem deslizar por telas vazias
    mountPage(idx);
    lv_obj_scroll_to_x(tileview, idx * SCR_W, LV_ANIM_OFF);
    lv_async_call(syncPages, nullptr);
    return;
  }
  int trans = ui_transition();
  if (trans == TRANS_NONE) {  // troca imediata; o syncPages roda fora do evento do gesto
    lv_obj_scroll_to_x(tileview, idx * SCR_W, LV_ANIM_OFF);
    lv_async_call(syncPages, nullptr);
    return;
  }
  animating = true;
  lv_anim_t a;
  lv_anim_init(&a);
  if (trans == TRANS_FADE) {
    // Esmaecer: troca sem deslizar e a tile de destino sobe de transparente a opaca.
    lv_obj_t *t = tiles[idx];
    lv_obj_scroll_to_x(tileview, idx * SCR_W, LV_ANIM_OFF);
    lv_obj_set_style_opa(t, LV_OPA_TRANSP, 0);
    lv_anim_set_var(&a, t);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a, [](void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); });
  } else {
    lv_anim_set_var(&a, tileview);
    lv_anim_set_values(&a, lv_obj_get_scroll_x(tileview), idx * SCR_W);
    lv_anim_set_time(&a, SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, [](void *o, int32_t v) { lv_obj_scroll_to_x((lv_obj_t *)o, v, LV_ANIM_OFF); });
  }
  lv_anim_set_ready_cb(&a, [](lv_anim_t *) {
    animating = false;
    lv_async_call(syncPages, nullptr);  // fora da animacao: monta as vizinhas novas e solta as distantes
  });
  lv_anim_start(&a);
}

// A pagina so troca por gesto: basta um arrasto curto (indev gesture_limit) e o
// resto do movimento e animado, entao o dedo aliviar no fim nao atrapalha.
// Gesto para baixo em qualquer ponto abre o painel de ajustes.
static void gestureCb(lv_event_t *) {
  lv_indev_t *indev = lv_indev_get_act();
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir == LV_DIR_LEFT) goToPage(curPage + 1);
  else if (dir == LV_DIR_RIGHT) goToPage(curPage - 1);
  else if (dir == LV_DIR_BOTTOM) quickpanel_open();
  else return;
  lv_indev_wait_release(indev);  // o gesto nao vira clique no botao em que comecou
}

bool ui_is_animating() { return animating; }

bool ui_goto(PageId id) {
  if (id == PG_USAGE && ui_ia_separate()) id = PG_CLAUDE;
  for (int i = 0; i < pageCount; i++) {
    if (pages[i] != id) continue;
    goToPage(i);
    return true;
  }
  return false;
}
bool ui_page_shown(PageId id) { return pageCount > 0 && pages[curPage] == id; }

static void unbuildPage(PageId id) {
  switch (id) {
    case PG_CLOCK: clock_ui_unbuild_page(); break;
    case PG_USAGE: usage_ui_unbuild_page(); break;
    case PG_CLAUDE: usage_ui_unbuild_provider_page(PROV_CLAUDE); break;
    case PG_CODEX: usage_ui_unbuild_provider_page(PROV_CODEX); break;
    case PG_AGY: usage_ui_unbuild_provider_page(PROV_ANTIGRAVITY); break;
    case PG_SPOTIFY: spotify_ui_unbuild_page(); break;
    case PG_SHORTCUTS: shortcuts_ui_unbuild_page(); break;
    case PG_PC: pc_ui_unbuild_page(); break;
    default: break;
  }
}

static void buildPage(PageId id, lv_obj_t *tile) {
  switch (id) {
    case PG_CLOCK: clock_ui_build_page(tile); break;
    case PG_USAGE: usage_ui_build_page(tile); break;
    case PG_CLAUDE: usage_ui_build_provider_page(tile, PROV_CLAUDE); break;
    case PG_CODEX: usage_ui_build_provider_page(tile, PROV_CODEX); break;
    case PG_AGY: usage_ui_build_provider_page(tile, PROV_ANTIGRAVITY); break;
    case PG_SPOTIFY: spotify_ui_build_page(tile); break;
    case PG_SHORTCUTS: shortcuts_ui_build_page(tile); break;
    case PG_PC: pc_ui_build_page(tile); break;
    default: break;
  }
}

void ui_init() {
  theme_init();

  // So monta as paginas ligadas: cada uma escondida e memoria do LVGL a menos.
  for (int i = 0; i < PG_COUNT; i++) {
    PageId id = (PageId)i;
    if (ui_page_applies(id) && ui_page_enabled(id)) pages[pageCount++] = id;
  }
  if (pageCount == 0) pages[pageCount++] = PG_CLOCK;  // nunca fica sem nada na tela

  tileview = lv_tileview_create(lv_scr_act());
  lv_obj_set_style_bg_color(tileview, thc(th::BG), 0);
  lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);
  // Sem rolagem seguindo o dedo: no toque resistivo o arrasto longo falha, e cada
  // pixel arrastado custava um redesenho da tela inteira.
  lv_obj_clear_flag(tileview, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(lv_scr_act(), gestureCb, LV_EVENT_GESTURE, nullptr);

  for (int i = 0; i < pageCount; i++) {
    lv_dir_t dir = (i > 0 ? LV_DIR_LEFT : 0) | (i < pageCount - 1 ? LV_DIR_RIGHT : 0);
    tiles[i] = lv_tileview_add_tile(tileview, i, 0, dir);
    lv_obj_set_style_text_color(tiles[i], thc(th::TEXT), 0);
  }
  for (int i = 0; i < pageCount && i <= 1; i++) mountPage(i);  // a atual (0) e a vizinha
  lv_obj_set_tile_id(tileview, 0, 0, LV_ANIM_OFF);
  board_log("[ui] %d paginas, IA %s", pageCount, ui_ia_separate() ? "separada" : "junta");

  // Indicador de pagina na camada de cima, sem capturar toque.
  lv_obj_t *row = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(row);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(row, 200, 18);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 5, 0);
  for (int i = 0; i < pageCount; i++) {
    dots[i] = lv_obj_create(row);
    lv_obj_remove_style_all(dots[i]);
    lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
  }
  if (pageCount == 1) lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
  updateDots();

  wifi_ui_init_overlays();
  quickpanel_init();  // por ultimo: fica acima das paginas, do indicador e da barra
}
