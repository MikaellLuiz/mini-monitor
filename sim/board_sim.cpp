// board.h no PC: tempo do Windows (ou virtual), log no console e configuracoes
// num arquivo texto ao lado do executavel.

#ifdef _WIN32
#include <windows.h>
#else
#include <stdlib.h>
#include <sys/time.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <fstream>
#include <map>
#include <string>

#include "board.h"
#include "sim.h"

bool g_sim_virtual_time = false;
uint64_t g_sim_vtime_ms = 0;
uint8_t g_sim_brightness = 200;

static const char *SETTINGS_FILE = "sim_settings.txt";

static uint64_t hostMillis() {
#ifdef _WIN32
  return GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
#endif
}

uint32_t board_millis(void) {
  if (g_sim_virtual_time) return (uint32_t)g_sim_vtime_ms;
  static uint64_t start = hostMillis();
  return (uint32_t)(hostMillis() - start);
}

void board_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
  fflush(stdout);
}

void board_set_brightness(uint8_t v) { g_sim_brightness = v; }
uint8_t board_get_brightness(void) { return g_sim_brightness; }
void board_restart(void) { board_log("[sim] reiniciar (ignorado no simulador)"); }
void board_fatal(const char *msg) {
  board_log("*** FATAL: %s", msg);
  abort();
}

void board_request_touch_recalibration(void) { board_log("[sim] recalibrar toque (ignorado)"); }
void board_factory_reset(void) { board_log("[sim] redefinicao completa (ignorada no simulador)"); }

// Valores medidos na placa no marco 1, so para a tela ter numeros realistas.
board_heap_t board_heap(void) { return {232 * 1024, 227 * 1024, 110 * 1024}; }

bool board_local_time(int *hour, int *minute) {
  if (g_sim_virtual_time) {  // hora fixa: screenshots iguais toda vez
    *hour = 14;
    *minute = 32;
    return true;
  }
#ifdef _WIN32
  SYSTEMTIME t;
  GetLocalTime(&t);
  *hour = t.wHour;
  *minute = t.wMinute;
#else
  time_t n = time(nullptr);
  struct tm t = *localtime(&n);
  *hour = t.tm_hour;
  *minute = t.tm_min;
#endif
  return true;
}

// Tempo virtual dos roteiros: 2026-10-30 14:32 local + tempo do roteiro.
static time_t simNow() {
  if (!g_sim_virtual_time) return time(nullptr);
  struct tm t = {};
  t.tm_year = 2026 - 1900;
  t.tm_mon = 9;
  t.tm_mday = 30;
  t.tm_hour = 14;
  t.tm_min = 32;
  t.tm_isdst = -1;
  return mktime(&t) + (time_t)(g_sim_vtime_ms / 1000);
}

bool board_local_datetime(board_datetime_t *dt) {
  time_t n = simNow();
  struct tm t = *localtime(&n);
  *dt = {t.tm_hour, t.tm_min, t.tm_sec, t.tm_wday, t.tm_mday, t.tm_mon + 1};
  return true;
}

bool board_epoch_now(uint32_t *now) {
  *now = (uint32_t)simNow();
  return true;
}

void board_format_time(uint32_t epoch, char *buf, int size) {
  static const char *DAYS[] = {"dom", "seg", "ter", "qua", "qui", "sex", "sab"};
  time_t e = epoch, n = simNow();
  struct tm te = *localtime(&e);
  struct tm tn = *localtime(&n);
  if (te.tm_yday == tn.tm_yday && te.tm_year == tn.tm_year)
    snprintf(buf, size, "%02d:%02d", te.tm_hour, te.tm_min);
  else
    snprintf(buf, size, "%s %02d:%02d", DAYS[te.tm_wday], te.tm_hour, te.tm_min);
}

static std::map<std::string, int> &settings() {
  static std::map<std::string, int> s;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    if (g_sim_virtual_time) return s;  // roteiro sempre parte do padrao: screenshots reproduziveis
    std::ifstream in(SETTINGS_FILE);
    std::string k;
    int v;
    while (in >> k >> v) s[k] = v;
  }
  return s;
}

uint8_t settings_get_u8(const char *key, uint8_t def) {
  auto &s = settings();
  auto it = s.find(key);
  return it == s.end() ? def : (uint8_t)it->second;
}

void settings_set_u8(const char *key, uint8_t v) {
  auto &s = settings();
  s[key] = v;
  if (g_sim_virtual_time) return;  // roteiros nao mexem nas configuracoes salvas
  std::ofstream out(SETTINGS_FILE);
  for (auto &kv : s) out << kv.first << " " << kv.second << "\n";
}
