#pragma once
// Estado compartilhado entre main_sim.cpp e board_sim.cpp.

#include <stdint.h>

extern bool g_sim_virtual_time;  // modo roteiro: o tempo so anda quando o roteiro manda
extern uint64_t g_sim_vtime_ms;
extern uint8_t g_sim_brightness;

// Roteiro "wifi <estado>": off|scanning|connecting|connected|offline|portal|empty
bool wifi_sim_set_state(const char *name);

// Roteiro "usage <claude|codex|agy> <estado>": direct|high|cache|closed|failed|waiting|none
bool usage_sim_set_state(const char *prov, const char *name);

// Roteiro "spotify <estado>": playing|paused|idle|none|error
bool spotify_sim_set_state(const char *name);

// Roteiro "pc <estado>": online|high|offline|none
bool pc_sim_set_state(const char *name);
