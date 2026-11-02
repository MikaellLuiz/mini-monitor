#pragma once

#include <lvgl.h>

// Pagina Relogio, com quatro estilos (Digital, Analogico, Minimo, Anel de segundos) e
// dois atalhos de leitura: a janela de 5h do Claude (abre o Uso de IA) e a musica
// atual (abre o Spotify). As opcoes ficam na NVS e valem na hora.
void clock_ui_build_page(lv_obj_t *tile);
// Antes de o ui.cpp apagar os filhos da tile: apaga o timer e zera os ponteiros.
void clock_ui_unbuild_page();

// Tela de opcoes do Relogio (estilo, 24 h, segundos, atalhos, cor de destaque).
void clock_ui_open_settings();

// Resumo para a lista de Configuracoes ("Digital - 24 h").
void clock_ui_summary(char *buf, int size);
