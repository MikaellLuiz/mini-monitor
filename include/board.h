#pragma once

// Tudo o que a interface (ui.cpp, quickpanel.cpp) precisa do hardware.
// Implementado em board_esp32.cpp (placa) e sim/board_sim.cpp (simulador no PC),
// para o mesmo codigo de interface rodar nos dois.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t board_millis(void);
void board_log(const char *fmt, ...);

void board_set_brightness(uint8_t v);
uint8_t board_get_brightness(void);
void board_restart(void);
void board_fatal(const char *msg);  // registra e reinicia (placa) / aborta (simulador)
void board_request_touch_recalibration(void);
// Apaga TUDO o que esta gravado (PIN, Wi-Fi, contas, PCs, calibracao do toque) e reinicia.
void board_factory_reset(void);

typedef struct {
  uint32_t free_bytes;
  uint32_t min_free_bytes;
  uint32_t largest_block;
} board_heap_t;
board_heap_t board_heap(void);

// Hora local. false enquanto o relogio nao foi sincronizado (NTP pelo Wi-Fi).
bool board_local_time(int *hour, int *minute);
// Hora e data local completas, para a pagina Relogio. false sem NTP.
typedef struct {
  int hour, minute, second;
  int wday;  // 0 = domingo
  int mday;
  int month;  // 1-12
} board_datetime_t;
bool board_local_datetime(board_datetime_t *dt);
// Segundos Unix de agora; false enquanto o relogio nao foi sincronizado.
bool board_epoch_now(uint32_t *now);
// Horario local curto: "14:20" se for hoje, "sex 09:00" se for outro dia.
void board_format_time(uint32_t epoch, char *buf, int size);

// Configuracoes persistentes: NVS na placa, arquivo no simulador.
uint8_t settings_get_u8(const char *key, uint8_t def);
void settings_set_u8(const char *key, uint8_t v);

#ifdef __cplusplus
}
#endif
