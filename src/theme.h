#pragma once

// Visual comum das telas (design "Telas Mini Monitor CYD"): fundo quase preto,
// cartoes escuros sem borda, botoes tonais no estilo Android e listas com seta.
// Sem sombra nem borda em lugar nenhum: cada quadro da troca de pagina redesenha
// a tela inteira e essas sao as partes mais caras do renderizador.

#include <lvgl.h>

// Cada cor: nome, valor no tema escuro, valor no tema claro. O tema vale desde o boot
// (as telas sao montadas uma vez), entao trocar de tema reinicia o painel.
#define TH_TOKENS(X) \
  X(BG, 0x0A0C0F, 0xEEF0F3)  /* fundo das paginas */ \
  X(CARD, 0x14181E, 0xFFFFFF)  /* cartoes */ \
  X(CARD_HI, 0x1B2433, 0xE1EAFB)  /* cartao em destaque */ \
  X(LINE, 0x222830, 0xE3E6EA)  /* divisoria de lista */ \
  X(TRACK, 0x252B34, 0xDDE1E6)  /* trilho das barras */ \
  X(TEXT, 0xE8EAED, 0x111418) \
  X(MUTED, 0x9AA0A6, 0x5B636D) \
  X(TONAL, 0x1F2A3A, 0xD8E3F8)  /* botao tonal */ \
  X(ON_TONAL, 0xA8C7FA, 0x0B57D0) \
  X(TONAL_PR, 0x2B3D58, 0xC2D3F2) \
  X(MINI, 0x181D24, 0xDDE1E6)  /* botao discreto */ \
  X(ON_MINI, 0xC9CED6, 0x2F353C) \
  X(MINI_PR, 0x262C35, 0xCDD2D8) \
  X(DANGER, 0x33201F, 0xFBE3E1) \
  X(ON_DANGER, 0xF2B8B5, 0xB3261E) \
  X(DANGER_PR, 0x4A2B2A, 0xF5CDC9) \
  X(GREEN, 0x6DD58C, 0x1E8E3E) \
  X(AMBER, 0xF6B26B, 0xB06000) \
  X(RED, 0xF28B82, 0xC5221F) \
  X(CLAUDE, 0xD97757, 0xB5532F) \
  X(CODEX, 0x5FC9B8, 0x0F7D6E) \
  X(AGY, 0xB39DFF, 0x5B3FC4) \
  X(ACCENT, 0x8AB4F8, 0x0B57D0)  /* azul de destaque */ \
  X(ROW_PR, 0x1E242C, 0xEDF0F4)  /* linha de lista tocada */ \
  X(FILL, 0x2B3D58, 0xB9CCF2)  /* barra de brilho */ \
  X(ON_FILL, 0xD3E3FD, 0x0B2A66) \
  X(DOT, 0x353B44, 0xC4C9D0)  /* pontos de pagina */ \
  X(DOT_ON, 0xC9CED6, 0x2F353C) \
  X(HANDLE, 0x3A4049, 0xB7BDC5) \
  X(SHEET, 0x1A1F26, 0xFFFFFF)  /* folha de baixo */ \
  X(SENT_BG, 0x15241A, 0xDDF2E3)  /* "atalho enviado" */ \
  X(PRIMARY, 0xA8C7FA, 0x0B57D0)  /* botao principal */ \
  X(ON_PRIMARY, 0x0A2A55, 0xFFFFFF) \
  X(SP_BG, 0x000000, 0xFFFFFF)  /* pagina do Spotify */ \
  X(SP_TEXT, 0xFFFFFF, 0x111111) \
  X(SP_SUB, 0xB3B3B3, 0x5E5E5E) \
  X(SP_GREEN, 0x1ED760, 0x0F7A35) \
  X(SP_TRACK, 0x3E3E3E, 0xDADADA) \
  X(SP_COVER, 0x282828, 0xE3E3E3) \
  X(SP_PR, 0x222222, 0xE6E6E6)

namespace th {
#define X(name, dark, light) extern uint32_t name;
TH_TOKENS(X)
#undef X
}  // namespace th

inline lv_color_t thc(uint32_t hex) { return lv_color_hex(hex); }

// Le o tema salvo (NVS), preenche as cores acima e configura o LVGL. Chamar antes de
// criar qualquer tela.
void theme_init();
bool theme_is_light();
// Salva o tema; vale depois de reiniciar.
void theme_set_light(bool light);
// Cor de destaque do Relogio: indice 0-3 (azul, argila, verde, cinza) no tema atual.
uint32_t theme_accent(int idx);

// true se o pool do LVGL tem um bloco livre de pelo menos `bytes`. As telas pesadas (Configuracoes,
// Paginas, opcoes do Relogio) checam antes de abrir: sem memoria elas simplesmente nao abrem, em vez
// de o assert do LVGL reiniciar o painel.
bool ui_mem_ok(uint32_t bytes);

// Titulo de pagina (20 px) no canto superior esquerdo.
lv_obj_t *ui_title(lv_obj_t *parent, const char *text);
// Texto pequeno e cinza.
lv_obj_t *ui_muted(lv_obj_t *parent, const char *text, const lv_font_t *font = &lv_font_montserrat_12);
// Cartao escuro arredondado, sem rolagem.
lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color = th::CARD);
// Barra fina com trilho escuro.
lv_obj_t *ui_bar(lv_obj_t *parent, int w, int h, uint32_t color);

// Botao tonal em pilula; devolve o rotulo. h padrao: alvo de toque confortavel.
lv_obj_t *ui_tonal_btn(lv_obj_t *parent, int x, int y, int w, const char *text, lv_event_cb_t cb,
                       void *user = nullptr, int h = 34);
// Botao discreto e menor (acoes raras: recalibrar, reiniciar, reconectar).
lv_obj_t *ui_mini_btn(lv_obj_t *parent, int x, int y, const char *text, lv_event_cb_t cb, void *user = nullptr);
// Botao redondo so com icone (engrenagem das paginas).
lv_obj_t *ui_icon_btn(lv_obj_t *parent, int x, int y, const char *symbol, lv_event_cb_t cb, void *user = nullptr);
// Engrenagem de 28 px no canto inferior direito de uma pagina (a esquerda e a barra de status e no
// meio os pontos). Um rotulo clicavel so, barato no pool do LVGL. Devolve o objeto.
lv_obj_t *ui_gear_btn(lv_obj_t *parent, lv_event_cb_t cb);
// Pinta um botao tonal como perigoso (remover, esquecer).
void ui_btn_danger(lv_obj_t *label);

// Linha de lista: icone num circulo, titulo, subtitulo opcional e seta.
// Devolve o objeto do subtitulo (ou do titulo, sem subtitulo) para atualizar depois.
struct UiRow {
  lv_obj_t *row, *title, *sub, *trail;
};
UiRow ui_list_row(lv_obj_t *list, const char *icon, const char *title, const char *sub, lv_event_cb_t cb,
                  void *user = nullptr, int h = 40);
// Cartao que empilha linhas com divisorias.
lv_obj_t *ui_list(lv_obj_t *parent, int x, int y, int w);

// Tela cheia na camada de cima que nao deixa gesto vazar para as paginas.
lv_obj_t *ui_overlay();
// Cabecalho de subtela: seta de voltar + titulo.
lv_obj_t *ui_back_header(lv_obj_t *overlay, const char *title, lv_event_cb_t backCb);

// Indicador redondo pequeno (cor do provedor, status).
lv_obj_t *ui_dot(lv_obj_t *parent, int size, uint32_t color);
