#include "pc_ui.h"

#include <stdio.h>
#include <string.h>

#include "account_portal.h"
#include "board.h"
#include "pcagent.h"
#include "theme.h"
#include "settings_ui.h"
#include "ui.h"
#include "wifi_mgr.h"

static constexpr int SCR_W = 320;
static constexpr int SCR_H = 240;

static void pageVisibilityCb(lv_timer_t *) { pc_set_visible(ui_page_shown(PG_PC), ui_page_shown(PG_SHORTCUTS)); }

// Um timer so, criado pela primeira das duas paginas que for montada.
static void ensureVisibilityTimer() {
  static bool created = false;
  if (created) return;
  created = true;
  lv_timer_create(pageVisibilityCb, 500, nullptr);
}

static void setIfChanged(lv_obj_t *l, const char *text) {
  if (strcmp(lv_label_get_text(l), text)) lv_label_set_text(l, text);
}

// ---- pagina PC ---------------------------------------------------------------

struct Metric {
  lv_obj_t *value, *sub, *arc;
  uint32_t color;
};
static Metric metrics[4];  // CPU, RAM, GPU, Temp.
static lv_obj_t *lblBadge, *badgeDot;
static lv_timer_t *pcTimer, *scTimer;
static uint32_t refreshingUntil;  // "atualizando" apos um toque
static void openPair(lv_event_t *);

// Toque num cartao: com agente, consulta agora; sem agente, abre o pareamento.
static void metricTapCb(lv_event_t *) {
  if (!pc_get().paired) {
    openPair(nullptr);
    return;
  }
  pc_refresh_now();
  refreshingUntil = board_millis() + 1500;
}

