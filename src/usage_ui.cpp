#include "usage_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "account_portal.h"
#include "board.h"
#include "settings_ui.h"
#include "theme.h"
#include "ui.h"
#include "wifi_mgr.h"

static constexpr int SCR_W = 320;
static constexpr int SCR_H = 240;
static constexpr float DEG2RAD_PI = 3.14159265f;

static const char *NAMES[PROV_COUNT] = {"Claude", "Codex", "Antigravity"};
static const char *NVS_IA_ORDER = "ia_ord";  // codifica a permutacao dos 3 provedores

static uint32_t provColor(int p) { return p == PROV_CLAUDE ? th::CLAUDE : p == PROV_CODEX ? th::CODEX : th::AGY; }
static const char *DEFAULT_WIN[PROV_COUNT][2] = {{"5h", "Semana"}, {"5h", "Semana"}, {"5h", "Semana"}};

static void openAccount(Provider p);

// ---- ordem das IAs (NVS) -----------------------------------------------------

static int curColIdx = 0;  // indice (0..2) dentro da ordem atual

void usage_ui_get_order(Provider out[PROV_COUNT]) {
  uint8_t v = settings_get_u8(NVS_IA_ORDER, 0);
  Provider p0 = (Provider)(v % 3);
  Provider p1 = (Provider)((v / 3) % 3);
  Provider p2 = (Provider)((v / 9) % 3);
  if (v == 0 || p0 == p1 || p0 == p2 || p1 == p2 || p0 >= PROV_COUNT || p1 >= PROV_COUNT || p2 >= PROV_COUNT) {
    out[0] = PROV_CLAUDE;
    out[1] = PROV_CODEX;
    out[2] = PROV_ANTIGRAVITY;
    return;
  }
  out[0] = p0;
  out[1] = p1;
  out[2] = p2;
}

static void saveOrder(const Provider ord[PROV_COUNT]) {
  uint8_t v = (uint8_t)(ord[0] + ord[1] * 3 + ord[2] * 9);
  settings_set_u8(NVS_IA_ORDER, v);
}

void usage_ui_order_summary(char *buf, int size) {
  Provider ord[PROV_COUNT];
  usage_ui_get_order(ord);
  snprintf(buf, size, "%s, %s, %s", NAMES[ord[0]], NAMES[ord[1]], NAMES[ord[2]]);
}

Provider usage_ui_current_provider() {
  Provider ord[PROV_COUNT];
  usage_ui_get_order(ord);
  if (curColIdx < 0 || curColIdx >= PROV_COUNT) curColIdx = 0;
  return ord[curColIdx];
}

// ---- textos ------------------------------------------------------------------

static void formatAge(uint32_t since, char *buf, int size) {
  uint32_t now;
  if (!since || !board_epoch_now(&now) || now < since) {
    snprintf(buf, size, "?");
    return;
  }
  uint32_t m = (now - since) / 60;
  if (m < 1) snprintf(buf, size, "agora");
  else if (m < 60) snprintf(buf, size, "ha %u min", (unsigned)m);
  else if (m < 48 * 60) snprintf(buf, size, "ha %u h", (unsigned)(m / 60));
  else snprintf(buf, size, "ha %u d", (unsigned)(m / (24 * 60)));
}

struct WinText {
  bool present;
  float pct;
  float elapsed_pct;  // >= 0 quando a janela tem duracao conhecida e reset no futuro
  char name[12], pct_txt[8], reset[18], left_short[16];
};

static uint32_t windowDurationSec(const char *name) {
  if (!strcmp(name, "5h")) return 5 * 3600;
  if (!strcmp(name, "Semana")) return 7 * 86400;
  if (!strcmp(name, "Mes")) return 30 * 86400;
  return 5 * 3600;
}

static WinText windowText(const ProviderUsage &u, Provider p, int k) {
  WinText t = {};
  t.elapsed_pct = -1.0f;
  bool values = u.source == UsageSource::Direct || u.source == UsageSource::Cache;
  const char *name = values ? u.win[k].name : DEFAULT_WIN[p][k];
  t.present = name[0] != 0;
  snprintf(t.name, sizeof(t.name), "%s", name);
  snprintf(t.pct_txt, sizeof(t.pct_txt), "--");
  if (!values || !t.present || u.win[k].pct < 0) return t;

  const UsageWindow &w = u.win[k];
  uint32_t now;
  bool clock = board_epoch_now(&now);
  if (w.reset_epoch && clock && w.reset_epoch <= now) {
    snprintf(t.pct_txt, sizeof(t.pct_txt), "0%%");
    snprintf(t.reset, sizeof(t.reset), "zerou");
    snprintf(t.left_short, sizeof(t.left_short), "zerou");
    return t;
  }
  t.pct = w.pct;
  snprintf(t.pct_txt, sizeof(t.pct_txt), "%d%%", (int)(w.pct + 0.5f));
  if (w.reset_epoch) {
    board_format_time(w.reset_epoch, t.reset, sizeof(t.reset));
    if (clock && w.reset_epoch > now) {
      uint32_t rem = w.reset_epoch - now;
      uint32_t dur = windowDurationSec(t.name);
      if (dur > 0) {
        if (rem >= dur) t.elapsed_pct = 0.0f;
        else t.elapsed_pct = (float)(dur - rem) * 100.0f / (float)dur;
      }
      uint32_t m = (rem + 59) / 60;
      if (m < 60) snprintf(t.left_short, sizeof(t.left_short), "%u min", (unsigned)m);
      else if (m < 48 * 60) snprintf(t.left_short, sizeof(t.left_short), "%uh %02um", (unsigned)(m / 60), (unsigned)(m % 60));
      else snprintf(t.left_short, sizeof(t.left_short), "%u dias", (unsigned)(m / (24 * 60)));
    }
  } else {
    snprintf(t.left_short, sizeof(t.left_short), "--");
  }
  return t;
}

