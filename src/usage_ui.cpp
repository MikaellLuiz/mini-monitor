#include "usage_ui.h"

#include <stdio.h>
#include <string.h>

#include "account_portal.h"
#include "board.h"
#include "theme.h"
#include "settings_ui.h"
#include "ui.h"
#include "wifi_mgr.h"

static constexpr int SCR_W = 320;
static constexpr int SCR_H = 240;

static const char *NAMES[PROV_COUNT] = {"Claude", "Codex", "Antigravity"};
static uint32_t provColor(int p) { return p == PROV_CLAUDE ? th::CLAUDE : p == PROV_CODEX ? th::CODEX : th::AGY; }
// Nomes das barras enquanto ainda nao ha leitura (depois vem do proprio dado).
static const char *DEFAULT_WIN[PROV_COUNT][2] = {{"5h", "Semana"}, {"5h", "Semana"}, {"5h", "Semana"}};

static void openAccount(Provider p);

// ---- textos ------------------------------------------------------------------

static void formatAge(uint32_t since, char *buf, int size) {
  uint32_t now;
  if (!since || !board_epoch_now(&now) || now < since) {
    snprintf(buf, size, "?");
    return;
  }
  uint32_t m = (now - since) / 60;
  if (m < 1) snprintf(buf, size, "agora");
  else if (m < 60) snprintf(buf, size, "%u min", (unsigned)m);
  else if (m < 48 * 60) snprintf(buf, size, "%u h", (unsigned)(m / 60));
  else snprintf(buf, size, "%u d", (unsigned)(m / (24 * 60)));
}

// Uma janela pronta para mostrar: porcentagem ("37%", "--"), horario do reset
// ("15:40", "zerou", "") e tempo que falta ("em 1h 38min", "").
struct WinText {
  bool present;
  float pct;  // para a barra (0 se desconhecido)
  char name[8], pct_txt[8], reset[16], left[20];
};

static WinText windowText(const ProviderUsage &u, Provider p, int k) {
  WinText t = {};
  bool values = u.source == UsageSource::Direct || u.source == UsageSource::Cache;
  // Com leitura, a janela existe so se o dado a trouxe (o plano free do Codex tem uma so).
  const char *name = values ? u.win[k].name : DEFAULT_WIN[p][k];
  t.present = name[0] != 0;
  snprintf(t.name, sizeof(t.name), "%s", name);
  snprintf(t.pct_txt, sizeof(t.pct_txt), "--");
  if (!values || !t.present || u.win[k].pct < 0) return t;

  const UsageWindow &w = u.win[k];
  uint32_t now;
  bool clock = board_epoch_now(&now);
  if (w.reset_epoch && clock && w.reset_epoch <= now) {  // janela vencida: zerou
    snprintf(t.pct_txt, sizeof(t.pct_txt), "0%%");
    snprintf(t.reset, sizeof(t.reset), "zerou");
    return t;
  }
  t.pct = w.pct;
  snprintf(t.pct_txt, sizeof(t.pct_txt), "%d%%", (int)(w.pct + 0.5f));
  if (w.reset_epoch) {
    board_format_time(w.reset_epoch, t.reset, sizeof(t.reset));
    if (clock) {
      uint32_t m = (w.reset_epoch - now + 59) / 60;
      if (m < 60) snprintf(t.left, sizeof(t.left), "em %u min", (unsigned)m);
      else if (m < 48 * 60) snprintf(t.left, sizeof(t.left), "em %uh %02umin", (unsigned)(m / 60), (unsigned)(m % 60));
      else snprintf(t.left, sizeof(t.left), "em %u dias", (unsigned)(m / (24 * 60)));
    }
  }
  return t;
}

// Selo de origem: texto e cor.
static uint32_t badgeText(const ProviderUsage &u, char *buf, int size) {
  char age[16];
  switch (u.source) {
    case UsageSource::NoAccount:
      snprintf(buf, size, "toque para configurar");
      return th::ON_TONAL;
    case UsageSource::Waiting:
      snprintf(buf, size, "aguardando rede");
      return th::MUTED;
    case UsageSource::Direct:
      formatAge(u.updated_epoch, age, sizeof(age));
      snprintf(buf, size, "Direto - %s", age);
      return th::GREEN;
    case UsageSource::Cache:
      formatAge(u.updated_epoch, age, sizeof(age));
      snprintf(buf, size, "Cache - %s", age);
      return u.error[0] ? th::AMBER : th::MUTED;
    case UsageSource::Failed:
      snprintf(buf, size, "Falha: %s", u.error);
      return th::RED;
  }
  buf[0] = 0;
  return th::MUTED;
}