static void makeMetric(lv_obj_t *parent, int idx, int x, int y, const char *name, uint32_t color) {
  Metric &m = metrics[idx];
  m.color = color;
  lv_obj_t *box = ui_card(parent, x, y, 148, 82);
  lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(box, thc(th::ROW_PR), LV_STATE_PRESSED);
  lv_obj_add_event_cb(box, metricTapCb, LV_EVENT_CLICKED, nullptr);

  m.arc = lv_arc_create(box);
  lv_obj_set_size(m.arc, 52, 52);
  lv_obj_set_pos(m.arc, 10, 15);
  lv_arc_set_rotation(m.arc, 270);
  lv_arc_set_bg_angles(m.arc, 0, 360);
  lv_arc_set_range(m.arc, 0, 100);
  lv_arc_set_value(m.arc, 0);
  lv_obj_remove_style(m.arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(m.arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(m.arc, 6, 0);
  lv_obj_set_style_arc_color(m.arc, thc(th::TRACK), 0);
  lv_obj_set_style_arc_width(m.arc, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(m.arc, thc(color), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(m.arc, true, LV_PART_INDICATOR);

  m.value = lv_label_create(m.arc);
  lv_label_set_text(m.value, "--");
  lv_obj_set_style_text_font(m.value, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(m.value, thc(th::TEXT), 0);
  lv_obj_center(m.value);

  lv_obj_t *n = lv_label_create(box);
  lv_label_set_text(n, name);
  lv_obj_set_style_text_font(n, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(n, thc(th::TEXT), 0);
  lv_obj_set_pos(n, 72, 22);

  m.sub = ui_muted(box, "");
  lv_label_set_long_mode(m.sub, LV_LABEL_LONG_DOT);
  lv_obj_set_width(m.sub, 68);
  lv_obj_set_pos(m.sub, 72, 42);
}

static void setMetric(Metric &m, float pct, const char *unit, int shownValue, const char *sub) {
  setIfChanged(m.sub, sub);
  if (pct < 0) {
    setIfChanged(m.value, "--");
    lv_arc_set_value(m.arc, 0);
    return;
  }
  char buf[12];
  snprintf(buf, sizeof(buf), "%d%s", shownValue, unit);
  setIfChanged(m.value, buf);
  int v = (int)(pct + 0.5f);
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  lv_arc_set_value(m.arc, v);
  uint32_t col = pct >= 90 ? th::RED : pct >= 75 ? th::AMBER : m.color;
  lv_obj_set_style_arc_color(m.arc, thc(col), LV_PART_INDICATOR);
}

static void paintPc() {
  PcMetrics m = pc_get();
  char badge[32];
  uint32_t color = th::MUTED;
  if (!m.paired) snprintf(badge, sizeof(badge), "Sem agente - toque");
  else if ((int32_t)(board_millis() - refreshingUntil) < 0) {
    snprintf(badge, sizeof(badge), "atualizando...");
    color = th::MUTED;
  }
  else if (!m.online) {
    snprintf(badge, sizeof(badge), "%.14s: %.14s", m.name, m.error[0] ? m.error : "conectando");
    color = m.error[0] ? th::RED : th::MUTED;
  } else {
    snprintf(badge, sizeof(badge), "%.18s - %s", m.name, m.source == PC_SRC_USB ? "USB" : "Wi-Fi");
    color = th::GREEN;
  }
  setIfChanged(lblBadge, badge);
  lv_obj_align(lblBadge, LV_ALIGN_TOP_RIGHT, -12, 14);
  lv_obj_set_style_bg_color(badgeDot, thc(color), 0);
  lv_obj_align_to(badgeDot, lblBadge, LV_ALIGN_OUT_LEFT_MID, -6, 0);

  bool ok = m.paired && m.online;
  char vram[16] = "";
  if (ok && m.vram >= 0) snprintf(vram, sizeof(vram), "VRAM %d%%", (int)(m.vram + 0.5f));
  else if (ok) snprintf(vram, sizeof(vram), "em uso");
  const char *cpuSub = !ok ? "--" : (m.cpu >= 80 ? "alta" : "normal");
  const char *ramSub = !ok ? "--" : "em uso";
  const char *tempSub = (ok && m.temp >= 0) ? "GPU" : "--";
  setMetric(metrics[0], ok ? m.cpu : -1, "%", (int)(m.cpu + 0.5f), cpuSub);
  setMetric(metrics[1], ok ? m.ram : -1, "%", (int)(m.ram + 0.5f), ramSub);
  setMetric(metrics[2], ok ? m.gpu : -1, "%", (int)(m.gpu + 0.5f), ok ? vram : "--");
  // Temp.: anel em escala de 0 a 100 C; rotulo curto "Temp." para nunca extrapolar os 68px.
  setMetric(metrics[3], ok && m.temp >= 0 ? (float)m.temp : -1, "C", m.temp, tempSub);
}

static void pcRefreshCb(lv_timer_t *) {
  if (!ui_page_shown(PG_PC) || ui_is_animating()) return;
  paintPc();
}

// ---- pareamento --------------------------------------------------------------

static lv_obj_t *pair, *pairCard, *pairStatus, *pairBtns, *pairSub, *forgetLbl, *forgetBtn, *pairNewBtn, *chooseBtn;
static lv_timer_t *pairTimer;
static uint32_t forgetArmedAt;

static void closePair() {
  account_portal_stop();
  lv_timer_del(pairTimer);
  pairTimer = nullptr;
  lv_obj_del_async(pair);  // async: chamado de um botao filho dele
  pair = pairCard = pairStatus = pairBtns = pairSub = forgetBtn = pairNewBtn = chooseBtn = nullptr;
  settings_ui_child_closed();
}

static void updatePair(lv_timer_t *) {
  char buf[200], line1[80];
  int n = pc_paired_count();
  PcMetrics m = pc_get();
  if (!m.paired) snprintf(line1, sizeof(line1), "Nenhum PC no momento.");
  else if (m.online) snprintf(line1, sizeof(line1), "%s - %s", m.name, m.source == PC_SRC_USB ? "USB" : "Wi-Fi");
  else snprintf(line1, sizeof(line1), "%s - %s", m.name, m.error[0] ? m.error : "aguardando resposta...");
  snprintf(buf, sizeof(buf), "%s\n%s - %d pareado(s) por Wi-Fi", line1, m.pinned ? "Fixado" : "Automatico", n);
  lv_label_set_text(pairStatus, buf);
  // Sem PC: so parear. Com PC: parear outro, escolher qual usar e remover.
  bool has = m.paired || n > 0;
  if (forgetBtn) {
    if (has) {
      lv_obj_clear_flag(forgetBtn, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(chooseBtn, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(forgetBtn, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(chooseBtn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_width(pairNewBtn, has ? 170 : 300);
  }
  if (forgetArmedAt && board_millis() - forgetArmedAt > 3000) {
    forgetArmedAt = 0;
    lv_label_set_text(forgetLbl, LV_SYMBOL_TRASH " Remover");
  }
}

// ---- escolher PC -------------------------------------------------------------

static lv_obj_t *choose, *chooseList;

static void chooseCb(lv_event_t *e);

static void rebuildChoose() {
  lv_obj_clean(chooseList);
  char pin[PC_NAME_MAX];
  bool hasPin = pc_pinned_name(pin, sizeof(pin));
  UiRow a = ui_list_row(chooseList, LV_SYMBOL_SHUFFLE, "Automatico", "USB e depois o primeiro do Wi-Fi", chooseCb,
                        (void *)0, 40);
  if (!hasPin) lv_label_set_text(a.trail, LV_SYMBOL_OK);
  for (int i = 0; i < pc_choice_count(); i++) {
    char name[PC_NAME_MAX];
    uint8_t mask = 0;
    if (!pc_choice_name(i, name, sizeof(name), &mask)) break;
    const char *sub = (mask & 3) == 3 ? "USB e Wi-Fi" : (mask & 1) ? "USB" : (mask & 2) ? "Wi-Fi" : "fora de alcance";
    UiRow r = ui_list_row(chooseList, (mask & 1) ? LV_SYMBOL_USB : LV_SYMBOL_WIFI, name, sub, chooseCb,
                          (void *)(intptr_t)(i + 1), 40);
    if (hasPin && !strcmp(name, pin)) lv_label_set_text(r.trail, LV_SYMBOL_OK);
  }
}

static void chooseCb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);  // 0 = automatico, n = n-esimo PC da lista
  char name[PC_NAME_MAX];
  if (idx == 0) pc_select("");
  else if (pc_choice_name(idx - 1, name, sizeof(name), nullptr)) pc_select(name);
  rebuildChoose();
}

static void chooseBackCb(lv_event_t *) {
  lv_obj_del_async(choose);  // async: o botao clicado e filho dele
  choose = chooseList = nullptr;
}

static void openChoose(lv_event_t *) {
  if (choose) return;
  choose = ui_overlay();
  ui_back_header(choose, "Escolher PC", chooseBackCb);
  lv_obj_t *scroll = lv_obj_create(choose);
  lv_obj_remove_style_all(scroll);
  lv_obj_set_size(scroll, 300, 150);
  lv_obj_set_pos(scroll, 10, 48);
  lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);
  chooseList = ui_list(scroll, 0, 0, 300);
  lv_obj_t *note = ui_muted(choose, "O PC fixado vale mesmo desligado.\nAutomatico: USB, depois Wi-Fi.");
  lv_obj_set_pos(note, 12, 204);
  rebuildChoose();
}

static void forgetCb(lv_event_t *) {
  if (!forgetArmedAt) {  // dois toques em 3 s, para nao apagar sem querer
    forgetArmedAt = board_millis();
    lv_label_set_text(forgetLbl, "Toque de novo");
    return;
  }
  forgetArmedAt = 0;
  pc_forget_all();
  lv_label_set_text(forgetLbl, LV_SYMBOL_TRASH " Remover");
}

static void pairNewCb(lv_event_t *) {
  if (!account_portal_start()) {
    lv_label_set_text(pairStatus, "Conecte o Wi-Fi primeiro\n(Ajustes > Wi-Fi).");
    return;
  }
  AccountPortalInfo info = account_portal_info();
  lv_obj_add_flag(pairCard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(pairBtns, LV_OBJ_FLAG_HIDDEN);
  pairSub = lv_obj_create(pair);
  lv_obj_remove_style_all(pairSub);
  lv_obj_set_size(pairSub, SCR_W, SCR_H - 44);
  lv_obj_set_pos(pairSub, 0, 44);

  lv_obj_t *l = lv_label_create(pairSub);
  lv_obj_set_width(l, 300);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(l, 12, 6);
  lv_label_set_text(l, "No PC, com o agente instalado, rode:");
  lv_obj_t *cmd = lv_label_create(pairSub);
  lv_obj_set_style_text_font(cmd, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(cmd, thc(th::ON_TONAL), 0);
  lv_obj_set_pos(cmd, 12, 32);
  lv_label_set_text(cmd, "python agent.py pair <codigo>");
  lv_obj_t *hint = ui_muted(pairSub, "Codigo (vale enquanto esta tela estiver aberta):");
  lv_obj_set_pos(hint, 12, 56);
  lv_obj_t *code = lv_label_create(pairSub);
  lv_obj_set_style_text_font(code, &lv_font_montserrat_28, 0);
  lv_obj_set_pos(code, 12, 78);
  lv_label_set_text(code, info.code);
}

static void backCb(lv_event_t *) { closePair(); }

void pc_ui_open_agent() { openPair(nullptr); }

static void openPair(lv_event_t *) {
  if (pair || !ui_mem_ok(6 * 1024)) return;
  forgetArmedAt = 0;
  pair = ui_overlay();
  ui_back_header(pair, "Agente do PC", backCb);

  pairCard = ui_card(pair, 10, 48, 300, 56);
  pairStatus = lv_label_create(pairCard);
  lv_obj_set_width(pairStatus, 280);
  lv_label_set_long_mode(pairStatus, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(pairStatus, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_line_space(pairStatus, 3, 0);
  lv_obj_set_pos(pairStatus, 10, 9);

  pairBtns = lv_obj_create(pair);
  lv_obj_remove_style_all(pairBtns);
  lv_obj_set_size(pairBtns, SCR_W, 120);
  lv_obj_set_pos(pairBtns, 0, 112);
  pairNewBtn = lv_obj_get_parent(ui_tonal_btn(pairBtns, 10, 0, 170, LV_SYMBOL_PLUS " Parear um PC", pairNewCb));
  forgetLbl = ui_tonal_btn(pairBtns, 190, 0, 120, LV_SYMBOL_TRASH " Remover", forgetCb);
  ui_btn_danger(forgetLbl);
  forgetBtn = lv_obj_get_parent(forgetLbl);
  chooseBtn = lv_obj_get_parent(ui_tonal_btn(pairBtns, 10, 42, 300, LV_SYMBOL_USB " Escolher PC", openChoose));

  pairTimer = lv_timer_create(updatePair, 1000, nullptr);
  updatePair(nullptr);
}

void pc_ui_build_page(lv_obj_t *tile) {
  ui_title(tile, "PC");
  lblBadge = ui_muted(tile, "");
  lv_obj_set_style_text_color(lblBadge, thc(th::ON_MINI), 0);
  lv_obj_align(lblBadge, LV_ALIGN_TOP_RIGHT, -12, 14);
  badgeDot = ui_dot(tile, 7, th::MUTED);
  lv_obj_align_to(badgeDot, lblBadge, LV_ALIGN_OUT_LEFT_MID, -6, 0);

  makeMetric(tile, 0, 10, 40, "CPU", th::ACCENT);
  makeMetric(tile, 1, 162, 40, "RAM", th::ACCENT);
  makeMetric(tile, 2, 10, 130, "GPU", th::ACCENT);
  makeMetric(tile, 3, 162, 130, "Temp.", th::AMBER);

  pcTimer = lv_timer_create(pcRefreshCb, 1000, nullptr);
  ensureVisibilityTimer();
  paintPc();  // a pagina pode ser montada antes de aparecer: ja nasce com os valores
}

// Paginas sob demanda: o ui.cpp apaga os filhos da tile (lv_obj_clean) depois disto.
// O timer de visibilidade e compartilhado com os Atalhos e nao guarda objeto nenhum: fica.
void pc_ui_unbuild_page() {
  if (pcTimer) lv_timer_del(pcTimer);
  pcTimer = nullptr;
  memset(metrics, 0, sizeof(metrics));
  lblBadge = badgeDot = nullptr;
  refreshingUntil = 0;
}

// ---- pagina Atalhos ----------------------------------------------------------

static lv_obj_t *scBtn[PC_MAX_SHORTCUTS], *scLbl[PC_MAX_SHORTCUTS], *scIcon[PC_MAX_SHORTCUTS];
static lv_obj_t *scSub, *scSent, *scSentLbl, *scEmpty, *scEmptyTitle, *scEmptySub, *scSetup;
static int shownCount = -1;
static char shownLabels[PC_MAX_SHORTCUTS][13];
static uint32_t sentAt;

static const char *iconForShortcut(const char *label) {
  if (!label || !label[0]) return LV_SYMBOL_CHARGE;
  char low[16];
  size_t i = 0;
  for (; label[i] && i < sizeof(low) - 1; i++) {
    char c = label[i];
    low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  low[i] = 0;
  if (strstr(low, "mud") || strstr(low, "mut") || strstr(low, "mic")) return LV_SYMBOL_MUTE;
  if (strstr(low, "som") || strstr(low, "vol") || strstr(low, "aud") || strstr(low, "fon")) return LV_SYMBOL_VOLUME_MAX;
  if (strstr(low, "tel") || strstr(low, "prin") || strstr(low, "cap") || strstr(low, "fot")) return LV_SYMBOL_IMAGE;
  if (strstr(low, "bloq") || strstr(low, "loc") || strstr(low, "sair")) return LV_SYMBOL_EYE_CLOSE;
  if (strstr(low, "pla") || strstr(low, "pau") || strstr(low, "mus")) return LV_SYMBOL_PLAY;
  if (strstr(low, "ter") || strstr(low, "cmd") || strstr(low, "cod")) return LV_SYMBOL_KEYBOARD;
  return LV_SYMBOL_CHARGE;
}

static void shortcutCb(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  pc_shortcut_press(i);
  lv_label_set_text_fmt(scSentLbl, LV_SYMBOL_OK " %s enviado", shownLabels[i]);
  lv_obj_clear_flag(scSent, LV_OBJ_FLAG_HIDDEN);
  if (scSub) lv_obj_add_flag(scSub, LV_OBJ_FLAG_HIDDEN);
  sentAt = board_millis();
}

static void paintShortcuts() {
  int n = pc_shortcut_count();
  PcMetrics m = pc_get();
  for (int i = 0; i < PC_MAX_SHORTCUTS; i++) {
    char label[13] = "";
    bool has = i < n && pc_shortcut_label(i, label, sizeof(label));
    if (has) {
      lv_obj_clear_flag(scBtn[i], LV_OBJ_FLAG_HIDDEN);
      if (strcmp(shownLabels[i], label)) {
        snprintf(shownLabels[i], sizeof(shownLabels[i]), "%s", label);
        lv_label_set_text(scLbl[i], label);
        lv_label_set_text(scIcon[i], iconForShortcut(label));
      }
    } else {
      lv_obj_add_flag(scBtn[i], LV_OBJ_FLAG_HIDDEN);
      shownLabels[i][0] = 0;
    }
  }
  if (scSub) {
    if (n > 0 && !sentAt) {
      lv_label_set_text_fmt(scSub, "%d do PC", n);
      lv_obj_align(scSub, LV_ALIGN_TOP_RIGHT, -12, 14);
      lv_obj_clear_flag(scSub, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(scSub, LV_OBJ_FLAG_HIDDEN);
    }
  }
  int state = n > 0 ? 1 : (m.paired ? 2 : 3);
  if (state != shownCount) {
    shownCount = state;
    if (state == 1) {
      lv_obj_add_flag(scEmpty, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(scEmpty, LV_OBJ_FLAG_HIDDEN);
      if (state == 2) {
        lv_label_set_text(scEmptyTitle, m.error[0] ? "PC sem resposta" : "Conectando ao PC...");
        lv_label_set_text(scEmptySub,
                          m.error[0] ? "Confira se o agente esta rodando no PC." : "Carregando os atalhos configurados.");
        lv_obj_add_flag(scSetup, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_label_set_text(scEmptyTitle, "Nenhum PC ligado");
        lv_label_set_text(scEmptySub, "Pareie o agente no PC para usar atalhos.");
        lv_obj_clear_flag(scSetup, LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
}

static void scRefreshCb(lv_timer_t *) {
  if (sentAt && board_millis() - sentAt > 2500) {
    sentAt = 0;
    lv_obj_add_flag(scSent, LV_OBJ_FLAG_HIDDEN);
  }
  if (!ui_page_shown(PG_SHORTCUTS) || ui_is_animating()) return;
  paintShortcuts();
}

void shortcuts_ui_build_page(lv_obj_t *tile) {
  ui_title(tile, "Atalhos");

  scSub = ui_muted(tile, "");
  lv_obj_align(scSub, LV_ALIGN_TOP_RIGHT, -12, 14);
  lv_obj_add_flag(scSub, LV_OBJ_FLAG_HIDDEN);

  // Confirmacao do ultimo atalho, some sozinha.
  scSent = ui_card(tile, 150, 8, 160, 24, th::OK_BG);
  lv_obj_set_style_radius(scSent, LV_RADIUS_CIRCLE, 0);
  scSentLbl = lv_label_create(scSent);
  lv_label_set_long_mode(scSentLbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(scSentLbl, 144);
  lv_obj_set_style_text_align(scSentLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(scSentLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(scSentLbl, thc(th::GREEN), 0);
  lv_obj_center(scSentLbl);
  lv_obj_add_flag(scSent, LV_OBJ_FLAG_HIDDEN);

  for (int i = 0; i < PC_MAX_SHORTCUTS; i++) {
    int col = i % 3, row = i / 3;
    scBtn[i] = ui_card(tile, 10 + col * 102, 42 + row * 88, 96, 80);
    lv_obj_add_flag(scBtn[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(scBtn[i], thc(th::CARD_HI), LV_STATE_PRESSED);
    lv_obj_add_flag(scBtn[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *ic = ui_dot(scBtn[i], 36, th::TONAL);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 10);
    scIcon[i] = lv_label_create(ic);
    lv_label_set_text(scIcon[i], LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(scIcon[i], thc(th::ON_TONAL), 0);
    lv_obj_center(scIcon[i]);
    scLbl[i] = lv_label_create(scBtn[i]);
    lv_label_set_long_mode(scLbl[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(scLbl[i], 88);
    lv_obj_set_style_text_align(scLbl[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(scLbl[i], &lv_font_montserrat_14, 0);
    lv_label_set_text(scLbl[i], "");
    lv_obj_align(scLbl[i], LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(scBtn[i], shortcutCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }

  scEmpty = lv_obj_create(tile);
  lv_obj_remove_style_all(scEmpty);
  lv_obj_set_size(scEmpty, 300, 170);
  lv_obj_set_pos(scEmpty, 10, 44);
  lv_obj_clear_flag(scEmpty, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *usbBadge = ui_dot(scEmpty, 44, th::TONAL);
  lv_obj_align(usbBadge, LV_ALIGN_TOP_MID, 0, 4);
  lv_obj_t *usbIc = lv_label_create(usbBadge);
  lv_label_set_text(usbIc, LV_SYMBOL_USB);
  lv_obj_set_style_text_font(usbIc, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(usbIc, thc(th::ON_TONAL), 0);
  lv_obj_center(usbIc);

  scEmptyTitle = lv_label_create(scEmpty);
  lv_label_set_text(scEmptyTitle, "Nenhum PC ligado");
  lv_obj_set_style_text_font(scEmptyTitle, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(scEmptyTitle, thc(th::TEXT), 0);
  lv_obj_align(scEmptyTitle, LV_ALIGN_TOP_MID, 0, 56);

  scEmptySub = ui_muted(scEmpty, "Pareie o agente no PC para usar atalhos.");
  lv_obj_set_width(scEmptySub, 240);
  lv_label_set_long_mode(scEmptySub, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(scEmptySub, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(scEmptySub, LV_ALIGN_TOP_MID, 0, 80);

  scSetup = lv_obj_get_parent(
      ui_tonal_btn(scEmpty, 75, 120, 150, LV_SYMBOL_PLUS " Parear um PC", [](lv_event_t *) { openPair(nullptr); }));
  lv_obj_add_flag(scEmpty, LV_OBJ_FLAG_HIDDEN);

  scTimer = lv_timer_create(scRefreshCb, 500, nullptr);
  ensureVisibilityTimer();
  paintShortcuts();  // a pagina pode ser montada antes de aparecer: ja nasce com os atalhos
}

void shortcuts_ui_unbuild_page() {
  if (scTimer) lv_timer_del(scTimer);
  scTimer = nullptr;
  memset(scBtn, 0, sizeof(scBtn));
  memset(scLbl, 0, sizeof(scLbl));
  memset(scIcon, 0, sizeof(scIcon));
  scSub = scSent = scSentLbl = scEmpty = scEmptyTitle = scEmptySub = scSetup = nullptr;
  shownCount = -1;
  memset(shownLabels, 0, sizeof(shownLabels));
  sentAt = 0;
}
