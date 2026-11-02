#include "spotify_ui.h"

#include <stdio.h>
#include <string.h>

#include "account_portal.h"
#include "board.h"
#include "spotify.h"
#include "theme.h"
#include "settings_ui.h"
#include "ui.h"

static constexpr int SCR_W = 320;
static constexpr int SCR_H = 240;
static constexpr int COVER_SHOWN = 96;  // a capa vem em 64 px e e ampliada 1,5x

static lv_obj_t *cover, *coverBox, *lblTitle, *lblArtist, *lblInfo, *bar, *lblPos, *lblDur;
static lv_obj_t *btnPlay, *btnPlayLbl, *content, *setup;
static lv_img_dsc_t coverDsc;
static uint16_t shownSeq = 0xFFFF;
static char shownTitle[64], shownArtist[48], shownInfo[96];
static int shownPos = -1, shownBar = -1;
static uint32_t refreshingUntil;  // "atualizando" apos tocar na faixa
static bool shownPlaying = false, shownLinked = true;

static void setText(lv_obj_t *l, char *cache, size_t cap, const char *text) {
  if (strncmp(cache, text, cap)) {
    snprintf(cache, cap, "%s", text);
    lv_label_set_text(l, text);
  }
}

static void formatTime(uint32_t ms, char *buf, int size) {
  uint32_t s = ms / 1000;
  snprintf(buf, size, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

// A pagina so consulta o player depois de ficar parada na tela por DWELL_MS: passar por ela
// deslizando entre paginas nao dispara consulta (cada uma abre uma conexao TLS).
static constexpr uint32_t DWELL_MS = 3000;
// Leitura mais velha que isso nao vale como "tocando": o player so e consultado com a
// pagina parada, e o que ficou da ultima visita pode nem ser verdade mais.
static constexpr uint32_t STALE_MS = 10000;
static bool dwelling = false;
static uint32_t dwellStart = 0;

static void refreshCb(lv_timer_t *) {
  bool here = ui_page_shown(PG_SPOTIFY) && !ui_is_animating();
  if (!here) dwelling = false;
  else if (!dwelling) {
    dwelling = true;
    dwellStart = board_millis();
  }
  spotify_set_visible(here && board_millis() - dwellStart >= DWELL_MS);
  if (ui_is_animating()) return;

  SpotifyState s = spotify_get();
  if (s.linked != shownLinked) {
    shownLinked = s.linked;
    if (s.linked) {
      lv_obj_clear_flag(content, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(setup, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(content, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(setup, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (!s.linked) return;

  if (s.cover_seq != shownSeq) {
    shownSeq = s.cover_seq;
    const uint16_t *px = spotify_cover();
    if (px) {
      coverDsc.header.always_zero = 0;
      coverDsc.header.w = SPOTIFY_COVER_PX;
      coverDsc.header.h = SPOTIFY_COVER_PX;
      coverDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
      coverDsc.data_size = SPOTIFY_COVER_PX * SPOTIFY_COVER_PX * 2;
      coverDsc.data = (const uint8_t *)px;
      lv_img_set_src(cover, &coverDsc);
      lv_obj_clear_flag(cover, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (!s.active) lv_obj_add_flag(cover, LV_OBJ_FLAG_HIDDEN);
  else if (spotify_cover()) lv_obj_clear_flag(cover, LV_OBJ_FLAG_HIDDEN);

  setText(lblTitle, shownTitle, sizeof(shownTitle), s.active ? s.title : "Nada tocando");
  setText(lblArtist, shownArtist, sizeof(shownArtist), s.active ? s.artist : "Abra o Spotify em um aparelho");

  // Sem leitura recente nao ha como saber se ainda toca: nao mostra "tocando" nem avanca a barra.
  bool fresh = s.busy || board_millis() - s.fetched_at_ms < STALE_MS;
  bool playing = s.playing && s.active && fresh;

  char info[96];
  if (s.error[0]) snprintf(info, sizeof(info), "%s", s.error);
  else if (s.active && !fresh) snprintf(info, sizeof(info), "ATUALIZANDO...");
  else if ((int32_t)(board_millis() - refreshingUntil) < 0) snprintf(info, sizeof(info), "ATUALIZANDO...");
  else if (s.active) snprintf(info, sizeof(info), LV_SYMBOL_WIFI "  %s EM %s", s.playing ? "TOCANDO" : "PAUSADO", s.device);
  else snprintf(info, sizeof(info), "SPOTIFY");
  setText(lblInfo, shownInfo, sizeof(shownInfo), info);
  lv_obj_set_style_text_color(lblInfo, thc(s.error[0] ? th::RED : (s.active && !fresh) ? th::SP_SUB : th::SP_GREEN), 0);

  if (playing != shownPlaying) {
    shownPlaying = playing;
    lv_label_set_text(btnPlayLbl, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  }
  lv_obj_set_style_opa(btnPlay, s.busy ? LV_OPA_60 : LV_OPA_COVER, 0);

  // Progresso interpolado entre as leituras (a API so e consultada a cada ~4 s).
  uint32_t pos = s.progress_ms;
  if (playing) pos += board_millis() - s.fetched_at_ms;
  if (s.duration_ms && pos > s.duration_ms) pos = s.duration_ms;
  int barVal = s.duration_ms ? (int)((uint64_t)pos * 1000 / s.duration_ms) : 0;
  if (barVal != shownBar) {
    shownBar = barVal;
    lv_bar_set_value(bar, barVal, LV_ANIM_OFF);
  }
  int posSec = pos / 1000;
  if (posSec != shownPos) {
    shownPos = posSec;
    char a[12], b[12];
    formatTime(pos, a, sizeof(a));
    formatTime(s.duration_ms, b, sizeof(b));
    lv_label_set_text(lblPos, s.active ? a : "");
    lv_label_set_text(lblDur, s.active ? b : "");
  }
}

static void cmdCb(lv_event_t *e) { spotify_command((SpotifyCmd)(intptr_t)lv_event_get_user_data(e)); }

// Anterior/proxima: so o icone, sem fundo (estilo do player do carro).
static lv_obj_t *makeCtl(lv_obj_t *parent, int x, int y, int size, const char *symbol, SpotifyCmd cmd, bool round) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_size(b, size, size);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, thc(round ? th::SP_TEXT : th::SP_BG), 0);
  lv_obj_set_style_bg_color(b, thc(round ? th::SP_SUB : th::SP_PR), LV_STATE_PRESSED);
  lv_obj_set_ext_click_area(b, 6);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, symbol);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(l, thc(round ? th::SP_BG : th::SP_TEXT), 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cmdCb, LV_EVENT_CLICKED, (void *)(intptr_t)cmd);
  return l;
}

static lv_timer_t *pageTimer;

// Paginas sob demanda: o ui.cpp apaga os filhos da tile (lv_obj_clean) depois disto.
void spotify_ui_unbuild_page() {
  if (pageTimer) lv_timer_del(pageTimer);
  pageTimer = nullptr;
  spotify_set_visible(false);  // sem timer ninguem mais desliga a consulta do player
  dwelling = false;
  cover = coverBox = lblTitle = lblArtist = lblInfo = bar = lblPos = lblDur = nullptr;
  btnPlay = btnPlayLbl = content = setup = nullptr;
  shownSeq = 0xFFFF;
  shownTitle[0] = shownArtist[0] = shownInfo[0] = 0;
  shownPos = shownBar = -1;
  refreshingUntil = 0;
  shownPlaying = false;
  shownLinked = true;
}

static void openAccount();
void spotify_ui_open_account() { openAccount(); }
static void setupCb(lv_event_t *) { openAccount(); }

// Tocar na faixa consulta o player agora (so existe com a conta ligada).
static void trackTapCb(lv_event_t *) {
  spotify_refresh_now();
  refreshingUntil = board_millis() + 1500;
  dwellStart = board_millis() - DWELL_MS;  // o toque dispensa a espera de pagina parada
  refreshCb(nullptr);
}

void spotify_ui_build_page(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, thc(th::SP_BG), 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

  // Conteudo do player (escondido enquanto nao ha conta).
  content = lv_obj_create(tile);
  lv_obj_remove_style_all(content);
  lv_obj_set_size(content, SCR_W, SCR_H);
  lv_obj_set_pos(content, 0, 0);
  lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);

  lblInfo = lv_label_create(content);
  lv_label_set_long_mode(lblInfo, LV_LABEL_LONG_DOT);
  lv_obj_set_size(lblInfo, 250, 16);
  lv_obj_set_style_text_font(lblInfo, &lv_font_montserrat_12, 0);
  lv_obj_set_pos(lblInfo, 12, 14);
  lv_label_set_text(lblInfo, "");

  coverBox = lv_obj_create(content);
  lv_obj_remove_style_all(coverBox);
  lv_obj_set_size(coverBox, COVER_SHOWN, COVER_SHOWN);
  lv_obj_set_pos(coverBox, 12, 40);
  lv_obj_clear_flag(coverBox, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(coverBox, thc(th::SP_COVER), 0);
  lv_obj_set_style_bg_opa(coverBox, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(coverBox, 6, 0);
  lv_obj_t *note = lv_label_create(coverBox);
  lv_label_set_text(note, LV_SYMBOL_AUDIO);
  lv_obj_set_style_text_font(note, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(note, thc(th::SP_SUB), 0);
  lv_obj_center(note);
  // Ampliada pelo centro (o pivo volta ao centro a cada lv_img_set_src): o objeto
  // de 64 px fica no meio da caixa e a imagem cresce 16 px para cada lado.
  cover = lv_img_create(content);
  lv_obj_set_pos(cover, 12 + (COVER_SHOWN - SPOTIFY_COVER_PX) / 2, 40 + (COVER_SHOWN - SPOTIFY_COVER_PX) / 2);
  lv_img_set_zoom(cover, 256 * COVER_SHOWN / SPOTIFY_COVER_PX);
  lv_img_set_antialias(cover, false);
  lv_obj_add_flag(cover, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *hit = lv_obj_create(content);  // area de toque sobre capa e titulo
  lv_obj_remove_style_all(hit);
  lv_obj_set_size(hit, 300, 100);
  lv_obj_set_pos(hit, 10, 36);
  lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(hit, trackTapCb, LV_EVENT_CLICKED, nullptr);

  lblTitle = lv_label_create(content);
  lv_label_set_long_mode(lblTitle, LV_LABEL_LONG_DOT);
  lv_obj_set_size(lblTitle, 184, 26);  // altura fixa: sem ela o modo DOT quebra em varias linhas
  lv_obj_set_style_text_font(lblTitle, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lblTitle, thc(th::SP_TEXT), 0);
  lv_obj_set_pos(lblTitle, 124, 46);
  lv_label_set_text(lblTitle, "");

  lblArtist = lv_label_create(content);
  lv_label_set_long_mode(lblArtist, LV_LABEL_LONG_DOT);
  lv_obj_set_size(lblArtist, 184, 18);
  lv_obj_set_style_text_color(lblArtist, thc(th::SP_SUB), 0);
  lv_obj_set_pos(lblArtist, 124, 74);
  lv_label_set_text(lblArtist, "");

  bar = lv_bar_create(content);
  lv_bar_set_range(bar, 0, 1000);
  lv_obj_set_size(bar, 184, 4);
  lv_obj_set_pos(bar, 124, 112);
  lv_obj_set_style_bg_color(bar, thc(th::SP_TRACK), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bar, thc(th::SP_TEXT), LV_PART_INDICATOR);
  lblPos = lv_label_create(content);
  lv_obj_set_style_text_font(lblPos, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblPos, thc(th::SP_SUB), 0);
  lv_obj_set_pos(lblPos, 124, 121);
  lv_label_set_text(lblPos, "");
  lblDur = lv_label_create(content);
  lv_obj_set_style_text_font(lblDur, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblDur, thc(th::SP_SUB), 0);
  lv_obj_set_width(lblDur, 60);
  lv_obj_set_style_text_align(lblDur, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(lblDur, 248, 121);
  lv_label_set_text(lblDur, "");

  makeCtl(content, 70, 156, 48, LV_SYMBOL_PREV, SpotifyCmd::Prev, false);
  btnPlayLbl = makeCtl(content, 134, 150, 52, LV_SYMBOL_PLAY, SpotifyCmd::PlayPause, true);
  btnPlay = lv_obj_get_parent(btnPlayLbl);
  makeCtl(content, 202, 156, 48, LV_SYMBOL_NEXT, SpotifyCmd::Next, false);

  // Sem conta: orienta a configurar.
  setup = lv_obj_create(tile);
  lv_obj_remove_style_all(setup);
  lv_obj_set_size(setup, SCR_W, SCR_H);
  lv_obj_set_pos(setup, 0, 0);
  lv_obj_clear_flag(setup, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *t = ui_title(setup, "Spotify");
  lv_obj_set_style_text_color(t, thc(th::SP_GREEN), 0);
  lv_obj_t *msg = lv_label_create(setup);
  lv_label_set_text(msg, "Sem conta ligada");
  lv_obj_set_style_text_font(msg, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(msg, thc(th::SP_TEXT), 0);
  lv_obj_set_pos(msg, 12, 64);
  lv_obj_t *hint = ui_muted(setup, "O painel controla o Spotify que ja toca\nno PC ou no celular.");
  lv_obj_set_pos(hint, 12, 90);
  ui_tonal_btn(setup, 12, 132, 0, LV_SYMBOL_SETTINGS " Configurar conta", setupCb);
  lv_obj_add_flag(setup, LV_OBJ_FLAG_HIDDEN);

  // Engrenagem no canto de baixo a direita, na pagina (fica com ou sem conta): abre a conta.
  ui_gear_btn(tile, setupCb);

  shownLinked = true;
  pageTimer = lv_timer_create(refreshCb, 500, nullptr);
  refreshCb(nullptr);
}

// ---- tela da conta -----------------------------------------------------------

static lv_obj_t *acct, *acctCard, *acctStatus, *acctSub, *acctButtons, *forgetLbl, *forgetBtn, *setupBtn, *setupLbl;
static lv_timer_t *acctTimer;
static uint32_t forgetArmedAt;

static void closeAccount() {
  account_portal_stop();
  lv_timer_del(acctTimer);
  acctTimer = nullptr;
  lv_obj_del_async(acct);  // async: chamado de um botao filho dele
  acct = acctCard = acctStatus = acctSub = acctButtons = forgetBtn = setupBtn = nullptr;
  settings_ui_child_closed();
}

static void updateAccount(lv_timer_t *) {
  SpotifyState s = spotify_get();
  char buf[160];
  snprintf(buf, sizeof(buf), "%s\n%s%s", s.linked ? "Conta configurada" : "Sem conta configurada",
           s.error[0] ? "Ultima falha: " : "", s.error);
  lv_label_set_text(acctStatus, buf);
  // Sem conta: so configurar. Com conta: trocar e remover.
  if (forgetBtn) {
    if (s.linked) lv_obj_clear_flag(forgetBtn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(forgetBtn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(setupBtn, s.linked ? 145 : 300);
    lv_label_set_text(setupLbl, s.linked ? LV_SYMBOL_SETTINGS " Trocar conta" : LV_SYMBOL_SETTINGS " Configurar conta");
    lv_obj_center(setupLbl);
  }
  if (forgetArmedAt && board_millis() - forgetArmedAt > 3000) {
    forgetArmedAt = 0;
    lv_label_set_text(forgetLbl, LV_SYMBOL_TRASH " Remover");
  }
}

static void forgetCb(lv_event_t *) {
  if (!forgetArmedAt) {  // dois toques em 3 s, para nao apagar sem querer
    forgetArmedAt = board_millis();
    lv_label_set_text(forgetLbl, "Toque de novo");
    return;
  }
  forgetArmedAt = 0;
  spotify_forget_account();
  lv_label_set_text(forgetLbl, LV_SYMBOL_TRASH " Remover");
}

static void configureCb(lv_event_t *) {
  if (!account_portal_start()) {
    lv_label_set_text(acctStatus, "Conecte o Wi-Fi primeiro\n(Ajustes > Wi-Fi).");
    return;
  }
  AccountPortalInfo info = account_portal_info();
  lv_obj_add_flag(acctCard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(acctButtons, LV_OBJ_FLAG_HIDDEN);  // o QR ocupa o lugar dos botoes
  acctSub = lv_obj_create(acct);
  lv_obj_remove_style_all(acctSub);
  lv_obj_set_size(acctSub, SCR_W, SCR_H - 44);
  lv_obj_set_pos(acctSub, 0, 44);

  lv_obj_t *qr = lv_qrcode_create(acctSub, 100, lv_color_black(), lv_color_white());
  lv_obj_set_style_border_color(qr, lv_color_white(), 0);
  lv_obj_set_style_border_width(qr, 6, 0);
  lv_obj_set_pos(qr, 10, 8);
  lv_qrcode_update(qr, info.url, strlen(info.url));

  lv_obj_t *l = lv_label_create(acctSub);
  lv_obj_set_width(l, 180);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
  lv_obj_set_pos(l, 130, 6);
  lv_label_set_text_fmt(l, "No PC rode:\nspotify_login.py\n<client_id> <codigo>\n\nEndereco da placa:\n%s", info.url + 7);
  lv_obj_t *code = lv_label_create(acctSub);
  lv_obj_set_style_text_font(code, &lv_font_montserrat_28, 0);
  lv_obj_set_pos(code, 130, 126);
  lv_label_set_text(code, info.code);
}

static void backCb(lv_event_t *) { closeAccount(); }

static void openAccount() {
  if (acct || !ui_mem_ok(6 * 1024)) return;
  forgetArmedAt = 0;
  acct = ui_overlay();
  ui_back_header(acct, "Spotify", backCb);

  acctCard = ui_card(acct, 10, 48, 300, 70);
  acctStatus = lv_label_create(acctCard);
  lv_obj_set_width(acctStatus, 280);
  lv_label_set_long_mode(acctStatus, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(acctStatus, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_line_space(acctStatus, 3, 0);
  lv_obj_set_pos(acctStatus, 10, 9);

  acctButtons = lv_obj_create(acct);
  lv_obj_remove_style_all(acctButtons);
  lv_obj_set_size(acctButtons, SCR_W, 40);
  lv_obj_set_pos(acctButtons, 0, 130);
  setupLbl = ui_tonal_btn(acctButtons, 10, 0, 145, LV_SYMBOL_SETTINGS " Configurar", configureCb);
  setupBtn = lv_obj_get_parent(setupLbl);
  forgetLbl = ui_tonal_btn(acctButtons, 165, 0, 145, LV_SYMBOL_TRASH " Remover", forgetCb);
  ui_btn_danger(forgetLbl);
  forgetBtn = lv_obj_get_parent(forgetLbl);

  acctTimer = lv_timer_create(updateAccount, 1000, nullptr);
  updateAccount(nullptr);
}
