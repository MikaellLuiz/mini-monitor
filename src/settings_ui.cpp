#include "settings_ui.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "clock_ui.h"
#include "diag_ui.h"
#include "pc_ui.h"
#include "pcagent.h"
#include "spotify.h"
#include "spotify_ui.h"
#include "theme.h"
#include "ui.h"
#include "usage.h"
#include "usage_ui.h"
#include "wifi_ui.h"

static constexpr int SCR_W = 320;
static constexpr int SCR_H = 240;

// ---- confirmacao -------------------------------------------------------------

static lv_obj_t *dlg;
static void (*dlgOk)();

static void dlgCloseCb(lv_event_t *) {
  lv_obj_del_async(dlg);  // async: o botao clicado e filho dele
  dlg = nullptr;
}

static void dlgOkCb(lv_event_t *e) {
  void (*fn)() = dlgOk;
  dlgCloseCb(e);
  if (fn) fn();
}

static void openDialog(const char *title, const char *text, const char *ok, void (*fn)()) {
  if (dlg) return;
  dlgOk = fn;
  dlg = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(dlg);
  lv_obj_set_size(dlg, SCR_W, SCR_H);
  lv_obj_set_style_bg_color(dlg, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(dlg, LV_OPA_60, 0);
  lv_obj_add_flag(dlg, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(dlg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

  lv_obj_t *card = ui_card(dlg, 24, 50, 272, 140, th::SHEET);
  lv_obj_set_style_radius(card, 16, 0);
  lv_obj_t *t = lv_label_create(card);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
  lv_obj_set_pos(t, 18, 14);
  lv_obj_t *m = ui_muted(card, text, &lv_font_montserrat_12);
  lv_obj_set_width(m, 236);
  lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(m, 18, 42);
  ui_tonal_btn(card, 18, 94, 100, "Cancelar", dlgCloseCb);
  lv_obj_t *okLbl = ui_tonal_btn(card, 128, 94, 126, ok, dlgOkCb);
  ui_btn_danger(okLbl);
}

// ---- paginas -----------------------------------------------------------------
// Escolhe quais paginas aparecem na linha horizontal. Salva na hora; o tileview so e
// montado no boot, entao vale ao reiniciar.

static lv_obj_t *pagesScreen, *pagesList;

static void styleSeg(lv_obj_t *label, bool on) {
  lv_obj_t *b = lv_obj_get_parent(label);
  lv_obj_set_style_bg_color(b, thc(on ? th::TONAL : th::MINI), 0);
  lv_obj_set_style_text_color(b, thc(on ? th::ON_TONAL : th::MUTED), 0);
}

static void pageSwitchCb(lv_event_t *e) {
  PageId id = (PageId)(intptr_t)lv_event_get_user_data(e);
  if (ui_ia_separate()) ui_set_ia_separate(false);
  ui_set_page_enabled(id, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void rebuildPagesList() {
  if (ui_ia_separate()) ui_set_ia_separate(false);
  lv_obj_clean(pagesList);
  for (int i = 0; i < PG_COUNT; i++) {
    PageId id = (PageId)i;
    if (!ui_page_applies(id)) continue;
    UiRow r = ui_list_row(pagesList, nullptr, ui_page_name(id), nullptr, nullptr, nullptr, 31);
    lv_obj_t *sw = lv_switch_create(r.row);
    lv_obj_set_size(sw, 40, 22);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(sw, thc(th::TRACK), 0);
    lv_obj_set_style_bg_color(sw, thc(th::FILL), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, thc(th::ON_TONAL), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_ext_click_area(sw, 8);
    if (ui_page_enabled(id)) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, pageSwitchCb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)id);
  }
}

static void restartNowCb(lv_event_t *) { board_restart(); }

static void pagesBackCb(lv_event_t *) {
  lv_obj_del_async(pagesScreen);  // async: o botao clicado e filho dela
  pagesScreen = nullptr;
  settings_ui_child_closed();
}

void pages_ui_open() {
  if (pagesScreen || !ui_mem_ok(11 * 1024)) return;
  pagesScreen = ui_overlay();
  ui_back_header(pagesScreen, "Paginas", pagesBackCb);

  lv_obj_t *scroll = lv_obj_create(pagesScreen);
  lv_obj_remove_style_all(scroll);
  lv_obj_set_size(scroll, 300, 158);
  lv_obj_set_pos(scroll, 10, 46);
  lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);
  pagesList = ui_list(scroll, 0, 0, 300);

  lv_obj_t *note = ui_muted(pagesScreen, "Vale depois de reiniciar.");
  lv_obj_set_pos(note, 12, 218);
  ui_tonal_btn(pagesScreen, 176, 210, 134, LV_SYMBOL_POWER " Reiniciar", restartNowCb, nullptr, 28);

  rebuildPagesList();
}

// ---- tema --------------------------------------------------------------------
// 4 paletas fixas (Grafite, Claude Code, Terminal, Noite) + modo escuro/claro.

static lv_obj_t *themeScreen;

static void themeModeCb(lv_event_t *e) {
  bool light = lv_event_get_user_data(e) != nullptr;
  if (light && !theme_palette_has_light(theme_palette())) return;
  if (light == theme_is_light()) return;
  theme_set_light(light);
  board_restart();
}

static void themePalCb(lv_event_t *e) {
  PaletteId p = (PaletteId)(intptr_t)lv_event_get_user_data(e);
  if (p == theme_palette()) return;
  theme_set_palette(p);
  board_restart();
}

static void themeBackCb(lv_event_t *) {
  lv_obj_del_async(themeScreen);
  themeScreen = nullptr;
  settings_ui_child_closed();
}

static void theme_ui_open() {
  if (themeScreen || !ui_mem_ok(9 * 1024)) return;
  themeScreen = ui_overlay();
  ui_back_header(themeScreen, "Tema", themeBackCb);

  bool hasLight = theme_palette_has_light(theme_palette());
  bool isLight = theme_is_light();

  lv_obj_t *btnDark = ui_tonal_btn(themeScreen, 186, 8, 60, "Escuro", themeModeCb, nullptr, 28);
  styleSeg(btnDark, !isLight);
  lv_obj_t *btnLight = ui_tonal_btn(themeScreen, 252, 8, 58, "Claro", themeModeCb, (void *)1, 28);
  styleSeg(btnLight, isLight && hasLight);
  if (!hasLight) {
    lv_obj_t *b = lv_obj_get_parent(btnLight);
    lv_obj_set_style_text_color(b, thc(th::OFF_TEXT), 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
  }

  static const char *const PAL_SUBS[PAL_COUNT] = {
      "Padrao - claro e escuro",
      "Terracota - claro e escuro",
      "Verde fosforo - so escuro",
      "Contraste OLED - so escuro",
  };
  lv_obj_t *list = ui_list(themeScreen, 10, 48, 300);
  for (int i = 0; i < PAL_COUNT; i++) {
    PaletteId p = (PaletteId)i;
    bool active = (p == theme_palette());
    UiRow r = ui_list_row(list, " ", theme_palette_name(p), PAL_SUBS[i], themePalCb, (void *)(intptr_t)p, 44);
    // Troca o icone por 3 circulos sobrepostos (bg, card, accent) da paleta.
    lv_obj_t *oldDot = lv_obj_get_child(r.row, 0);
    if (oldDot) lv_obj_add_flag(oldDot, LV_OBJ_FLAG_HIDDEN);
    ThemeSwatch swc = theme_swatch(p, isLight);
    uint32_t cols[3] = {swc.bg, swc.card, swc.accent};
    for (int k = 0; k < 3; k++) {
      lv_obj_t *sw = ui_dot(r.row, 13, cols[k]);
      lv_obj_set_style_border_width(sw, 1, 0);
      lv_obj_set_style_border_color(sw, thc(th::LINE), 0);
      lv_obj_align(sw, LV_ALIGN_LEFT_MID, 8 + k * 9, 0);
    }
    lv_label_set_text(r.trail, active ? LV_SYMBOL_OK : "");
    if (active) lv_obj_set_style_text_color(r.trail, thc(th::ACCENT), 0);
  }
}

// ---- fluidez e transicao -----------------------------------------------------
// Linhas do buffer de desenho (vale ao reiniciar) e animacao da troca de pagina (vale na hora).

static lv_obj_t *fluidScreen, *fluidWarn, *segBuf[3], *segTrans[3];
static const int BUF_OPTS[3] = {8, 12, 16};

static void refreshFluid() {
  int bl = ui_buf_lines(), tr = ui_transition();
  for (int i = 0; i < 3; i++) {
    styleSeg(segBuf[i], BUF_OPTS[i] == bl);
    styleSeg(segTrans[i], i == tr);
  }
  if (bl == 16) lv_obj_clear_flag(fluidWarn, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(fluidWarn, LV_OBJ_FLAG_HIDDEN);
}

static void bufCb(lv_event_t *e) {
  ui_set_buf_lines(BUF_OPTS[(intptr_t)lv_event_get_user_data(e)]);
  refreshFluid();
}

static void transCb(lv_event_t *e) {
  ui_set_transition((int)(intptr_t)lv_event_get_user_data(e));
  refreshFluid();
}

static void fluidBackCb(lv_event_t *) {
  lv_obj_del_async(fluidScreen);  // async: o botao clicado e filho dela
  fluidScreen = nullptr;
  settings_ui_child_closed();
}

static void fluid_ui_open() {
  if (fluidScreen || !ui_mem_ok(7 * 1024)) return;  // custo medido ~4,3 KB (x1,6)
  fluidScreen = ui_overlay();
  ui_back_header(fluidScreen, "Fluidez e transicao", fluidBackCb);

  lv_obj_t *l1 = ui_muted(fluidScreen, "Fluidez (buffer de desenho)");
  lv_obj_set_pos(l1, 12, 48);
  segBuf[0] = ui_tonal_btn(fluidScreen, 10, 66, 96, "Economia 8", bufCb, (void *)0, 30);
  segBuf[1] = ui_tonal_btn(fluidScreen, 112, 66, 96, "Normal 12", bufCb, (void *)1, 30);
  segBuf[2] = ui_tonal_btn(fluidScreen, 214, 66, 96, "Fluida 16", bufCb, (void *)2, 30);
  fluidWarn = ui_muted(fluidScreen, "Usa mais memoria: o Codex pode falhar.");
  lv_obj_set_style_text_color(fluidWarn, thc(th::AMBER), 0);
  lv_obj_set_pos(fluidWarn, 12, 100);

  lv_obj_t *l2 = ui_muted(fluidScreen, "Transicao entre paginas");
  lv_obj_set_pos(l2, 12, 130);
  segTrans[0] = ui_tonal_btn(fluidScreen, 10, 148, 84, "Deslizar", transCb, (void *)TRANS_SLIDE, 30);
  segTrans[1] = ui_tonal_btn(fluidScreen, 100, 148, 84, "Esmaecer", transCb, (void *)TRANS_FADE, 30);
  segTrans[2] = ui_tonal_btn(fluidScreen, 190, 148, 120, "Sem animacao", transCb, (void *)TRANS_NONE, 30);

  lv_obj_t *note = ui_muted(fluidScreen, "Fluidez vale depois de reiniciar.");
  lv_obj_set_pos(note, 12, 218);
  ui_tonal_btn(fluidScreen, 206, 210, 104, LV_SYMBOL_POWER " Reiniciar", restartNowCb, nullptr, 28);

  refreshFluid();
}

// ---- configuracoes -----------------------------------------------------------
// Tres categorias (Contas, Tela, Sistema), cada uma numa tela curta. Uma lista unica com as 12
// linhas custava ~15 KB do pool do LVGL de uma vez, e o pool e pequeno (ver lv_conf.h):
// com IA separada o painel chegava a reiniciar ao abrir as Configuracoes.

enum Row : int {
  R_CLAUDE, R_CODEX, R_AGY, R_SPOTIFY, R_PC,
  R_CLOCK, R_IA_ORDER, R_PAGES, R_THEME, R_FLUID,
  R_WIFI, R_DIAG, R_TOUCH, R_RESTART, R_RESET,
  R_CAT_ACCOUNTS, R_CAT_SCREEN, R_CAT_SYSTEM,
  R_COUNT
};
static constexpr int R_LEAF_COUNT = R_CAT_ACCOUNTS;  // linhas das telas de categoria

static lv_obj_t *screen, *catScreen, *subs[R_COUNT];
static lv_timer_t *timer;  // da tela montada no momento (lista principal ou categoria)
// Nada fica empilhado: abrir uma categoria desmonta a lista, abrir uma tela filha desmonta a categoria
// (o pool do LVGL e pequeno). curCat = categoria montada; retCat = para onde o voltar da filha leva.
static int curCat = -1, retCat = -1;
// true = categoria aberta de fora (engrenagem): o voltar dela fecha, sem montar a lista.
static bool catDirect = false;

// Categoria nao remontou (sem memoria): volta para a lista, ou so fecha se veio de fora.
static void fallbackToList() {
  curCat = -1;
  if (catDirect) catDirect = false;
  else settings_ui_open();
}

static void setSub(Row r, const char *text, uint32_t color) {
  if (!subs[r]) return;
  if (strcmp(lv_label_get_text(subs[r]), text)) lv_label_set_text(subs[r], text);
  lv_obj_set_style_text_color(subs[r], thc(color), 0);
}

static void refreshSubs(lv_timer_t *) {
  static const Provider PROV[3] = {PROV_CLAUDE, PROV_CODEX, PROV_ANTIGRAVITY};
  char buf[48];
  int connected = 0;
  for (int i = 0; i < 3; i++) {
    bool has = usage_has_account(PROV[i]);
    connected += has;
    setSub((Row)(R_CLAUDE + i), has ? "Conectado" : "Sem conta", has ? th::GREEN : th::ON_TONAL);
  }
  SpotifyState s = spotify_get();
  connected += s.linked;
  setSub(R_SPOTIFY, s.linked ? "Conectado" : "Sem conta", s.linked ? th::GREEN : th::ON_TONAL);
  PcMetrics m = pc_get();
  connected += m.paired;
  if (!m.paired) setSub(R_PC, "Sem agente", th::ON_TONAL);
  else setSub(R_PC, m.name, m.online ? th::GREEN : th::MUTED);

  snprintf(buf, sizeof(buf), "%d de 5 conectadas", connected);
  setSub(R_CAT_ACCOUNTS, buf, connected == 5 ? th::GREEN : th::MUTED);
  setSub(R_CAT_SCREEN, "Relogio, IAs, paginas e tema", th::MUTED);
  setSub(R_CAT_SYSTEM, "Wi-Fi, diagnostico, toque e reiniciar", th::MUTED);

  clock_ui_summary(buf, sizeof(buf));
  setSub(R_CLOCK, buf, th::MUTED);
  usage_ui_order_summary(buf, sizeof(buf));
  setSub(R_IA_ORDER, buf, th::MUTED);
  int n = 0;
  for (int i = 0; i < PG_COUNT; i++)
    if (ui_page_applies((PageId)i) && ui_page_enabled((PageId)i)) n++;
  snprintf(buf, sizeof(buf), "%d de 5 visiveis", n);
  setSub(R_PAGES, buf, th::MUTED);
  theme_summary(buf, sizeof(buf));
  setSub(R_THEME, buf, th::MUTED);
  static const char *const TRANS_NAMES[3] = {"Deslizar", "Esmaecer", "Sem animacao"};
  snprintf(buf, sizeof(buf), "%d linhas - %s", ui_buf_lines(), TRANS_NAMES[ui_transition()]);
  setSub(R_FLUID, buf, th::MUTED);

  wifi_ui_summary(buf, sizeof(buf));
  setSub(R_WIFI, buf, th::MUTED);
  board_heap_t h = board_heap();
  snprintf(buf, sizeof(buf), "Heap %u KB - Testar servicos", (unsigned)(h.free_bytes / 1024));
  setSub(R_DIAG, buf, th::MUTED);
  setSub(R_TOUCH, "Toque nos 4 cantos da tela", th::MUTED);
}

static void openCategory(Row cat);

static void unmountMain() {
  if (timer) lv_timer_del(timer);
  timer = nullptr;
  if (screen) lv_obj_del(screen);
  screen = nullptr;
  for (int i = R_CAT_ACCOUNTS; i < R_COUNT; i++) subs[i] = nullptr;
}

static void unmountCat() {
  if (timer) lv_timer_del(timer);
  timer = nullptr;
  if (catScreen) lv_obj_del(catScreen);
  catScreen = nullptr;
  for (int i = 0; i < R_LEAF_COUNT; i++) subs[i] = nullptr;
}

// As trocas rodam em lv_async_call (fora do evento do botao, que e filho da tela apagada), entao
// apagam direto e a memoria ja esta livre quando a proxima tela e montada.
static void toCategoryAsync(void *p) {
  unmountMain();
  curCat = (int)(intptr_t)p;
  openCategory((Row)curCat);
  if (!catScreen) fallbackToList();  // sem memoria: volta para a lista
}

static void openLeaf(Row r) {
  switch (r) {
    case R_CLAUDE: usage_ui_open_account(PROV_CLAUDE); break;
    case R_CODEX: usage_ui_open_account(PROV_CODEX); break;
    case R_AGY: usage_ui_open_account(PROV_ANTIGRAVITY); break;
    case R_SPOTIFY: spotify_ui_open_account(); break;
    case R_PC: pc_ui_open_agent(); break;
    case R_CLOCK: clock_ui_open_settings(); break;
    case R_IA_ORDER: usage_ui_open_order(); break;
    case R_PAGES: pages_ui_open(); break;
    case R_THEME: theme_ui_open(); break;
    case R_FLUID: fluid_ui_open(); break;
    case R_WIFI: wifi_ui_open(); break;
    case R_DIAG: diag_ui_open(); break;
    default: break;
  }
}

static void toLeafAsync(void *p) {
  int cat = curCat;
  unmountCat();
  uint32_t n = lv_obj_get_child_cnt(lv_layer_top());
  retCat = cat;
  openLeaf((Row)(intptr_t)p);
  if (lv_obj_get_child_cnt(lv_layer_top()) <= n) {  // a tela nao abriu (sem memoria): refaz a categoria
    retCat = -1;
    openCategory((Row)cat);
    if (!catScreen) fallbackToList();
  }
}

// O voltar das telas filhas chama settings_ui_child_closed() logo apos pedir a propria exclusao
// (lv_obj_del_async). O lv_async_call novo roda ANTES dessa exclusao no mesmo ciclo (timers novos
// entram na frente), por isso dois saltos: no segundo a filha ja sumiu.
static void backHop2(void *) {
  int cat = retCat;
  retCat = -1;
  if (cat < 0) return;
  curCat = cat;
  openCategory((Row)cat);
  if (!catScreen) fallbackToList();
}
static void backHop1(void *) { lv_async_call(backHop2, nullptr); }

void settings_ui_child_closed() {
  if (retCat >= 0) lv_async_call(backHop1, nullptr);
}

static void rowCb(lv_event_t *e) {
  switch ((Row)(intptr_t)lv_event_get_user_data(e)) {
    case R_CAT_ACCOUNTS:
    case R_CAT_SCREEN:
    case R_CAT_SYSTEM: lv_async_call(toCategoryAsync, lv_event_get_user_data(e)); break;
    case R_CLAUDE:
    case R_CODEX:
    case R_AGY:
    case R_SPOTIFY:
    case R_PC:
    case R_CLOCK:
    case R_IA_ORDER:
    case R_PAGES:
    case R_THEME:
    case R_FLUID:
    case R_WIFI:
    case R_DIAG: lv_async_call(toLeafAsync, lv_event_get_user_data(e)); break;
    case R_TOUCH: board_request_touch_recalibration(); break;
    case R_RESTART:
      openDialog("Reiniciar o painel?", "A tela apaga por alguns segundos. Contas e Wi-Fi continuam salvos.", "Reiniciar",
                 board_restart);
      break;
    case R_RESET:
      openDialog("Redefinir tudo?", "Apaga PIN, Wi-Fi, contas e PCs guardados. Nao tem volta.", "Apagar tudo",
                 board_factory_reset);
      break;
    default: break;
  }
}

static void closeCb(lv_event_t *) {
  lv_timer_del(timer);
  timer = nullptr;
  lv_obj_del_async(screen);  // async: o botao clicado e filho dela
  screen = nullptr;
  memset(subs, 0, sizeof(subs));
}

static void catBackAsync(void *) {
  unmountCat();
  curCat = -1;
  if (catDirect) catDirect = false;  // veio de fora: so fecha
  else settings_ui_open();           // remonta a lista
}
static void catBackCb(lv_event_t *) { lv_async_call(catBackAsync, nullptr); }

// Linha da lista; `plain` = sem subtitulo (acoes), com o titulo centralizado.
static UiRow addRow(lv_obj_t *list, Row id, const char *icon, const char *title, bool danger = false, bool plain = false,
                    int h = 0) {
  UiRow r = ui_list_row(list, icon, title, plain ? nullptr : "", rowCb, (void *)(intptr_t)id, h ? h : (plain ? 40 : 44));
  subs[id] = r.sub;
  if (danger) lv_obj_set_style_text_color(r.title, thc(th::ON_DANGER), 0);
  return r;
}

static void colorRowBadge(UiRow r, uint32_t bg, uint32_t fg) {
  lv_obj_t *dot = lv_obj_get_child(r.row, 0);
  if (!dot) return;
  lv_obj_set_style_bg_color(dot, thc(bg), 0);
  lv_obj_t *ic = lv_obj_get_child(dot, 0);
  if (ic) lv_obj_set_style_text_color(ic, thc(fg), 0);
}

static void openCategory(Row cat) {
  if (catScreen || !ui_mem_ok(9 * 1024)) return;  // Contas, a maior: ~6 KB medidos (x1,5)
  const char *name = cat == R_CAT_ACCOUNTS ? "Contas" : cat == R_CAT_SCREEN ? "Tela" : "Sistema";
  catScreen = ui_overlay();
  ui_back_header(catScreen, name, catBackCb);

  lv_obj_t *scroll = lv_obj_create(catScreen);
  lv_obj_remove_style_all(scroll);
  lv_obj_set_size(scroll, SCR_W, SCR_H - 44);
  lv_obj_set_pos(scroll, 0, 44);
  lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_hor(scroll, 10, 0);
  lv_obj_set_style_pad_bottom(scroll, 12, 0);
  lv_obj_t *list = ui_list(scroll, 0, 0, 300);

  if (cat == R_CAT_ACCOUNTS) {
    colorRowBadge(addRow(list, R_CLAUDE, "C", "Claude", false, false, 36), 0x3b241d, 0xd97757);
    colorRowBadge(addRow(list, R_CODEX, "X", "Codex", false, false, 36), 0x12312a, 0x10a37f);
    colorRowBadge(addRow(list, R_AGY, "A", "Antigravity", false, false, 36), 0x261f3d, 0x8b5cf6);
    colorRowBadge(addRow(list, R_SPOTIFY, LV_SYMBOL_AUDIO, "Spotify", false, false, 36), th::OK_BG, th::SP_GREEN);
    addRow(list, R_PC, LV_SYMBOL_USB, "Agente do PC", false, false, 36);
  } else if (cat == R_CAT_SCREEN) {
    addRow(list, R_CLOCK, LV_SYMBOL_BELL, "Relogio", false, false, 36);
    addRow(list, R_IA_ORDER, LV_SYMBOL_UP, "Ordem das IAs", false, false, 36);
    addRow(list, R_PAGES, LV_SYMBOL_LIST, "Paginas", false, false, 36);
    addRow(list, R_THEME, LV_SYMBOL_IMAGE, "Tema", false, false, 36);
    addRow(list, R_FLUID, LV_SYMBOL_REFRESH, "Fluidez e transicao", false, false, 36);
  } else {
    addRow(list, R_WIFI, LV_SYMBOL_WIFI, "Wi-Fi", false, false, 36);
    addRow(list, R_DIAG, LV_SYMBOL_LOOP, "Diagnostico de rede", false, false, 36);
    addRow(list, R_TOUCH, LV_SYMBOL_GPS, "Recalibrar toque", false, false, 36);
    addRow(list, R_RESTART, LV_SYMBOL_POWER, "Reiniciar o painel", true, true, 34);
    addRow(list, R_RESET, LV_SYMBOL_TRASH, "Redefinir tudo", true, true, 34);
  }
  timer = lv_timer_create(refreshSubs, 1000, nullptr);
  refreshSubs(nullptr);
}

void settings_ui_open_category(SettingsCat cat) {
  if (screen || catScreen || retCat >= 0) return;  // ja ha Configuracoes (ou filha dela) aberta
  catDirect = true;
  curCat = R_CAT_ACCOUNTS + (int)cat;
  openCategory((Row)curCat);
  if (!catScreen) {  // sem memoria: nao abre
    curCat = -1;
    catDirect = false;
  }
}

void settings_ui_open() {
  if (screen || !ui_mem_ok(6 * 1024)) return;  // lista: ~3,9 KB medidos (x1,5)
  screen = ui_overlay();
  ui_back_header(screen, "Configuracoes", closeCb);

  lv_obj_t *list = ui_list(screen, 10, 52, 300);
  addRow(list, R_CAT_ACCOUNTS, LV_SYMBOL_DIRECTORY, "Contas", false, false, 56);
  addRow(list, R_CAT_SCREEN, LV_SYMBOL_IMAGE, "Tela", false, false, 56);
  addRow(list, R_CAT_SYSTEM, LV_SYMBOL_SETTINGS, "Sistema", false, false, 56);

  timer = lv_timer_create(refreshSubs, 1000, nullptr);
  refreshSubs(nullptr);
}
