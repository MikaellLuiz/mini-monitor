#include "wifi_ui.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "settings_ui.h"
#include "theme.h"
#include "ui.h"
#include "wifi_mgr.h"

static constexpr int SCR_W = 320;
static constexpr int SCR_H = 240;

// As telas cheias (Wi-Fi, portal, redes salvas) so existem enquanto abertas: o pool
// do LVGL na placa e pequeno e o QR pede ~8 KB temporarios ao ser gerado.
static lv_obj_t *wifiScreen, *wsCard, *wsIcon, *wsIconLbl, *lblLine1, *lblLine2, *lblSavedCount;
static lv_obj_t *portalOverlay, *qrJoin, *qrPage, *lblPortalCreds;
static lv_obj_t *savedOverlay, *savedList, *sheetDim, *lblSheetTitle, *lblSheetSub;
static int sheetIdx = -1;

// ---- helpers -----------------------------------------------------------------

static const char *signalWord(int rssi) {
  if (rssi >= -60) return "otimo";
  if (rssi >= -70) return "bom";
  if (rssi >= -80) return "fraco";
  return "ruim";
}

void wifi_ui_summary(char *buf, int size) {
  WifiStatus s = wifi_status();
  switch (s.state) {
    case WifiState::Connected:
      snprintf(buf, size, "%s - conectado", s.ssid);
      break;
    case WifiState::Connecting:
      snprintf(buf, size, "Conectando a %s...", s.ssid);
      break;
    case WifiState::Portal:
      snprintf(buf, size, "Portal aberto (%s)", s.ap_ssid);
      break;
    case WifiState::Offline:
      if (s.saved_count == 0) snprintf(buf, size, "Nenhuma rede salva");
      else snprintf(buf, size, "Offline, tenta em %u s", (unsigned)s.retry_in_s);
      break;
    default:
      snprintf(buf, size, "Procurando...");
  }
}

// ---- tela Wi-Fi --------------------------------------------------------------

static void updateWifiScreen(const WifiStatus &s) {
  bool connected = s.state == WifiState::Connected;
  switch (s.state) {
    case WifiState::Off:
      lv_label_set_text(lblLine1, "Iniciando...");
      lv_label_set_text(lblLine2, "");
      break;
    case WifiState::Scanning:
      lv_label_set_text(lblLine1, "Procurando redes...");
      lv_label_set_text(lblLine2, "");
      break;
    case WifiState::Connecting:
      lv_label_set_text_fmt(lblLine1, "%s", s.ssid);
      lv_label_set_text(lblLine2, "Conectando...");
      break;
    case WifiState::Connected:
      lv_label_set_text_fmt(lblLine1, "%s", s.ssid);
      lv_label_set_text_fmt(lblLine2, "Conectado - sinal %s - %s", signalWord(s.rssi), s.ip);
      break;
    case WifiState::Offline:
      if (s.saved_count == 0) {
        lv_label_set_text(lblLine1, "Nenhuma rede salva");
        lv_label_set_text(lblLine2, "Adicione uma pelo celular");
      } else {
        lv_label_set_text(lblLine1, "Offline");
        lv_label_set_text_fmt(lblLine2, "Nova tentativa em %u s", (unsigned)s.retry_in_s);
      }
      break;
    case WifiState::Portal:
      lv_label_set_text(lblLine1, "Modo de configuracao");
      lv_label_set_text_fmt(lblLine2, "Rede %s", s.ap_ssid);
      break;
  }
  lv_obj_set_style_bg_color(wsCard, thc(connected ? th::CARD_HI : th::CARD), 0);
  lv_obj_set_style_bg_color(wsIcon, thc(connected ? th::PRIMARY : th::TONAL), 0);
  lv_obj_set_style_text_color(wsIconLbl, thc(connected ? th::ON_PRIMARY : th::ON_TONAL), 0);
  lv_obj_set_style_text_color(lblLine2, thc(connected ? th::ON_MINI : th::MUTED), 0);
  lv_label_set_text_fmt(lblSavedCount, "%u  " LV_SYMBOL_RIGHT, (unsigned)s.saved_count);
}

