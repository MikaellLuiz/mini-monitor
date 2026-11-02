#pragma once

#include <lvgl.h>

// Digitos e ":" grandes para o Relogio (gerados por tools/gen_clock_font.py).
#ifdef __cplusplus
extern "C" {
#endif
extern const lv_font_t font_clock_big;   // ~80 px: estilos Digital
extern const lv_font_t font_clock_huge;  // ~100 px: estilo Minimo
#ifdef __cplusplus
}
#endif
