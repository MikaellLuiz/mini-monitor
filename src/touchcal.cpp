#include "touchcal.h"

#include <Preferences.h>

#include "lgfx_cyd.h"

static const char *NVS_NS = "panel";
static const char *NVS_KEY = "touchcal";

void touchcal_init() {
  uint16_t params[8];

  Preferences prefs;
  prefs.begin(NVS_NS, true);
  size_t n = prefs.getBytes(NVS_KEY, params, sizeof(params));
  prefs.end();

  if (n == sizeof(params)) {
    tft.setTouchCalibrate(params);
    Serial.println("touch: calibracao carregada do NVS");
    return;
  }

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setFont(&fonts::Font2);
  tft.drawString("Calibrando o toque", tft.width() / 2, tft.height() / 2 - 20);
  tft.drawString("Toque no centro de cada cruz", tft.width() / 2, tft.height() / 2 + 10);
  delay(2000);

  tft.calibrateTouch(params, TFT_WHITE, TFT_BLACK, 15);
  tft.setTouchCalibrate(params);

  prefs.begin(NVS_NS, false);
  prefs.putBytes(NVS_KEY, params, sizeof(params));
  prefs.end();
  Serial.println("touch: calibracao feita e salva");
}

void touchcal_request_recalibration() {
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  prefs.remove(NVS_KEY);
  prefs.end();
  ESP.restart();
}
