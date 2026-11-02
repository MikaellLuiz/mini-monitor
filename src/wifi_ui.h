#pragma once

#include <lvgl.h>

// Barra de status (icone do Wi-Fi + hora) e tela do portal com QR. Chamar depois
// de criar as paginas.
void wifi_ui_init_overlays();

// Tela Wi-Fi, aberta pelo item Wi-Fi dos ajustes: rede atual, adicionar pelo
// celular, redes salvas (conectar, subir prioridade, esquecer) e reconectar.
void wifi_ui_open();

// Resumo curto para o item Wi-Fi dos ajustes ("CasaNet - conectado").
void wifi_ui_summary(char *buf, int size);