static void wifiBackCb(lv_event_t *) {
  lv_obj_del_async(wifiScreen);  // async: o botao clicado e filho dela
  wifiScreen = nullptr;
  settings_ui_child_closed();
}
static void addCb(lv_event_t *) { wifi_portal_start(); }
static void reconnectCb(lv_event_t *) { wifi_connect_now(); }

static void buildSavedOverlay();
static void savedOpenCb(lv_event_t *) {
  if (!savedOverlay && ui_mem_ok(8 * 1024)) buildSavedOverlay();
}

void wifi_ui_open() {
  if (wifiScreen || !ui_mem_ok(6 * 1024)) return;
  wifiScreen = ui_overlay();
  ui_back_header(wifiScreen, "Wi-Fi", wifiBackCb);

  wsCard = ui_card(wifiScreen, 10, 48, 300, 56);
  wsIcon = ui_dot(wsCard, 32, th::TONAL);
  lv_obj_align(wsIcon, LV_ALIGN_LEFT_MID, 12, 0);
  wsIconLbl = lv_label_create(wsIcon);
  lv_label_set_text(wsIconLbl, LV_SYMBOL_WIFI);
  lv_obj_center(wsIconLbl);
  lblLine1 = lv_label_create(wsCard);
  lv_label_set_long_mode(lblLine1, LV_LABEL_LONG_DOT);
  lv_obj_set_size(lblLine1, 240, 20);
  lv_obj_set_style_text_font(lblLine1, &lv_font_montserrat_16, 0);
  lv_obj_set_pos(lblLine1, 54, 9);
  lv_label_set_text(lblLine1, "");
  lblLine2 = lv_label_create(wsCard);
  lv_label_set_long_mode(lblLine2, LV_LABEL_LONG_DOT);
  lv_obj_set_size(lblLine2, 240, 15);
  lv_obj_set_style_text_font(lblLine2, &lv_font_montserrat_12, 0);
  lv_obj_set_pos(lblLine2, 54, 31);
  lv_label_set_text(lblLine2, "");

  lv_obj_t *list = ui_list(wifiScreen, 10, 112, 300);
  ui_list_row(list, LV_SYMBOL_PLUS, "Adicionar rede pelo celular", nullptr, addCb);
  lblSavedCount = ui_list_row(list, LV_SYMBOL_LIST, "Redes salvas", nullptr, savedOpenCb).trail;

  ui_mini_btn(wifiScreen, 10, 204, LV_SYMBOL_REFRESH " Reconectar", reconnectCb);

  updateWifiScreen(wifi_status());
}

// ---- portal ------------------------------------------------------------------

static void portalCancelCb(lv_event_t *) {
  wifi_portal_stop();
  lv_obj_del_async(portalOverlay);  // async: o botao clicado e filho dele
  portalOverlay = nullptr;
}

static constexpr int QR_SIZE = 108;
static constexpr int QR_QUIET = 6;  // borda branca = zona de silencio; sem ela muitos celulares nao leem

// x = borda esquerda do bloco (QR + zona de silencio).
static lv_obj_t *makeQr(lv_obj_t *parent, int x, const char *caption) {
  lv_obj_t *qr = lv_qrcode_create(parent, QR_SIZE, lv_color_black(), lv_color_white());
  lv_obj_set_pos(qr, x, 42);
  lv_obj_set_style_border_color(qr, lv_color_white(), 0);
  lv_obj_set_style_border_width(qr, QR_QUIET, 0);
  int block = QR_SIZE + 2 * QR_QUIET;
  lv_obj_t *l = ui_muted(parent, caption);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(l, block + 20);
  lv_obj_set_pos(l, x - 10, 42 + block + 4);
  return qr;
}

