#include <Arduino.h>
#include <lvgl.h>
#include <esp_task_wdt.h>

#include "lgfx_cyd.h"
#include "quickpanel.h"
#include "touchcal.h"
#include "account_portal.h"
#include "auth.h"
#include "https_client.h"
#include "pcagent.h"
#include "spotify.h"
#include "ui.h"
#include "usage.h"
#include "usb_link.h"
#include "wifi_mgr.h"

LGFX tft;

static constexpr int SCR_W = 320;
static constexpr int SCR_H = 240;
static constexpr int ROTATION = 1;  // paisagem; 3 = girada 180 graus (recalibrar o toque)
// Linhas por buffer de desenho (2 buffers de DMA = 320 x linhas x 2 B x 2): 24 = 30 KB, 12 = 15 KB.
// Com 40 linhas (51 KB) o maior bloco livre oscilava ate ~19 KB e a TLS (dois buffers de 16,7 KB)
// falhava. A TLS do chatgpt.com precisa de ~78 KB livres de heap, entao 12 linhas devolvem 15 KB a ela;
// o custo e o dobro de blocos por redesenho (mais chamadas de flush; a varredura DMA e a mesma).
// Para voltar a fluidez de antes, suba para 24 (ou 16/20 como meio-termo).
// Com 8 linhas (2 x 5 KB) sobram ~5 KB a mais para a TLS; custo: mais flushes por redesenho (12 -> 8 =
// 1,5x blocos). Se a fluidez piorar, basta voltar a constante para 12.
// Esse e o padrao; a tela Fluidez (Configuracoes > Tela) permite 12 ou 16, lido no boot.
static constexpr int BUF_LINES = 8;
// Toque resistivo com pouca forca falha por alguns ms no meio do arrasto; segurar
// o "pressionado" nesse intervalo evita que o LVGL entenda como soltou.
static constexpr uint32_t TOUCH_HOLD_MS = 50;
// Varredura interna do ST7789 (FRCTRL2): 0x0F = 60 Hz (padrao), 0x01 = 111 Hz.
// Sem o pino TE nao da para sincronizar; varrer mais rapido encurta cada rasgo.
static constexpr uint8_t PANEL_FRCTRL2 = 0x01;

static lv_disp_draw_buf_t dispBuf;
static lv_disp_drv_t dispDrv;
static lv_indev_drv_t indevDrv;

// Buffer duplo + DMA: o LVGL desenha num buffer enquanto o outro vai para a tela.
// pushImageDMA espera o envio anterior terminar antes de comecar o proximo.
static void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  if (tft.getStartCount() == 0) tft.startWrite();
  tft.pushImageDMA(area->x1, area->y1, w, h, (lgfx::swap565_t *)&px->full);
  lv_disp_flush_ready(drv);
}

