#include "diag_ui.h"

#include <lvgl.h>
#include <stdio.h>

#include "net_diag.h"
#include "theme.h"
#include "settings_ui.h"

static constexpr int SCR_W = 320;
static constexpr int SCR_H = 240;
static constexpr int MAX_ROWS = 6;
static constexpr int ROW_H = 30;

static lv_obj_t *overlay, *lblFooter, *btnAgain;
static lv_obj_t *rowName[MAX_ROWS], *rowStatus[MAX_ROWS];
static lv_timer_t *timer;

static void closeCb(lv_event_t *) {
  lv_timer_del(timer);
  timer = nullptr;
  lv_obj_del_async(overlay);  // async: o botao clicado e filho dele
  overlay = nullptr;
  settings_ui_child_closed();
}

static void againCb(lv_event_t *) { net_diag_start(); }

static void refresh(lv_timer_t *) {
  for (int i = 0; i < net_diag_count() && i < MAX_ROWS; i++) {
    NetDiagResult r;
    if (!net_diag_get(i, &r)) continue;
    lv_label_set_text(rowName[i], r.label);
    uint32_t color = th::MUTED;
    switch (r.state) {
      case DiagState::Pending:
        lv_label_set_text(rowStatus[i], "aguardando");
        break;
      case DiagState::Running:
        lv_label_set_text(rowStatus[i], "testando...");
        color = th::AMBER;
        break;
      case DiagState::Ok:
        lv_label_set_text_fmt(rowStatus[i], "OK  HTTP %d  %u ms  %u KB", r.http_status,
                              (unsigned)r.handshake_ms, (unsigned)(r.tls_heap_bytes / 1024));
        color = th::GREEN;
        break;
      case DiagState::Blocked:
        lv_label_set_text_fmt(rowStatus[i], "BLOQUEADO: %s", r.detail);
        color = th::RED;
        break;
      case DiagState::Failed:
        lv_label_set_text_fmt(rowStatus[i], "FALHOU: %s", r.detail);
        color = th::RED;
        break;
    }
    lv_obj_set_style_text_color(rowStatus[i], thc(color), 0);
  }

  bool busy = net_diag_running();
  if (busy) {
    lv_label_set_text(lblFooter, "Testando um servico por vez...");
    lv_obj_add_state(btnAgain, LV_STATE_DISABLED);
  } else {
    NetDiagSummary s = net_diag_summary();
    lv_label_set_text_fmt(lblFooter, "TLS ate %u KB | heap min %u KB | bloco %u KB",
                          (unsigned)(s.tls_heap_max / 1024), (unsigned)(s.heap_min_free / 1024),
                          (unsigned)(s.largest_block / 1024));
    lv_obj_clear_state(btnAgain, LV_STATE_DISABLED);
  }
}

void diag_ui_open() {
  if (overlay) return;
  overlay = ui_overlay();
  ui_back_header(overlay, "Rede", closeCb);
  btnAgain = lv_obj_get_parent(ui_tonal_btn(overlay, 214, 6, 96, LV_SYMBOL_REFRESH " Testar", againCb, nullptr, 30));

  for (int i = 0; i < MAX_ROWS; i++) {
    int y = 46 + i * ROW_H;
    rowName[i] = lv_label_create(overlay);
    lv_obj_set_style_text_color(rowName[i], thc(th::TEXT), 0);
    lv_label_set_text(rowName[i], "");
    lv_obj_set_pos(rowName[i], 10, y);
    rowStatus[i] = lv_label_create(overlay);
    lv_label_set_text(rowStatus[i], "");
    lv_label_set_long_mode(rowStatus[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(rowStatus[i], 196);
    lv_obj_set_style_text_font(rowStatus[i], &lv_font_montserrat_12, 0);
    lv_obj_set_pos(rowStatus[i], 116, y + 2);
  }

  lblFooter = lv_label_create(overlay);
  lv_obj_set_style_text_font(lblFooter, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblFooter, thc(th::MUTED), 0);
  lv_obj_set_pos(lblFooter, 10, SCR_H - 24);
  lv_label_set_text(lblFooter, "");

  net_diag_start();
  timer = lv_timer_create(refresh, 300, nullptr);
  refresh(nullptr);
}
