#pragma once

#include <lvgl.h>

// Pagina Spotify: capa, faixa, progresso e controles (anterior, play/pause, proxima).
// O engrenagem abre a tela da conta (configurar pelo PC, remover).
void spotify_ui_build_page(lv_obj_t *tile);
// Antes de o ui.cpp apagar os filhos da tile: apaga o timer e zera ponteiros e caches.
void spotify_ui_unbuild_page();

// Tela da conta do Spotify (aberta pelas Configuracoes ou pelo botao sem conta).
void spotify_ui_open_account();
