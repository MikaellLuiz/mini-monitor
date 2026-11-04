#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <nvs_flash.h>
#include <stdarg.h>
#include <time.h>

#include "board.h"
#include "lgfx_cyd.h"
#include "touchcal.h"

static const char *NVS_NS = "panel";

uint32_t board_millis(void) { return millis(); }

void board_log(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof(buf) - 3) n = sizeof(buf) - 3;
  buf[n++] = '\r';
  buf[n++] = '\n';
  Serial.write((const uint8_t *)buf, n);
}

void board_set_brightness(uint8_t v) { tft.setBrightness(v); }
uint8_t board_get_brightness(void) { return tft.getBrightness(); }
void board_restart(void) {
  Serial.flush();
  delay(50);
  ESP.restart();
}

void board_fatal(const char *msg) {
  Serial.printf("\n*** FATAL: %s - reiniciando\n", msg);
  Serial.flush();
  delay(200);
  ESP.restart();
}
void board_request_touch_recalibration(void) { touchcal_request_recalibration(); }

void board_factory_reset(void) {
  Serial.println("*** redefinicao completa: apagando a NVS");
  Serial.flush();
  nvs_flash_erase();  // a NVS inteira: o Arduino a recria no proximo boot
  delay(200);
  ESP.restart();
}

board_heap_t board_heap(void) {
  return {ESP.getFreeHeap(), ESP.getMinFreeHeap(),
          (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)};
}

bool board_local_time(int *hour, int *minute) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;  // ainda em 1970: NTP nao respondeu
  struct tm t;
  localtime_r(&now, &t);
  *hour = t.tm_hour;
  *minute = t.tm_min;
  return true;
}

bool board_local_datetime(board_datetime_t *dt) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  struct tm t;
  localtime_r(&now, &t);
  *dt = {t.tm_hour, t.tm_min, t.tm_sec, t.tm_wday, t.tm_mday, t.tm_mon + 1};
  return true;
}

bool board_epoch_now(uint32_t *now) {
  time_t t = time(nullptr);
  if (t < 1700000000) return false;
  *now = (uint32_t)t;
  return true;
}

void board_format_time(uint32_t epoch, char *buf, int size) {
  static const char *DAYS[] = {"dom", "seg", "ter", "qua", "qui", "sex", "sab"};
  time_t e = epoch, n = time(nullptr);
  struct tm te, tn;
  localtime_r(&e, &te);
  localtime_r(&n, &tn);
  if (te.tm_yday == tn.tm_yday && te.tm_year == tn.tm_year)
    snprintf(buf, size, "%02d:%02d", te.tm_hour, te.tm_min);
  else
    snprintf(buf, size, "%s %02d:%02d", DAYS[te.tm_wday], te.tm_hour, te.tm_min);
}

uint8_t settings_get_u8(const char *key, uint8_t def) {
  Preferences prefs;
  prefs.begin(NVS_NS, true);
  uint8_t v = prefs.getUChar(key, def);
  prefs.end();
  return v;
}

void settings_set_u8(const char *key, uint8_t v) {
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  prefs.putUChar(key, v);
  prefs.end();
}
