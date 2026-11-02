#if 1  /* habilita o conteudo */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH 16
/* Bytes ja na ordem do painel: o DMA manda o buffer direto, sem conversao. */
#define LV_COLOR_16_SWAP 1

/* Pool fixo do LVGL: sem PSRAM, o resto do heap fica para Wi-Fi/TLS/BLE. */
#define LV_MEM_CUSTOM 0
#ifdef ESP_PLATFORM
/* O heap e o recurso mais escasso: cada conexao TLS precisa de ~42 KB de uma vez e, com o
   pool em 80 KB alocado no heap, o livre ocioso caiu para ~69 KB e o TLS passou a falhar
   ("SSL - Memory allocation failed"). Por isso o pool e estatico e o maior que cabe na area de
   dados do ESP32 (~62 KB; com 80 KB o link falhava por 18 KB), e o consumo da interface e que
   deve caber nele. Medido em 32 bits (build_sim.py --m32 com sim/scripts/mem.txt): so as
   paginas usam 38 KB (43 KB com IA separada); cada tela de Configuracoes soma ~5-9 KB.
   Telas pesadas conferem ui_mem_ok() antes de abrir, em vez de o assert reiniciar o painel. */
#define LV_MEM_SIZE (60U * 1024U)
#else
/* Simulador em 64 bits: ponteiros dobram o tamanho dos objetos. O limite real
   e o da placa; olhar o uso do pool la (log "[lvgl] mem"). */
#define LV_MEM_SIZE (160U * 1024U)
#endif

/* Falha de assert (ex.: pool cheio) avisa e reinicia/aborta, em vez do while(1)
   silencioso padrao, que so congela a tela. */
#define LV_ASSERT_HANDLER_INCLUDE "board.h"
#define LV_ASSERT_HANDLER board_fatal("assert do LVGL (pool cheio?)");

/* board_millis(): millis() na placa, relogio do PC no simulador. */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "board.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (board_millis())

#define LV_DISP_DEF_REFR_PERIOD 20
#define LV_INDEV_DEF_READ_PERIOD 10

#define LV_USE_LOG 0

/* Rotinas de desenho mais quentes na IRAM: evita falta de cache da flash. */
#ifdef ESP_PLATFORM
#include <esp_attr.h>
#define LV_ATTRIBUTE_FAST_MEM IRAM_ATTR
#endif

/* Contador de FPS/CPU no canto da tela, ligado por -DUI_PERF no platformio.ini. */
#ifdef UI_PERF
#define LV_USE_PERF_MONITOR 1
#define LV_USE_PERF_MONITOR_POS LV_ALIGN_BOTTOM_RIGHT
#endif

#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1  // so os digitos do Relogio usam (~95 KB de flash)
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* QR code do portal Wi-Fi (entrar na rede e abrir a pagina). */
#define LV_USE_QRCODE 1

#endif
#endif