static void buildPortalOverlay(const WifiStatus &s) {
  portalOverlay = ui_overlay();
  ui_title(portalOverlay, "Adicionar Wi-Fi");
  // Dois blocos de 120 px com 30 px entre eles, centralizados em 320.
  qrJoin = makeQr(portalOverlay, 25, "1. Escaneie para\nentrar na rede");
  qrPage = makeQr(portalOverlay, 175, "2. Escaneie para\nabrir a pagina");

  lblPortalCreds = lv_label_create(portalOverlay);
  lv_obj_set_style_text_font(lblPortalCreds, &lv_font_montserrat_12, 0);
  lv_obj_set_width(lblPortalCreds, SCR_W);
  lv_obj_set_style_text_align(lblPortalCreds, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(lblPortalCreds, 0, 208);
  lv_label_set_text_fmt(lblPortalCreds, "Rede %s  |  Senha %s", s.ap_ssid, s.ap_pass);

  ui_tonal_btn(portalOverlay, 216, 6, 94, "Cancelar", portalCancelCb, nullptr, 30);

  // Formato que a camera do Android/iOS entende para entrar na rede direto.
  char join[80];
  snprintf(join, sizeof(join), "WIFI:T:WPA;S:%s;P:%s;;", s.ap_ssid, s.ap_pass);
  lv_qrcode_update(qrJoin, join, strlen(join));
  static const char *url = "http://192.168.4.1";
  lv_qrcode_update(qrPage, url, strlen(url));
}

// Chamado pelo timer (fora de evento de filho), entao pode apagar direto.
static void updatePortal(const WifiStatus &s) {
  if (s.state != WifiState::Portal) {
    if (portalOverlay) {
      lv_obj_del(portalOverlay);
      portalOverlay = nullptr;
    }
    return;
  }
  if (!portalOverlay) buildPortalOverlay(s);
}

// ---- redes salvas ------------------------------------------------------------

static void closeSheet() {
  if (sheetDim) lv_obj_add_flag(sheetDim, LV_OBJ_FLAG_HIDDEN);
  sheetIdx = -1;
}

// Chamado de dentro de eventos dos filhos: apaga de forma assincrona.
static void closeSaved() {
  lv_obj_del_async(savedOverlay);
  savedOverlay = savedList = sheetDim = lblSheetTitle = lblSheetSub = nullptr;
  sheetIdx = -1;
}

static void rebuildSavedList();

static void sheetActionCb(lv_event_t *e) {
  int action = (int)(intptr_t)lv_event_get_user_data(e);
  int idx = sheetIdx;
  closeSheet();
  if (action == 0) {
    wifi_connect_saved(idx);
    closeSaved();
    return;
  }
  if (action == 1) wifi_move_up(idx);
  if (action == 2) wifi_forget(idx);
  rebuildSavedList();
}

static void rowCb(lv_event_t *e) {
  sheetIdx = (int)(intptr_t)lv_event_get_user_data(e);
  char name[33];
  if (!wifi_saved_name(sheetIdx, name, sizeof(name))) return;
  lv_label_set_text(lblSheetTitle, name);
  lv_label_set_text_fmt(lblSheetSub, "prioridade %d", sheetIdx + 1);
  lv_obj_clear_flag(sheetDim, LV_OBJ_FLAG_HIDDEN);
}

static void savedBackCb(lv_event_t *) { closeSaved(); }

static void rebuildSavedList() {
  lv_obj_clean(savedList);
  WifiStatus s = wifi_status();
  char name[33], num[4];
  int n = 0;
  for (int i = 0; i < WIFI_MAX_SAVED && wifi_saved_name(i, name, sizeof(name)); i++, n++) {
    bool current = s.state == WifiState::Connected && !strcmp(name, s.ssid);
    snprintf(num, sizeof(num), "%d", i + 1);
    UiRow r = ui_list_row(savedList, num, name, current ? "Conectada" : "Salva", rowCb, (void *)(intptr_t)i);
    if (current) lv_obj_set_style_text_color(r.sub, thc(th::GREEN), 0);
    lv_label_set_text(r.trail, LV_SYMBOL_LIST);
  }
  if (n == 0) ui_list_row(savedList, nullptr, "Nenhuma rede salva ainda", nullptr, nullptr);
}

static lv_obj_t *makeSheetAction(lv_obj_t *sheet, int y, const char *text, int action, uint32_t color) {
  lv_obj_t *b = lv_obj_create(sheet);
  lv_obj_remove_style_all(b);
  lv_obj_set_size(b, SCR_W, 34);
  lv_obj_set_pos(b, 0, y);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(b, thc(th::MINI_PR), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_add_event_cb(b, sheetActionCb, LV_EVENT_CLICKED, (void *)(intptr_t)action);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, thc(color), 0);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 20, 0);
  return b;
}

