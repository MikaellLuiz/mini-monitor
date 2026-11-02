#pragma once

#include <lvgl.h>

#include "usage.h"

// Pagina "Uso de IA": um cartao por provedor (duas janelas, horario de reset e
// selo de origem). Tocar no cartao atualiza; sem conta, abre a configuracao dela.
void usage_ui_build_page(lv_obj_t *tile);
// Antes de o ui.cpp apagar os filhos da tile: apaga timers e zera ponteiros e caches.
void usage_ui_unbuild_page();
void usage_ui_unbuild_provider_page(Provider p);

// Pagina de uma IA so (modo "IA separada"): anel com a primeira janela e cartoes
// com o reset e a segunda janela. Sem conta, oferece configurar.
void usage_ui_build_provider_page(lv_obj_t *tile, Provider p);

// Tela da conta (atualizar, trocar token, remover). Aberta pelas Configuracoes e,
// sem conta, pelo toque no cartao da IA.
void usage_ui_open_account(Provider p);