static uint32_t barColor(Provider p, float pct) {
  if (pct >= 90) return th::RED;
  if (pct >= 75) return th::AMBER;
  return provColor(p);
}

static void setText(lv_obj_t *l, const char *text) {
  if (l && strcmp(lv_label_get_text(l), text)) lv_label_set_text(l, text);
}

static void show(lv_obj_t *o, bool on) {
  if (!o) return;
  if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void usage_ui_open_account(Provider p) { openAccount(p); }

// ---- pagina vertical de IA ---------------------------------------------------

struct View {
  bool built;
  bool hasVDots;
  Provider fixedProv;
  lv_obj_t *provDot, *provTitle, *badge;
  lv_obj_t *arc, *tickLine, *pct, *winName;
  lv_point_t tickPts[2];
  lv_obj_t *chipCard, *chipDot, *chipLbl;
  lv_obj_t *vals, *c1Title, *c1Value, *c1Sub, *divLine;
  lv_obj_t *c2Name, *c2Pct, *c2Bar, *c2Sub;
  lv_obj_t *empty, *emptyTitle, *emptyMsg, *emptyBtn, *emptyBtnLbl, *emptyFoot;
  lv_obj_t *vDots[PROV_COUNT];
};

static View mainView;
static View provViews[PROV_COUNT];
static lv_timer_t *usageTimer;

static Provider viewProvider(const View &v) {
  return v.hasVDots ? usage_ui_current_provider() : v.fixedProv;
}

static void updateVDots(View &v) {
  if (!v.hasVDots) return;
  int y = 88;
  for (int i = 0; i < PROV_COUNT; i++) {
    bool on = i == curColIdx;
    int h = on ? 14 : 4;
    lv_obj_set_size(v.vDots[i], 4, h);
    lv_obj_set_pos(v.vDots[i], 310, y);
    lv_obj_set_style_bg_color(v.vDots[i], thc(on ? th::DOT_ON : th::DOT), 0);
    y += h + 5;
  }
}

static void updateView(View &v) {
  if (!v.built) return;
  Provider p = viewProvider(v);
  ProviderUsage u = usage_get(p);
  uint32_t pCol = provColor(p);

  lv_obj_set_style_bg_color(v.provDot, thc(pCol), 0);
  setText(v.provTitle, NAMES[p]);
  updateVDots(v);

  char buf[56];
  char age[20];
  uint32_t bCol = th::MUTED;
  if (usage_is_refreshing(p) && usage_has_account(p)) {
    snprintf(buf, sizeof(buf), "atualizando...");
  } else {
    switch (u.source) {
      case UsageSource::NoAccount:
        snprintf(buf, sizeof(buf), "sem conta");
        break;
      case UsageSource::Waiting:
        snprintf(buf, sizeof(buf), "aguardando rede");
        break;
      case UsageSource::Direct:
        formatAge(u.updated_epoch, age, sizeof(age));
        if (!strcmp(age, "agora")) snprintf(buf, sizeof(buf), "atualizado agora");
        else snprintf(buf, sizeof(buf), "atualizado %s", age);
        break;
      case UsageSource::Cache:
        formatAge(u.updated_epoch, age, sizeof(age));
        snprintf(buf, sizeof(buf), "cache - %s", age);
        bCol = u.error[0] ? th::AMBER : th::MUTED;
        break;
      case UsageSource::Failed:
        snprintf(buf, sizeof(buf), "falha na leitura");
        bCol = th::RED;
        break;
    }
  }
  setText(v.badge, buf);
  lv_obj_set_style_text_color(v.badge, thc(bCol), 0);

  bool values = u.source == UsageSource::Direct || u.source == UsageSource::Cache;
  WinText w0 = windowText(u, p, 0);
  setText(v.pct, w0.pct_txt);
  if (values && w0.present) {
    snprintf(buf, sizeof(buf), "janela %s", w0.name);
    setText(v.winName, buf);
  } else {
    setText(v.winName, u.source == UsageSource::Failed ? "sem leitura" : "janela 5h");
  }
  lv_arc_set_value(v.arc, values ? (int)(w0.pct + 0.5f) : 0);
  lv_obj_set_style_arc_color(v.arc, thc(barColor(p, w0.pct)), LV_PART_INDICATOR);

  // Marca transversal no anel indicando o tempo decorrido da janela.
  if (values && w0.elapsed_pct >= 0.0f) {
    float a = w0.elapsed_pct * 3.6f * DEG2RAD_PI / 180.0f;
    v.tickPts[0] = {(lv_coord_t)(74 + 45 * sinf(a)), (lv_coord_t)(102 - 45 * cosf(a))};
    v.tickPts[1] = {(lv_coord_t)(74 + 61 * sinf(a)), (lv_coord_t)(102 - 61 * cosf(a))};
    lv_line_set_points(v.tickLine, v.tickPts, 2);
    show(v.tickLine, true);
  } else {
    show(v.tickLine, false);
  }

  // Pilula de ritmo/status abaixo do anel (cores opacas pre-misturadas, sem alpha).
  uint32_t chipBg = th::MINI, chipFg = th::MUTED;
  const char *chipTxt = "Sem dados";
  if (usage_is_refreshing(p) && usage_has_account(p)) {
    chipBg = th::MINI;
    chipFg = th::MUTED;
    chipTxt = "Atualizando";
  } else if (values) {
    if (u.source == UsageSource::Cache && u.error[0]) {
      chipBg = th::WARN_BG;
      chipFg = th::AMBER;
      chipTxt = "Cache salvo";
    } else if ((w0.elapsed_pct >= 0.0f && w0.pct > w0.elapsed_pct + 5.0f) || w0.pct >= 80.0f) {
      chipBg = th::WARN_BG;
      chipFg = th::AMBER;
      chipTxt = "Acima do ritmo";
    } else {
      chipBg = th::OK_BG;
      chipFg = th::GREEN;
      chipTxt = "No ritmo";
    }
  } else if (u.source == UsageSource::Failed) {
    chipBg = th::BAD_BG;
    chipFg = th::RED;
    chipTxt = "Falhou";
  } else if (u.source == UsageSource::NoAccount) {
    chipBg = th::MINI;
    chipFg = th::MUTED;
    chipTxt = "Sem conta";
  } else {
    chipBg = th::MINI;
    chipFg = th::MUTED;
    chipTxt = "Aguardando";
  }
  lv_obj_set_style_bg_color(v.chipCard, thc(chipBg), 0);
  lv_obj_set_style_bg_color(v.chipDot, thc(chipFg), 0);
  lv_obj_set_style_text_color(v.chipLbl, thc(chipFg), 0);
  setText(v.chipLbl, chipTxt);

  show(v.vals, values);
  show(v.empty, !values);

  if (!values) {
    if (u.source == UsageSource::NoAccount) {
      setText(v.emptyTitle, "Sem conta ligada");
      setText(v.emptyMsg, "Configure pelo celular ou PC para ver o uso.");
      setText(v.emptyBtnLbl, LV_SYMBOL_PLUS " Ligar conta");
      show(v.emptyFoot, false);
    } else if (u.source == UsageSource::Failed) {
      bool authErr = strstr(u.error, "expirou") || strstr(u.error, "recusado");
      setText(v.emptyTitle, authErr ? "Conta expirou" : "Leitura falhou");
      setText(v.emptyMsg, authErr ? "Entre de novo para voltar a ver o uso." : "Nao foi possivel consultar agora.");
      setText(v.emptyBtnLbl, LV_SYMBOL_REFRESH " Reconectar");
      setText(v.emptyFoot, "Detalhes em Sistema > Diagnostico");
      show(v.emptyFoot, true);
    } else {
      setText(v.emptyTitle, "Aguardando rede");
      setText(v.emptyMsg, "Conectando ao Wi-Fi e sincronizando a hora.");
      setText(v.emptyBtnLbl, LV_SYMBOL_REFRESH " Atualizar");
      show(v.emptyFoot, false);
    }
    return;
  }

  setText(v.c1Title, "Zera em");
  setText(v.c1Value, w0.left_short[0] ? w0.left_short : "--");
  if (w0.reset[0] && strcmp(w0.reset, "zerou") != 0) {
    if (w0.reset[0] >= '0' && w0.reset[0] <= '9') snprintf(buf, sizeof(buf), "as %s", w0.reset);
    else snprintf(buf, sizeof(buf), "%s", w0.reset);
  } else {
    snprintf(buf, sizeof(buf), "janela livre");
  }
  setText(v.c1Sub, buf);

  WinText w1 = windowText(u, p, 1);
  if (w1.present) {
    setText(v.c2Name, w1.name);
    setText(v.c2Pct, w1.pct_txt);
    show(v.c2Pct, true);
    show(v.c2Bar, true);
    lv_bar_set_value(v.c2Bar, (int)(w1.pct + 0.5f), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(v.c2Bar, thc(barColor(p, w1.pct)), LV_PART_INDICATOR);
    if (w1.reset[0]) {
      if (w1.reset[0] >= '0' && w1.reset[0] <= '9') snprintf(buf, sizeof(buf), "zera as %s", w1.reset);
      else snprintf(buf, sizeof(buf), "zera %s", w1.reset);
    } else {
      buf[0] = 0;
    }
    setText(v.c2Sub, buf);
    lv_obj_set_y(v.c2Sub, 128);
  } else {
    setText(v.c2Name, "Plano");
    show(v.c2Pct, false);
    show(v.c2Bar, false);
    setText(v.c2Sub, "Janela unica de 30 dias");
    lv_obj_set_y(v.c2Sub, 110);
  }
}

static void refreshAllViews() {
  if (mainView.built) updateView(mainView);
  for (int i = 0; i < PROV_COUNT; i++)
    if (provViews[i].built) updateView(provViews[i]);
}

bool usage_ui_step(int delta) {
  int next = curColIdx + delta;
  if (next < 0 || next >= PROV_COUNT) return false;
  curColIdx = next;
  if (mainView.built) updateView(mainView);
  return true;
}

void usage_ui_select_provider(Provider p) {
  Provider ord[PROV_COUNT];
  usage_ui_get_order(ord);
  for (int i = 0; i < PROV_COUNT; i++) {
    if (ord[i] == p) {
      curColIdx = i;
      break;
    }
  }
  if (mainView.built) updateView(mainView);
}

void usage_ui_move_order(int idx, int dir) {
  int dst = idx + dir;
  if (idx < 0 || idx >= PROV_COUNT || dst < 0 || dst >= PROV_COUNT) return;
  Provider cur = usage_ui_current_provider();
  Provider ord[PROV_COUNT];
  usage_ui_get_order(ord);
  Provider tmp = ord[idx];
  ord[idx] = ord[dst];
  ord[dst] = tmp;
  saveOrder(ord);
  for (int i = 0; i < PROV_COUNT; i++)
    if (ord[i] == cur) curColIdx = i;
  if (mainView.built) updateView(mainView);
}

static void cardTapCb(lv_event_t *e) {
  View *v = (View *)lv_event_get_user_data(e);
  Provider p = viewProvider(*v);
  if (!usage_has_account(p)) {
    openAccount(p);
    return;
  }
  usage_refresh_now();
  refreshAllViews();
}

static void emptyActionCb(lv_event_t *e) {
  View *v = (View *)lv_event_get_user_data(e);
  Provider p = viewProvider(*v);
  ProviderUsage u = usage_get(p);
  if (u.source == UsageSource::Waiting && usage_has_account(p)) {
    usage_refresh_now();
    refreshAllViews();
    return;
  }
  openAccount(p);
}

static void vDotStepCb(lv_event_t *e) {
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  if (!usage_ui_step(dir)) {
    curColIdx = dir > 0 ? 0 : PROV_COUNT - 1;
    if (mainView.built) updateView(mainView);
  }
}

static void timerCb(lv_timer_t *) {
  if (ui_is_animating()) return;
  refreshAllViews();
}

static void ensureTimer() {
  if (!usageTimer) usageTimer = lv_timer_create(timerCb, 1000, nullptr);
}

static void stopTimerIfIdle() {
  if (mainView.built) return;
  for (int i = 0; i < PROV_COUNT; i++)
    if (provViews[i].built) return;
  if (usageTimer) {
    lv_timer_del(usageTimer);
    usageTimer = nullptr;
  }
}

static void buildView(lv_obj_t *tile, View &v, bool withVDots, Provider fixedProv) {
  memset(&v, 0, sizeof(v));
  v.built = true;
  v.hasVDots = withVDots;
  v.fixedProv = fixedProv;
  Provider p = viewProvider(v);

  // Cabecalho: ponto + nome a esquerda, idade da leitura a direita.
  v.provDot = ui_dot(tile, 10, provColor(p));
  lv_obj_set_pos(v.provDot, 12, 16);
  v.provTitle = ui_title(tile, NAMES[p]);
  lv_obj_set_pos(v.provTitle, 28, 10);

  v.badge = ui_muted(tile, "");
  lv_label_set_long_mode(v.badge, LV_LABEL_LONG_DOT);
  lv_obj_set_width(v.badge, 154);
  lv_obj_set_style_text_align(v.badge, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(v.badge, LV_ALIGN_TOP_RIGHT, -14, 15);

  // Coluna esquerda: anel 120x120 da janela de 5h + marca de tempo decorrido + pilula de ritmo.
  v.arc = lv_arc_create(tile);
  lv_obj_set_size(v.arc, 120, 120);
  lv_obj_set_pos(v.arc, 14, 42);
  lv_arc_set_rotation(v.arc, 270);
  lv_arc_set_bg_angles(v.arc, 0, 360);
  lv_arc_set_range(v.arc, 0, 100);
  lv_obj_remove_style(v.arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(v.arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(v.arc, 0, 0);
  lv_obj_set_style_arc_width(v.arc, 11, 0);
  lv_obj_set_style_arc_color(v.arc, thc(th::TRACK), 0);
  lv_obj_set_style_arc_width(v.arc, 11, LV_PART_INDICATOR);

  v.pct = lv_label_create(v.arc);
  lv_obj_set_style_text_font(v.pct, &lv_font_montserrat_28, 0);
  lv_obj_align(v.pct, LV_ALIGN_CENTER, 0, -7);
  lv_label_set_text(v.pct, "--");

  v.winName = ui_muted(v.arc, "janela 5h");
  lv_obj_align(v.winName, LV_ALIGN_CENTER, 0, 17);

  v.tickLine = lv_line_create(tile);
  lv_obj_set_style_line_width(v.tickLine, 3, 0);
  lv_obj_set_style_line_color(v.tickLine, thc(th::TEXT), 0);
  lv_obj_set_style_line_rounded(v.tickLine, true, 0);
  v.tickPts[0] = {74, 46};
  v.tickPts[1] = {74, 62};
  lv_line_set_points(v.tickLine, v.tickPts, 2);

  v.chipCard = ui_card(tile, 10, 170, 128, 24, th::OK_BG);
  lv_obj_set_style_radius(v.chipCard, LV_RADIUS_CIRCLE, 0);
  v.chipDot = ui_dot(v.chipCard, 6, th::GREEN);
  lv_obj_align(v.chipDot, LV_ALIGN_LEFT_MID, 8, 0);
  v.chipLbl = lv_label_create(v.chipCard);
  lv_obj_set_style_text_font(v.chipLbl, &lv_font_montserrat_12, 0);
  lv_label_set_long_mode(v.chipLbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(v.chipLbl, 106);
  lv_obj_align(v.chipLbl, LV_ALIGN_LEFT_MID, 18, 0);
  lv_label_set_text(v.chipLbl, "");

  // Coluna direita (estado com leitura): Zera em + tempo restante + divisoria + Semana.
  v.vals = lv_obj_create(tile);
  lv_obj_remove_style_all(v.vals);
  lv_obj_set_size(v.vals, 154, 162);
  lv_obj_set_pos(v.vals, 148, 42);
  lv_obj_add_flag(v.vals, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(v.vals, cardTapCb, LV_EVENT_CLICKED, &v);

  v.c1Title = ui_muted(v.vals, "Zera em", &lv_font_montserrat_14);
  lv_obj_set_pos(v.c1Title, 0, 0);

  v.c1Value = lv_label_create(v.vals);
  lv_obj_set_style_text_font(v.c1Value, &lv_font_montserrat_28, 0);
  lv_label_set_long_mode(v.c1Value, LV_LABEL_LONG_DOT);
  lv_obj_set_width(v.c1Value, 152);
  lv_obj_set_pos(v.c1Value, 0, 18);
  lv_label_set_text(v.c1Value, "--");

  v.c1Sub = ui_muted(v.vals, "", &lv_font_montserrat_14);
  lv_label_set_long_mode(v.c1Sub, LV_LABEL_LONG_DOT);
  lv_obj_set_width(v.c1Sub, 152);
  lv_obj_set_pos(v.c1Sub, 0, 52);

  v.divLine = lv_obj_create(v.vals);
  lv_obj_remove_style_all(v.divLine);
  lv_obj_set_size(v.divLine, 152, 1);
  lv_obj_set_pos(v.divLine, 0, 78);
  lv_obj_set_style_bg_color(v.divLine, thc(th::LINE), 0);
  lv_obj_set_style_bg_opa(v.divLine, LV_OPA_COVER, 0);

  v.c2Name = ui_muted(v.vals, "Semana", &lv_font_montserrat_14);
  lv_obj_set_pos(v.c2Name, 0, 88);

  v.c2Pct = lv_label_create(v.vals);
  lv_obj_set_style_text_font(v.c2Pct, &lv_font_montserrat_16, 0);
  lv_obj_set_width(v.c2Pct, 64);
  lv_obj_set_style_text_align(v.c2Pct, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(v.c2Pct, 88, 86);
  lv_label_set_text(v.c2Pct, "");

  v.c2Bar = ui_bar(v.vals, 152, 6, provColor(p));
  lv_obj_set_pos(v.c2Bar, 0, 112);

  v.c2Sub = ui_muted(v.vals, "");
  lv_label_set_long_mode(v.c2Sub, LV_LABEL_LONG_DOT);
  lv_obj_set_width(v.c2Sub, 152);
  lv_obj_set_pos(v.c2Sub, 0, 126);

  // Coluna direita (estado sem leitura / falha): texto claro + botao Reconectar/Ligar conta.
  v.empty = lv_obj_create(tile);
  lv_obj_remove_style_all(v.empty);
  lv_obj_set_size(v.empty, 154, 162);
  lv_obj_set_pos(v.empty, 148, 44);

  v.emptyTitle = lv_label_create(v.empty);
  lv_obj_set_style_text_font(v.emptyTitle, &lv_font_montserrat_16, 0);
  lv_obj_set_pos(v.emptyTitle, 0, 0);
  lv_label_set_text(v.emptyTitle, "");

  v.emptyMsg = ui_muted(v.empty, "", &lv_font_montserrat_14);
  lv_obj_set_width(v.emptyMsg, 150);
  lv_label_set_long_mode(v.emptyMsg, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(v.emptyMsg, 0, 22);

  v.emptyBtnLbl = ui_tonal_btn(v.empty, 0, 76, 146, "", emptyActionCb, &v, 36);
  v.emptyBtn = lv_obj_get_parent(v.emptyBtnLbl);

  v.emptyFoot = ui_muted(v.empty, "");
  lv_obj_set_width(v.emptyFoot, 150);
  lv_label_set_long_mode(v.emptyFoot, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(v.emptyFoot, 0, 120);

  if (withVDots) {
    for (int i = 0; i < PROV_COUNT; i++) {
      v.vDots[i] = ui_dot(tile, 4, th::DOT);
    }
    // Toque na borda direita tambem avanca para a proxima IA da coluna.
    lv_obj_t *hitNext = lv_obj_create(tile);
    lv_obj_remove_style_all(hitNext);
    lv_obj_set_size(hitNext, 24, 120);
    lv_obj_set_pos(hitNext, 296, 60);
    lv_obj_add_flag(hitNext, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hitNext, vDotStepCb, LV_EVENT_CLICKED, (void *)1);
  }

  ensureTimer();
  updateView(v);
}

void usage_ui_build_page(lv_obj_t *tile) { buildView(tile, mainView, true, PROV_CLAUDE); }

void usage_ui_unbuild_page() {
  memset(&mainView, 0, sizeof(mainView));
  stopTimerIfIdle();
}

void usage_ui_build_provider_page(lv_obj_t *tile, Provider p) { buildView(tile, provViews[p], false, p); }

void usage_ui_unbuild_provider_page(Provider p) {
  memset(&provViews[p], 0, sizeof(provViews[p]));
  stopTimerIfIdle();
}

// ---- tela Ordem das IAs (Configuracoes > Tela > Ordem das IAs) ---------------

static lv_obj_t *ordScreen;
static lv_obj_t *ordDot[PROV_COUNT], *ordName[PROV_COUNT], *ordUp[PROV_COUNT], *ordDown[PROV_COUNT];

static void refreshOrderScreen() {
  if (!ordScreen) return;
  Provider ord[PROV_COUNT];
  usage_ui_get_order(ord);
  for (int i = 0; i < PROV_COUNT; i++) {
    Provider p = ord[i];
    lv_obj_set_style_bg_color(ordDot[i], thc(provColor(p)), 0);
    lv_label_set_text(ordName[i], NAMES[p]);

    bool canUp = i > 0;
    bool canDown = i < PROV_COUNT - 1;
    lv_obj_set_style_text_color(ordUp[i], thc(canUp ? th::ON_MINI : th::OFF_TEXT), 0);
    lv_obj_set_style_text_color(ordDown[i], thc(canDown ? th::ON_MINI : th::OFF_TEXT), 0);
    if (canUp) lv_obj_add_flag(ordUp[i], LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_clear_flag(ordUp[i], LV_OBJ_FLAG_CLICKABLE);
    if (canDown) lv_obj_add_flag(ordDown[i], LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_clear_flag(ordDown[i], LV_OBJ_FLAG_CLICKABLE);
  }
}

static void ordMoveCb(lv_event_t *e) {
  int code = (int)(intptr_t)lv_event_get_user_data(e);
  int idx = code / 10;
  int dir = (code % 10) == 1 ? -1 : 1;
  usage_ui_move_order(idx, dir);
  refreshOrderScreen();
}

static void ordBackCb(lv_event_t *) {
  lv_obj_del_async(ordScreen);
  ordScreen = nullptr;
  settings_ui_child_closed();
}

void usage_ui_open_order() {
  if (ordScreen || !ui_mem_ok(7 * 1024)) return;
  ordScreen = ui_overlay();
  ui_back_header(ordScreen, "Ordem das IAs", ordBackCb);

  lv_obj_t *sub = ui_muted(ordScreen, "Use as setas para mudar a ordem na coluna.");
  lv_obj_set_pos(sub, 12, 42);

  lv_obj_t *list = ui_list(ordScreen, 10, 62, 300);
  for (int i = 0; i < PROV_COUNT; i++) {
    char num[4];
    snprintf(num, sizeof(num), "%d", i + 1);
    UiRow r = ui_list_row(list, nullptr, "", nullptr, nullptr, nullptr, 46);
    lv_obj_t *n = ui_muted(r.row, num);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 12, 0);

    ordDot[i] = ui_dot(r.row, 9, th::CLAUDE);
    lv_obj_align(ordDot[i], LV_ALIGN_LEFT_MID, 28, 0);

    ordName[i] = r.title;
    lv_obj_align(ordName[i], LV_ALIGN_LEFT_MID, 46, 0);

    ordUp[i] = ui_icon_btn(r.row, 214, 7, LV_SYMBOL_UP, ordMoveCb, (void *)(intptr_t)(i * 10 + 1));
    ordDown[i] = ui_icon_btn(r.row, 254, 7, LV_SYMBOL_DOWN, ordMoveCb, (void *)(intptr_t)(i * 10 + 2));
  }

  lv_obj_t *foot = ui_muted(ordScreen, "A 1a abre primeiro na pagina de IA.");
  lv_obj_set_pos(foot, 12, 212);

  refreshOrderScreen();
}

// ---- tela da conta -----------------------------------------------------------

static lv_obj_t *acct, *acctCard, *acctStatus, *acctButtons, *acctSub, *btnForgetLbl;
static lv_obj_t *btnRefresh, *btnForget, *btnSetup, *btnSetupLbl;
static lv_obj_t *subCode, *subInfo;
static lv_timer_t *acctTimer;
static Provider acctProv;
static uint32_t forgetArmedAt;
static bool subIsLogin;

static void closeAccount() {
  account_portal_stop();
  codex_login_cancel();
  lv_timer_del(acctTimer);
  acctTimer = nullptr;
  lv_obj_del_async(acct);
  acct = acctCard = acctStatus = acctButtons = acctSub = subCode = subInfo = nullptr;
  btnRefresh = btnForget = btnSetup = btnSetupLbl = nullptr;
  settings_ui_child_closed();
}

static void closeSub() {
  if (!acctSub) return;
  lv_obj_del(acctSub);
  acctSub = subCode = subInfo = nullptr;
  lv_obj_clear_flag(acctCard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(acctButtons, LV_OBJ_FLAG_HIDDEN);
}

static void updateAccount(lv_timer_t *) {
  ProviderUsage u = usage_get(acctProv);

  AccountPortalInfo info = account_portal_info();
  if (acctSub && !subIsLogin && info.saved && info.saved_provider == acctProv && u.attempts > info.saved_attempts)
    closeSub();

  if (acctSub && subIsLogin) {
    CodexLogin l = codex_login_status();
    if (l.state == LoginState::Waiting) {
      lv_label_set_text(subCode, l.code);
      lv_label_set_text_fmt(subInfo, "No celular, abra\n%s\ne digite o codigo.\nValido por %u min.", l.url,
                            (unsigned)((l.seconds_left + 59) / 60));
    } else if (l.state == LoginState::Requesting || l.state == LoginState::Exchanging) {
      lv_label_set_text(subCode, "...");
      lv_label_set_text(subInfo, l.state == LoginState::Requesting ? "Pedindo o codigo..." : "Confirmado, entrando...");
    } else {
      closeSub();
    }
  }

  char buf[220];
  int n = snprintf(buf, sizeof(buf), "%s\n", usage_has_account(acctProv) ? "Conta configurada" : "Sem conta configurada");
  if (u.updated_epoch) {
    char t[16], age[20];
    board_format_time(u.updated_epoch, t, sizeof(t));
    formatAge(u.updated_epoch, age, sizeof(age));
    n += snprintf(buf + n, sizeof(buf) - n, "Ultima leitura: %s (%s)\n", t, age);
  }
  if (u.source == UsageSource::Direct) {
    n += snprintf(buf + n, sizeof(buf) - n, "Uso: %s %d%%", u.win[0].name, (int)(u.win[0].pct + 0.5f));
    if (u.win[1].name[0]) n += snprintf(buf + n, sizeof(buf) - n, ", %s %d%%", u.win[1].name, (int)(u.win[1].pct + 0.5f));
    n += snprintf(buf + n, sizeof(buf) - n, "\n");
  }
  if (u.error[0]) n += snprintf(buf + n, sizeof(buf) - n, "Ultima falha: %s\n", u.error);
  CodexLogin l = codex_login_status();
  if (acctProv == PROV_CODEX && l.state == LoginState::Failed) n += snprintf(buf + n, sizeof(buf) - n, "Login: %s\n", l.error);
  bool has = usage_has_account(acctProv);
  if (has && acctProv == PROV_CLAUDE) snprintf(buf + n, sizeof(buf) - n, "Consulta a cada 5 min com a janela de 5h aberta.");
  else if (has) snprintf(buf + n, sizeof(buf) - n, "Consulta a cada 5 min.");
  lv_label_set_text(acctStatus, buf);

  if (btnRefresh) {
    show(btnRefresh, has);
    show(btnForget, has);
    lv_obj_set_y(btnSetup, has ? 42 : 0);
    const char *label = acctProv == PROV_CODEX ? (has ? LV_SYMBOL_PLAY " Entrar de novo com codigo" : LV_SYMBOL_PLAY " Entrar com codigo no celular")
                                               : (has ? LV_SYMBOL_SETTINGS " Trocar token (navegador)" : LV_SYMBOL_SETTINGS " Configurar pelo navegador");
    if (strcmp(lv_label_get_text(btnSetupLbl), label)) lv_label_set_text(btnSetupLbl, label);
  }

  if (forgetArmedAt && board_millis() - forgetArmedAt > 3000) {
    forgetArmedAt = 0;
    lv_label_set_text(btnForgetLbl, LV_SYMBOL_TRASH " Remover");
  }
}

static void refreshNowCb(lv_event_t *) { usage_refresh_now(); }

static void forgetCb(lv_event_t *) {
  if (!forgetArmedAt) {
    forgetArmedAt = board_millis();
    lv_label_set_text(btnForgetLbl, "Toque de novo");
    return;
  }
  forgetArmedAt = 0;
  usage_forget_account(acctProv);
  lv_label_set_text(btnForgetLbl, LV_SYMBOL_TRASH " Remover");
}

static lv_obj_t *openSub(const char *qrText, bool login) {
  subIsLogin = login;
  lv_obj_add_flag(acctButtons, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(acctCard, LV_OBJ_FLAG_HIDDEN);
  acctSub = lv_obj_create(acct);
  lv_obj_remove_style_all(acctSub);
  lv_obj_set_size(acctSub, SCR_W, SCR_H - 44);
  lv_obj_set_pos(acctSub, 0, 44);

  lv_obj_t *qr = lv_qrcode_create(acctSub, 100, lv_color_black(), lv_color_white());
  lv_obj_set_style_border_color(qr, lv_color_white(), 0);
  lv_obj_set_style_border_width(qr, 6, 0);
  lv_obj_set_pos(qr, 10, 8);
  lv_qrcode_update(qr, qrText, strlen(qrText));

  subInfo = lv_label_create(acctSub);
  lv_obj_set_width(subInfo, 178);
  lv_label_set_long_mode(subInfo, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(subInfo, &lv_font_montserrat_12, 0);
  lv_obj_set_pos(subInfo, 130, 6);

  lv_obj_t *hint = ui_muted(acctSub, "Codigo:");
  lv_obj_set_pos(hint, 130, 112);
  subCode = lv_label_create(acctSub);
  lv_obj_set_style_text_font(subCode, &lv_font_montserrat_28, 0);
  lv_obj_set_pos(subCode, 130, 126);
  return acctSub;
}

static void configureCb(lv_event_t *) {
  if (!account_portal_start()) {
    lv_label_set_text(acctStatus, "Conecte o Wi-Fi primeiro\n(Ajustes > Wi-Fi).");
    return;
  }
  AccountPortalInfo info = account_portal_info();
  openSub(info.url, false);
  lv_label_set_text(subCode, info.code);
  if (acctProv == PROV_ANTIGRAVITY)
    lv_label_set_text_fmt(subInfo, "Endereco:\n%s\nNo PC rode\nsend_antigravity.py e\ninforme endereco e codigo.", info.url + 7);
  else
    lv_label_set_text_fmt(subInfo, "No PC ou celular (mesma rede), abra:\n%s\n\nRode claude setup-token no PC e cole o token.",
                          info.url);
}

static void loginCb(lv_event_t *) {
  if (wifi_status().state != WifiState::Connected) {
    lv_label_set_text(acctStatus, "Conecte o Wi-Fi primeiro\n(Ajustes > Wi-Fi).");
    return;
  }
  codex_login_start();
  openSub("https://auth.openai.com/codex/device", true);
  lv_label_set_text(subInfo, "Pedindo o codigo...");
  lv_label_set_text(subCode, "...");
}

static void backCb(lv_event_t *) { closeAccount(); }

static void openAccount(Provider p) {
  if (acct || !ui_mem_ok(6 * 1024)) return;
  acctProv = p;
  forgetArmedAt = 0;
  acct = ui_overlay();
  ui_back_header(acct, NAMES[p], backCb);

  acctCard = ui_card(acct, 10, 48, 300, 100);
  acctStatus = lv_label_create(acctCard);
  lv_obj_set_width(acctStatus, 280);
  lv_label_set_long_mode(acctStatus, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(acctStatus, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_line_space(acctStatus, 3, 0);
  lv_obj_set_pos(acctStatus, 10, 9);

  acctButtons = lv_obj_create(acct);
  lv_obj_remove_style_all(acctButtons);
  lv_obj_set_size(acctButtons, SCR_W, 86);
  lv_obj_set_pos(acctButtons, 0, 156);
  btnRefresh = lv_obj_get_parent(ui_tonal_btn(acctButtons, 10, 0, 145, LV_SYMBOL_REFRESH " Atualizar", refreshNowCb));
  btnForgetLbl = ui_tonal_btn(acctButtons, 165, 0, 145, LV_SYMBOL_TRASH " Remover", forgetCb);
  ui_btn_danger(btnForgetLbl);
  btnForget = lv_obj_get_parent(btnForgetLbl);
  btnSetupLbl = ui_tonal_btn(acctButtons, 10, 42, 300, "", p == PROV_CODEX ? loginCb : configureCb);
  btnSetup = lv_obj_get_parent(btnSetupLbl);

  acctTimer = lv_timer_create(updateAccount, 1000, nullptr);
  updateAccount(nullptr);
}