static void buildSavedOverlay() {
  savedOverlay = ui_overlay();
  ui_back_header(savedOverlay, "Redes salvas", savedBackCb);

  lv_obj_t *hint = ui_muted(savedOverlay, "Ordem = prioridade: o painel tenta a 1 primeiro.");
  lv_obj_set_pos(hint, 12, 46);

  // Lista rolavel dentro de um cartao.
  lv_obj_t *scroll = lv_obj_create(savedOverlay);
  lv_obj_remove_style_all(scroll);
  lv_obj_set_size(scroll, 300, SCR_H - 70);
  lv_obj_set_pos(scroll, 10, 66);
  lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);
  savedList = ui_list(scroll, 0, 0, 300);

  // Folha de acoes que sobe de baixo, sobre um fundo escurecido.
  sheetDim = lv_obj_create(savedOverlay);
  lv_obj_remove_style_all(sheetDim);
  lv_obj_set_size(sheetDim, SCR_W, SCR_H);
  lv_obj_set_style_bg_color(sheetDim, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(sheetDim, LV_OPA_60, 0);
  lv_obj_add_flag(sheetDim, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(sheetDim, [](lv_event_t *e) {
    if (lv_event_get_target(e) == sheetDim) closeSheet();  // toque fora da folha
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(sheetDim, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *sheet = ui_card(sheetDim, 0, SCR_H - 152, SCR_W, 168, th::SHEET);
  lv_obj_set_style_radius(sheet, 16, 0);
  lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);  // toque na folha nao fecha
  lv_obj_t *pill = ui_dot(sheet, 0, th::HANDLE);
  lv_obj_set_size(pill, 36, 4);
  lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, 6);

  lblSheetTitle = lv_label_create(sheet);
  lv_label_set_long_mode(lblSheetTitle, LV_LABEL_LONG_DOT);
  lv_obj_set_size(lblSheetTitle, 170, 20);
  lv_obj_set_style_text_font(lblSheetTitle, &lv_font_montserrat_16, 0);
  lv_obj_set_pos(lblSheetTitle, 20, 16);
  lblSheetSub = ui_muted(sheet, "");
  lv_obj_set_pos(lblSheetSub, 196, 19);

  makeSheetAction(sheet, 42, LV_SYMBOL_WIFI "   Conectar agora", 0, th::TEXT);
  makeSheetAction(sheet, 76, LV_SYMBOL_UP "   Subir prioridade", 1, th::TEXT);
  makeSheetAction(sheet, 110, LV_SYMBOL_TRASH "   Esquecer rede", 2, th::ON_DANGER);

  rebuildSavedList();
}

// ---- atualizacao periodica ---------------------------------------------------

static void refreshCb(lv_timer_t *) {
  if (ui_is_animating()) return;  // nao competir com os quadros da transicao
  WifiStatus s = wifi_status();
  if (wifiScreen) updateWifiScreen(s);
  updatePortal(s);
}

void wifi_ui_init_overlays() {
  lv_timer_create(refreshCb, 500, nullptr);
  refreshCb(nullptr);
}
