#pragma once

#include <lvgl.h>
#include <stdint.h>

// Ajustes do toque comuns a placa e ao simulador: gesto curto troca de pagina.
// O padrao do LVGL (velocidade minima 3 px/leitura) zerava a soma em deslizes
// lentos, comuns no toque resistivo.
inline void ui_setup_indev(lv_indev_drv_t *drv) {
  drv->gesture_limit = 25;
  drv->gesture_min_velocity = 1;
}

// Paginas possiveis, na ordem em que aparecem. O uso de IA tem dois modos: uma
// pagina com as tres IAs (PG_USAGE) ou uma pagina por IA (PG_CLAUDE..PG_AGY).
enum PageId : uint8_t { PG_CLOCK, PG_USAGE, PG_CLAUDE, PG_CODEX, PG_AGY, PG_SPOTIFY, PG_SHORTCUTS, PG_PC, PG_COUNT };

// Cria o tileview so com as paginas ligadas e o indicador. Chamar depois do lv_init
// e do registro do display e do touch.
void ui_init();

// true durante a animacao de troca de pagina. Atualizacoes de dados devem esperar
// (guardar o valor e aplicar depois) para nao competir com os quadros da transicao.
bool ui_is_animating();

// Vai para uma pagina com a animacao de troca (os atalhos do Relogio usam isto). O Uso de
// IA vira a pagina do Claude no modo separado. false se a pagina nao existe ou esta escondida.
bool ui_goto(PageId id);

// true se essa pagina e a que esta na tela agora.
bool ui_page_shown(PageId id);

// Configuracao das paginas (NVS). Muda o que ui_init monta: vale apos reiniciar.
const char *ui_page_name(PageId id);
bool ui_ia_separate();
void ui_set_ia_separate(bool on);
bool ui_page_applies(PageId id);  // a pagina existe no modo de IA escolhido
bool ui_page_enabled(PageId id);
void ui_set_page_enabled(PageId id, bool on);
