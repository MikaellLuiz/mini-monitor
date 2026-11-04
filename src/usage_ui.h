#pragma once

#include <lvgl.h>

#include "usage.h"

// Pagina "Uso de IA": coluna vertical com os tres provedores (Claude, Codex, Antigravity)
// na ordem escolhida em Configuracoes > Tela > Ordem das IAs.
void usage_ui_build_page(lv_obj_t *tile);
void usage_ui_unbuild_page();

// Compatibilidade com o modo de pagina por provedor.
void usage_ui_build_provider_page(lv_obj_t *tile, Provider p);
void usage_ui_unbuild_provider_page(Provider p);

// Navegacao vertical dentro da coluna de IA (delta = +1 desce para a proxima, -1 sobe).
// Devolve true se trocou de IA na coluna.
bool usage_ui_step(int delta);
void usage_ui_select_provider(Provider p);
Provider usage_ui_current_provider();

// Ordem das IAs na coluna (salva na NVS).
void usage_ui_get_order(Provider out[PROV_COUNT]);
void usage_ui_move_order(int idx, int dir);
void usage_ui_order_summary(char *buf, int size);
void usage_ui_open_order();

// Tela da conta (atualizar, trocar token, remover).
void usage_ui_open_account(Provider p);