static uint32_t barColor(Provider p, float pct) {
  if (pct >= 90) return th::RED;
  if (pct >= 70) return th::AMBER;
  return provColor(p);
}

static void setText(lv_obj_t *l, const char *text) {
  if (strcmp(lv_label_get_text(l), text)) lv_label_set_text(l, text);
}

static void show(lv_obj_t *o, bool on) {
  if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void usage_ui_open_account(Provider p) { openAccount(p); }

// Toque no cartao: com conta, atualiza agora; sem conta, abre a configuracao dela.
static bool cardsBuilt;
static lv_timer_t *cardsTimer, *provTimer;
static void updateCard(Provider p);
static void updateProvPage(Provider p);
static bool provBuilt(int i);
static void accountCb(lv_event_t *e) {
  Provider p = (Provider)(intptr_t)lv_event_get_user_data(e);
  if (!usage_has_account(p)) {
    openAccount(p);
    return;
  }
  usage_refresh_now();
  for (int i = 0; i < PROV_COUNT; i++) {
    if (cardsBuilt) updateCard((Provider)i);
    if (provBuilt(i)) updateProvPage((Provider)i);
  }
}

// Engrenagem no canto de baixo a direita das paginas de IA: abre Configuracoes > Contas.
static void gearCb(lv_event_t *) { settings_ui_open_category(SETCAT_ACCOUNTS); }
static void addGear(lv_obj_t *tile) { ui_gear_btn(tile, gearCb); }

// ---- pagina com as tres IAs ---------------------------------------------------

struct Card {
  bool built;
  lv_obj_t *badge, *wl[2], *wp[2], *wr[2], *bar[2];  // nome, porcentagem, reset, barra
};
static Card cards[PROV_COUNT];

static void updateCard(Provider p) {
  Card &c = cards[p];
  ProviderUsage u = usage_get(p);
  char badge[48];
  uint32_t color = badgeText(u, badge, sizeof(badge));
  if (usage_is_refreshing(p) && usage_has_account(p)) {
    snprintf(badge, sizeof(badge), "atualizando...");
    color = th::MUTED;
  }
  setText(c.badge, badge);
  lv_obj_set_style_text_color(c.badge, thc(color), 0);

  // Cache: valores apagados, para nao parecerem atuais.
  lv_opa_t opa = u.source == UsageSource::Cache ? LV_OPA_60 : LV_OPA_COVER;
  for (int k = 0; k < 2; k++) {
    WinText t = windowText(u, p, k);
    show(c.wl[k], t.present);
    show(c.wp[k], t.present);
    show(c.bar[k], t.present);
    show(c.wr[k], t.present);
    if (!t.present) continue;
    char reset[40] = "";  // "16:20 - em 1h 48min": horario e quanto falta, numa linha so
    if (t.reset[0] && t.left[0]) snprintf(reset, sizeof(reset), "%s - %s", t.reset, t.left);
    else if (t.reset[0]) snprintf(reset, sizeof(reset), "%s", t.reset);
    setText(c.wl[k], t.name);
    setText(c.wr[k], reset);
    setText(c.wp[k], t.pct_txt);
    lv_bar_set_value(c.bar[k], (int)t.pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(c.bar[k], thc(barColor(p, t.pct)), LV_PART_INDICATOR);
    lv_obj_set_style_opa(c.wp[k], opa, 0);
    lv_obj_set_style_opa(c.wr[k], opa, 0);
    lv_obj_set_style_opa(c.bar[k], opa, 0);
  }
}

static void makeCard(lv_obj_t *parent, int y, Provider p) {
  Card &c = cards[p];
  c.built = true;
  lv_obj_t *card = ui_card(parent, 8, y, 304, 68);
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(card, thc(th::ROW_PR), LV_STATE_PRESSED);
  lv_obj_add_event_cb(card, accountCb, LV_EVENT_CLICKED, (void *)(intptr_t)p);

  lv_obj_t *dot = ui_dot(card, 7, provColor(p));
  lv_obj_set_pos(dot, 11, 11);
  lv_obj_t *n = lv_label_create(card);
  lv_label_set_text(n, NAMES[p]);
  lv_obj_set_style_text_font(n, &lv_font_montserrat_16, 0);
  lv_obj_set_pos(n, 24, 3);

  c.badge = ui_muted(card, "");
  lv_obj_align(c.badge, LV_ALIGN_TOP_RIGHT, -10, 6);

  for (int k = 0; k < 2; k++) {
    int x = 10 + k * 148;
    c.wl[k] = ui_muted(card, "", &lv_font_montserrat_14);
    lv_obj_set_pos(c.wl[k], x, 25);
    c.wp[k] = lv_label_create(card);
    lv_obj_set_style_text_font(c.wp[k], &lv_font_montserrat_16, 0);
    lv_obj_set_width(c.wp[k], 70);
    lv_obj_set_style_text_align(c.wp[k], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(c.wp[k], x + 66, 23);
    lv_label_set_text(c.wp[k], "");
    c.bar[k] = ui_bar(card, 136, 5, provColor(p));
    lv_obj_set_pos(c.bar[k], x, 44);
    c.wr[k] = ui_muted(card, "");
    lv_label_set_long_mode(c.wr[k], LV_LABEL_LONG_DOT);
    lv_obj_set_size(c.wr[k], 138, 15);
    lv_obj_set_pos(c.wr[k], x, 51);
  }
}

static void refreshCardsCb(lv_timer_t *) {
  if (!ui_page_shown(PG_USAGE) || ui_is_animating()) return;
  for (int i = 0; i < PROV_COUNT; i++) updateCard((Provider)i);
}

void usage_ui_build_page(lv_obj_t *tile) {
  makeCard(tile, 4, PROV_CLAUDE);
  makeCard(tile, 76, PROV_CODEX);
  makeCard(tile, 148, PROV_ANTIGRAVITY);
  cardsBuilt = true;
  addGear(tile);

  cardsTimer = lv_timer_create(refreshCardsCb, 1000, nullptr);
  for (int i = 0; i < PROV_COUNT; i++) updateCard((Provider)i);
}

// Paginas sob demanda: o ui.cpp apaga os filhos da tile (lv_obj_clean) depois disto.
void usage_ui_unbuild_page() {
  if (cardsTimer) lv_timer_del(cardsTimer);
  cardsTimer = nullptr;
  cardsBuilt = false;
  memset(cards, 0, sizeof(cards));
}

// ---- uma pagina por IA --------------------------------------------------------

struct ProvPage {
  bool built;
  lv_obj_t *badge, *arc, *pct, *winName;
  lv_obj_t *vals, *c1Title, *c1Value, *c1Left, *card2, *c2Name, *c2Pct, *c2Bar, *c2Sub;
  lv_obj_t *empty, *emptyTitle;
};
static ProvPage provPages[PROV_COUNT];
static const PageId PAGE_OF[PROV_COUNT] = {PG_CLAUDE, PG_CODEX, PG_AGY};
static bool provBuilt(int i) { return provPages[i].built; }

// Idem, para a pagina de uma IA; o timer compartilhado morre com a ultima pagina montada.
void usage_ui_unbuild_provider_page(Provider p) {
  memset(&provPages[p], 0, sizeof(provPages[p]));
  if (!provPages[0].built && !provPages[1].built && !provPages[2].built && provTimer) {
    lv_timer_del(provTimer);
    provTimer = nullptr;
  }
}

static void updateProvPage(Provider p) {
  ProvPage &pg = provPages[p];
  ProviderUsage u = usage_get(p);
  char buf[48];
  uint32_t color = badgeText(u, buf, sizeof(buf));
  if (usage_is_refreshing(p) && usage_has_account(p)) {
    snprintf(buf, sizeof(buf), "atualizando...");
    color = th::MUTED;
  }
  setText(pg.badge, buf);
  lv_obj_set_style_text_color(pg.badge, thc(color), 0);

  bool values = u.source == UsageSource::Direct || u.source == UsageSource::Cache;
  show(pg.vals, values);
  show(pg.empty, !values);
  if (!values) {
    setText(pg.emptyTitle, u.source == UsageSource::NoAccount ? "Sem conta ligada"
                           : u.source == UsageSource::Failed  ? "Leitura falhou"
                                                              : "Sem dados ainda");
  }

  WinText w0 = windowText(u, p, 0);
  setText(pg.pct, w0.pct_txt);
  setText(pg.winName, w0.present ? w0.name : "");
  lv_arc_set_value(pg.arc, (int)w0.pct);
  lv_obj_set_style_arc_color(pg.arc, thc(barColor(p, w0.pct)), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(pg.arc, u.source == UsageSource::Cache ? LV_OPA_60 : LV_OPA_COVER, LV_PART_INDICATOR);
  if (!values) return;

  snprintf(buf, sizeof(buf), "%s reinicia", w0.name);
  setText(pg.c1Title, buf);
  setText(pg.c1Value, w0.reset[0] ? w0.reset : "--");
  setText(pg.c1Left, w0.left);

  WinText w1 = windowText(u, p, 1);
  show(pg.card2, w1.present);
  if (w1.present) {
    setText(pg.c2Name, w1.name);
    setText(pg.c2Pct, w1.pct_txt);
    lv_bar_set_value(pg.c2Bar, (int)w1.pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(pg.c2Bar, thc(barColor(p, w1.pct)), LV_PART_INDICATOR);
    if (w1.reset[0]) snprintf(buf, sizeof(buf), "reinicia %s", w1.reset);
    else buf[0] = 0;
    setText(pg.c2Sub, buf);
  }
}

static void refreshProvCb(lv_timer_t *) {
  if (ui_is_animating()) return;
  for (int i = 0; i < PROV_COUNT; i++)
    if (provPages[i].built && ui_page_shown(PAGE_OF[i])) updateProvPage((Provider)i);
}

void usage_ui_build_provider_page(lv_obj_t *tile, Provider p) {
  ProvPage &pg = provPages[p];
  void *user = (void *)(intptr_t)p;

  lv_obj_t *dot = ui_dot(tile, 10, provColor(p));
  lv_obj_set_pos(dot, 12, 16);
  lv_obj_t *t = ui_title(tile, NAMES[p]);
  lv_obj_set_x(t, 28);
  pg.badge = ui_muted(tile, "");
  lv_obj_align(pg.badge, LV_ALIGN_TOP_RIGHT, -12, 14);

  pg.arc = lv_arc_create(tile);
  lv_obj_set_size(pg.arc, 116, 116);
  lv_obj_set_pos(pg.arc, 10, 48);
  lv_arc_set_rotation(pg.arc, 270);
  lv_arc_set_bg_angles(pg.arc, 0, 360);
  lv_arc_set_range(pg.arc, 0, 100);
  lv_obj_remove_style(pg.arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(pg.arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(pg.arc, 0, 0);
  lv_obj_set_style_arc_width(pg.arc, 11, 0);
  lv_obj_set_style_arc_color(pg.arc, thc(th::TRACK), 0);
  lv_obj_set_style_arc_width(pg.arc, 11, LV_PART_INDICATOR);
  pg.pct = lv_label_create(pg.arc);
  lv_obj_set_style_text_font(pg.pct, &lv_font_montserrat_28, 0);
  lv_obj_align(pg.pct, LV_ALIGN_CENTER, 0, -6);
  lv_label_set_text(pg.pct, "--");
  pg.winName = ui_muted(pg.arc, "");
  lv_obj_align(pg.winName, LV_ALIGN_CENTER, 0, 18);

  // Coluna da direita com valores; toque abre a conta.
  pg.vals = lv_obj_create(tile);
  lv_obj_remove_style_all(pg.vals);
  lv_obj_set_size(pg.vals, 178, 132);
  lv_obj_set_pos(pg.vals, 134, 40);
  lv_obj_add_flag(pg.vals, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(pg.vals, accountCb, LV_EVENT_CLICKED, user);

  lv_obj_t *c1 = ui_card(pg.vals, 0, 0, 178, 64);
  lv_obj_clear_flag(c1, LV_OBJ_FLAG_CLICKABLE);
  pg.c1Title = ui_muted(c1, "");
  lv_obj_set_pos(pg.c1Title, 12, 6);
  pg.c1Value = lv_label_create(c1);
  lv_obj_set_style_text_font(pg.c1Value, &lv_font_montserrat_20, 0);
  lv_obj_set_pos(pg.c1Value, 12, 21);
  lv_label_set_text(pg.c1Value, "");
  pg.c1Left = ui_muted(c1, "");
  lv_obj_set_pos(pg.c1Left, 12, 46);  // "em 3h 05min" numa linha propria: ao lado colidia com "dom 00:50"

  pg.card2 = ui_card(pg.vals, 0, 70, 178, 62);
  lv_obj_clear_flag(pg.card2, LV_OBJ_FLAG_CLICKABLE);
  pg.c2Name = ui_muted(pg.card2, "");
  lv_obj_set_pos(pg.c2Name, 12, 6);
  pg.c2Pct = lv_label_create(pg.card2);
  lv_obj_set_style_text_font(pg.c2Pct, &lv_font_montserrat_16, 0);
  lv_obj_align(pg.c2Pct, LV_ALIGN_TOP_RIGHT, -12, 5);
  lv_label_set_text(pg.c2Pct, "");
  pg.c2Bar = ui_bar(pg.card2, 154, 6, provColor(p));
  lv_obj_set_pos(pg.c2Bar, 12, 28);
  pg.c2Sub = ui_muted(pg.card2, "");
  lv_obj_set_pos(pg.c2Sub, 12, 41);

  // Sem leitura: explica e oferece configurar.
  pg.empty = lv_obj_create(tile);
  lv_obj_remove_style_all(pg.empty);
  lv_obj_set_size(pg.empty, 178, 132);
  lv_obj_set_pos(pg.empty, 134, 46);
  pg.emptyTitle = lv_label_create(pg.empty);
  lv_obj_set_style_text_font(pg.emptyTitle, &lv_font_montserrat_16, 0);
  lv_label_set_text(pg.emptyTitle, "");
  lv_obj_t *msg = ui_muted(pg.empty, "Configure pelo celular ou\nnavegador para ver o uso.");
  lv_obj_set_pos(msg, 0, 26);
  ui_tonal_btn(pg.empty, 0, 70, 0, LV_SYMBOL_PLUS " Ligar conta", accountCb, user);

  if (!provTimer) provTimer = lv_timer_create(refreshProvCb, 1000, nullptr);  // um timer so para as tres paginas
  addGear(tile);
  pg.built = true;
  updateProvPage(p);
}

// ---- tela da conta -----------------------------------------------------------
// So existe enquanto aberta (o QR pede ~8 KB do pool do LVGL ao ser gerado).

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
  lv_obj_del_async(acct);  // async: chamado de um botao filho dele
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

  // Token salvo pela pagina e a primeira consulta ja respondeu: sai do QR e mostra o resultado.
  AccountPortalInfo info = account_portal_info();
  if (acctSub && !subIsLogin && info.saved && info.saved_provider == acctProv && u.attempts > info.saved_attempts)
    closeSub();

  // Login do Codex: acompanha o codigo e volta para o status ao terminar.
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
      closeSub();  // Done ou Failed: o status abaixo mostra o resultado
    }
  }

  char buf[220];
  int n = snprintf(buf, sizeof(buf), "%s\n", usage_has_account(acctProv) ? "Conta configurada" : "Sem conta configurada");
  if (u.updated_epoch) {
    char t[16], age[16];
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

  // Sem conta: so configurar. Com conta: atualizar, trocar a conta e remover.
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
  // Dois toques em 3 s: um toque so nao apaga a conta.
  if (!forgetArmedAt) {
    forgetArmedAt = board_millis();
    lv_label_set_text(btnForgetLbl, "Toque de novo");
    return;
  }
  forgetArmedAt = 0;
  usage_forget_account(acctProv);
  lv_label_set_text(btnForgetLbl, LV_SYMBOL_TRASH " Remover");
}

// Painel secundario com QR + texto + (opcional) codigo grande, no lugar do status.
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
  lv_obj_set_style_border_width(qr, 6, 0);  // zona de silencio
  lv_obj_set_pos(qr, 10, 8);
  lv_qrcode_update(qr, qrText, strlen(qrText));

  subInfo = lv_label_create(acctSub);
  lv_obj_set_width(subInfo, 180);
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