static void touchReadCb(lv_indev_drv_t *, lv_indev_data_t *data) {
  static int32_t lastX = 0, lastY = 0;
  static uint32_t lastSeen = 0;
  int32_t x, y;
  if (tft.getTouch(&x, &y)) {
    lastX = x;
    lastY = y;
    lastSeen = millis();
    data->state = LV_INDEV_STATE_PR;
  } else if (lastSeen && millis() - lastSeen < TOUCH_HOLD_MS) {
    data->state = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
  data->point.x = lastX;
  data->point.y = lastY;
}

static void logHeap(const char *tag) {
  Serial.printf("[heap] %s livre=%u min=%u maior_bloco=%u reserva=%d\n", tag, ESP.getFreeHeap(),
                ESP.getMinFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                https_reserve_held());
  unsigned nFree, big;
  https_heap_frag(&nFree, &big);
  Serial.printf("[heap] fragmentacao: blocos_livres=%u maior=%u\n", nFree, big);
  if (lv_is_initialized()) {
    // Pool fixo do LVGL (LV_MEM_SIZE): no simulador ele e maior, o limite real e este.
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    Serial.printf("[lvgl] mem usado=%u%% livre=%u maior_bloco=%u\n", mon.used_pct,
                  (unsigned)mon.free_size, (unsigned)mon.free_biggest_size);
  }
}

void setup() {
  Serial.setRxBufferSize(1024);  // comandos do programa do PC (usb_link) chegam em rajadas
  Serial.begin(115200);
  delay(200);
  // A verificacao da cadeia do chatgpt.com (ECDSA) segura o nucleo 0 por mais de 5 s e a IDLE0 estourava o
  // watchdog padrao. 15 s, mantendo o panico (nao desligar o watchdog).
  {
    esp_err_t wr = esp_task_wdt_init(15, true);
    if (wr == ESP_OK) Serial.println("[wdt] task watchdog: 15 s");
    else Serial.printf("[wdt] esp_task_wdt_init falhou: %d\n", (int)wr);
  }
  Serial.println("\nPainel CYD - marco 1");
  logHeap("boot");
  heap_mark("boot (Serial.begin)");

  tft.init();
  tft.startWrite();
  tft.getPanel()->writeCommand(0xC6, 1);
  tft.getPanel()->writeData(PANEL_FRCTRL2, 1);
  tft.endWrite();
  tft.setRotation(ROTATION);
  quickpanel_apply_saved_brightness();
  tft.fillScreen(TFT_BLACK);

  touchcal_init();
  logHeap("apos touch");
  heap_mark("tft.init + touch");

  lv_init();
  // Linhas do buffer: escolha da tela Fluidez (NVS "buf_ln"; 8, 12 ou 16, o resto vira 8).
  int bufLines = ui_buf_lines();
  size_t bufBytes = SCR_W * bufLines * sizeof(lv_color_t);
  auto *buf1 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_DMA);
  auto *buf2 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_DMA);
  if ((!buf1 || !buf2) && bufLines != BUF_LINES) {  // sem memoria DMA para 12/16: volta ao padrao
    if (buf1) heap_caps_free(buf1);
    if (buf2) heap_caps_free(buf2);
    board_log("[ui] alocacao de %d linhas falhou, usando %d", bufLines, BUF_LINES);
    bufLines = BUF_LINES;
    bufBytes = SCR_W * bufLines * sizeof(lv_color_t);
    buf1 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_DMA);
    buf2 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_DMA);
  }
  board_log("[ui] buffer de desenho: %d linhas", bufLines);
  lv_disp_draw_buf_init(&dispBuf, buf1, buf2, SCR_W * bufLines);

  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = SCR_W;
  dispDrv.ver_res = SCR_H;
  dispDrv.flush_cb = flushCb;
  dispDrv.draw_buf = &dispBuf;
  lv_disp_drv_register(&dispDrv);

  lv_indev_drv_init(&indevDrv);
  indevDrv.type = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = touchReadCb;
  ui_setup_indev(&indevDrv);
  lv_indev_drv_register(&indevDrv);
  heap_mark("lv_init + buffers + drivers");

  auth_begin();
  heap_mark("auth_begin");
  usb_link_begin();
  spotify_begin();
  heap_mark("spotify_begin");
  pc_begin();
  heap_mark("pc_begin (task pcstep)");
  usage_begin();  // antes da interface: a pagina Uso de IA ja le o cache ao montar
  heap_mark("usage_begin (task usage)");
  ui_init();
  logHeap("apos ui");
  heap_mark("ui_init");
  https_reserve_init();  // heap ainda inteiro: os dois blocos de lastro evitam falta de memoria na TLS depois
  heap_mark("https_reserve_init");

  wifi_begin();
  heap_mark("wifi_begin");
}

void loop() {
  wifi_loop();
  usb_link_loop();
  account_portal_loop();
  uint32_t wait = lv_timer_handler();

  // Auditoria: Wi-Fi conectado e 10 s depois (mDNS, NTP, primeiras consultas).
  static uint32_t connectedAt = 0;
  static bool marked10 = false;
  if (!connectedAt && wifi_status().state == WifiState::Connected) {
    connectedAt = millis() ? millis() : 1;
    heap_mark("wifi conectado");
  } else if (connectedAt && !marked10 && millis() - connectedAt > 10000) {
    marked10 = true;
    heap_mark("conectado +10 s");
  }

  static uint32_t lastLog = 0, lastValve = 0;
  if (millis() - lastValve >= 1000) {
    lastValve = millis();
    https_reserve_check();
  }
  if (millis() - lastLog > 10000) {
    lastLog = millis();
    logHeap("loop");
  }
  delay(wait < 5 ? wait : 5);
}
