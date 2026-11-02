#pragma once

#include <lvgl.h>

// Pagina PC (CPU, RAM, GPU, temperatura do PC pareado) e pagina Atalhos (botoes
// definidos no agente do PC). A engrenagem da pagina PC abre o pareamento.
void pc_ui_build_page(lv_obj_t *tile);
// Antes de o ui.cpp apagar os filhos da tile: apaga o timer e zera ponteiros e caches.
void pc_ui_unbuild_page();

// Tela do agente do PC (parear, escolher PC, remover). Aberta pelas Configuracoes e,
// sem agente, pelo toque nos cartoes.
void pc_ui_open_agent();
void shortcuts_ui_build_page(lv_obj_t *tile);
void shortcuts_ui_unbuild_page();
