// Simulador da interface no PC: mesma ui.cpp/quickpanel.cpp da placa, 320x240.
//
//   sim.exe                         janela interativa (mouse = toque, P = screenshot)
//   sim.exe --script roteiro.txt    sem janela, tempo virtual, salva PNGs
//            [--out pasta]
//
// Comandos do roteiro (um por linha, '#' comenta):
//   wait <ms> | tap <x> <y> | swipe <x1> <y1> <x2> <y2> <ms> | shot <arquivo.png>
//   wifi <off|scanning|connecting|connected|offline|portal|empty>
//   usage <claude|codex|agy> <direct|high|cache|closed|failed|waiting|none>
//
// O que o simulador NAO reproduz: velocidade do ESP32, SPI, tearing e a
// sensacao do toque resistivo. FPS/CPU so valem medidos na placa.

#include <windows.h>
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "board.h"
#include "png_write.h"
#include "quickpanel.h"
#include "sim.h"
#include "pcagent.h"
#include "spotify.h"
#include "ui.h"
#include "usage.h"

static constexpr int W = 320;
static constexpr int H = 240;
static constexpr int SCALE = 2;
static constexpr int BUF_LINES = 40;

static uint32_t fb[W * H];  // 0x00RRGGBB
static bool fbDirty = true;
static struct {
  int x = 0, y = 0;
  bool pressed = false;
} pointer;

static HWND hwnd;
static std::string outDir = "sim_shots";

static void flushCb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
  for (int y = a->y1; y <= a->y2; y++)
    for (int x = a->x1; x <= a->x2; x++) fb[y * W + x] = lv_color_to32(*px++) & 0xFFFFFF;
  fbDirty = true;
  lv_disp_flush_ready(drv);
}

static void readCb(lv_indev_drv_t *, lv_indev_data_t *data) {
  data->point.x = pointer.x;
  data->point.y = pointer.y;
  data->state = pointer.pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

// Aplica o brilho como na tela de verdade, so na hora de mostrar/salvar.
static inline uint32_t dim(uint32_t c) {
  uint32_t k = g_sim_brightness;
  return ((((c >> 16) & 0xFF) * k / 255) << 16) | ((((c >> 8) & 0xFF) * k / 255) << 8) |
         ((c & 0xFF) * k / 255);
}

static bool saveShot(const std::string &name) {
  std::vector<uint8_t> rgb((size_t)W * SCALE * H * SCALE * 3);
  for (int y = 0; y < H * SCALE; y++)
    for (int x = 0; x < W * SCALE; x++) {
      uint32_t c = dim(fb[(y / SCALE) * W + x / SCALE]);
      uint8_t *p = &rgb[((size_t)y * W * SCALE + x) * 3];
      p[0] = c >> 16; p[1] = c >> 8; p[2] = c;
    }
  CreateDirectoryA(outDir.c_str(), nullptr);
  std::string path = outDir + "/" + name;
  bool ok = png_write(path.c_str(), W * SCALE, H * SCALE, rgb.data());
  board_log("[sim] screenshot %s %s", path.c_str(), ok ? "ok" : "FALHOU");
  return ok;
}

// ---- modo roteiro -----------------------------------------------------------

static void step(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 5) {
    g_sim_vtime_ms += 5;
    lv_timer_handler();
  }
}

static int runScript(const char *path) {
  std::ifstream in(path);
  if (!in) {
    board_log("[sim] nao abri o roteiro %s", path);
    return 1;
  }
  step(200);
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    n++;
    size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    std::istringstream ss(line);
    std::string cmd;
    if (!(ss >> cmd)) continue;
    if (cmd == "wait") {
      int ms; ss >> ms; step(ms);
    } else if (cmd == "tap") {
      ss >> pointer.x >> pointer.y;
      pointer.pressed = true; step(80);
      pointer.pressed = false; step(80);
    } else if (cmd == "swipe") {
      int x1, y1, x2, y2, ms; ss >> x1 >> y1 >> x2 >> y2 >> ms;
      pointer.x = x1; pointer.y = y1; pointer.pressed = true; step(20);
      int steps = ms / 10 > 0 ? ms / 10 : 1;
      for (int i = 1; i <= steps; i++) {
        pointer.x = x1 + (x2 - x1) * i / steps;
        pointer.y = y1 + (y2 - y1) * i / steps;
        step(10);
      }
      pointer.pressed = false; step(20);
    } else if (cmd == "mem") {  // uso do pool do LVGL (so vale em 32 bits: build_sim.py --m32)
      lv_mem_monitor_t mon;
      lv_mem_monitor(&mon);
      std::string tag; ss >> tag;
      board_log("[mem] %-18s usado=%u B  livre=%u B  maior_bloco=%u B  (pool da placa: 57344 B)", tag.c_str(),
                (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.free_size, (unsigned)mon.free_biggest_size);
    } else if (cmd == "shot") {
      std::string name; ss >> name; saveShot(name);
    } else if (cmd == "usage") {
      std::string prov, st; ss >> prov >> st;
      if (!usage_sim_set_state(prov.c_str(), st.c_str())) {
        board_log("[sim] linha %d: estado de uso desconhecido '%s'", n, st.c_str());
        return 1;
      }
      step(1100);  // a pagina le o uso a cada 1 s
    } else if (cmd == "pc") {
      std::string st; ss >> st;
      if (!pc_sim_set_state(st.c_str())) {
        board_log("[sim] linha %d: estado do pc desconhecido '%s'", n, st.c_str());
        return 1;
      }
      step(1200);
    } else if (cmd == "spotify") {
      std::string st; ss >> st;
      if (!spotify_sim_set_state(st.c_str())) {
        board_log("[sim] linha %d: estado do spotify desconhecido '%s'", n, st.c_str());
        return 1;
      }
      step(700);
    } else if (cmd == "wifi") {
      std::string st; ss >> st;
      if (!wifi_sim_set_state(st.c_str())) {
        board_log("[sim] linha %d: estado de wifi desconhecido '%s'", n, st.c_str());
        return 1;
      }
      step(600);  // a interface le o estado a cada 500 ms
    } else {
      board_log("[sim] linha %d: comando desconhecido '%s'", n, cmd.c_str());
      return 1;
    }
  }
  return 0;
}

// ---- modo janela ------------------------------------------------------------

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_LBUTTONDOWN:
      SetCapture(h);
      pointer.pressed = true;
      [[fallthrough]];
    case WM_MOUSEMOVE:
      pointer.x = GET_X_LPARAM(lp) / SCALE;
      pointer.y = GET_Y_LPARAM(lp) / SCALE;
      if (pointer.x < 0) pointer.x = 0;
      if (pointer.x >= W) pointer.x = W - 1;
      if (pointer.y < 0) pointer.y = 0;
      if (pointer.y >= H) pointer.y = H - 1;
      return 0;
    case WM_LBUTTONUP:
      ReleaseCapture();
      pointer.pressed = false;
      return 0;
    case WM_KEYDOWN:
      if (wp == 'P') {
        static int count = 0;
        char name[32];
        snprintf(name, sizeof(name), "manual_%02d.png", ++count);
        saveShot(name);
      }
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      static uint32_t shown[W * H];
      for (int i = 0; i < W * H; i++) shown[i] = dim(fb[i]);
      BITMAPINFO bmi = {};
      bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
      bmi.bmiHeader.biWidth = W;
      bmi.bmiHeader.biHeight = -H;  // de cima para baixo
      bmi.bmiHeader.biPlanes = 1;
      bmi.bmiHeader.biBitCount = 32;
      bmi.bmiHeader.biCompression = BI_RGB;
      StretchDIBits(dc, 0, 0, W * SCALE, H * SCALE, 0, 0, W, H, shown, &bmi, DIB_RGB_COLORS, SRCCOPY);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcA(h, msg, wp, lp);
}

static int runWindow() {
  WNDCLASSA wc = {};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = "PainelCydSim";
  RegisterClassA(&wc);

  RECT r = {0, 0, W * SCALE, H * SCALE};
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  AdjustWindowRect(&r, style, FALSE);
  hwnd = CreateWindowA(wc.lpszClassName, "Painel CYD - simulador (P = screenshot)", style,
                       CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr,
                       nullptr, wc.hInstance, nullptr);
  ShowWindow(hwnd, SW_SHOW);

  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) return 0;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    lv_timer_handler();
    if (fbDirty) {
      fbDirty = false;
      InvalidateRect(hwnd, nullptr, FALSE);
    }
    Sleep(5);
  }
}

int main(int argc, char **argv) {
  const char *script = nullptr;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
    else if (!strcmp(argv[i], "--out") && i + 1 < argc) outDir = argv[++i];
  }
  g_sim_virtual_time = script != nullptr;
  // --set chave=valor: configuracao antes da interface montar (ex.: ia_sep=1).
  for (int i = 1; i + 1 < argc; i++) {
    if (strcmp(argv[i], "--set")) continue;
    const char *kv = argv[++i];
    const char *eq = strchr(kv, '=');
    if (!eq) continue;
    std::string key(kv, eq - kv);
    settings_set_u8(key.c_str(), (uint8_t)atoi(eq + 1));
  }

  lv_init();
  static lv_color_t buf1[W * BUF_LINES], buf2[W * BUF_LINES];
  static lv_disp_draw_buf_t drawBuf;
  lv_disp_draw_buf_init(&drawBuf, buf1, buf2, W * BUF_LINES);

  static lv_disp_drv_t dispDrv;
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = W;
  dispDrv.ver_res = H;
  dispDrv.flush_cb = flushCb;
  dispDrv.draw_buf = &drawBuf;
  lv_disp_drv_register(&dispDrv);

  static lv_indev_drv_t indevDrv;
  lv_indev_drv_init(&indevDrv);
  indevDrv.type = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = readCb;
  ui_setup_indev(&indevDrv);
  lv_indev_drv_register(&indevDrv);

  quickpanel_apply_saved_brightness();
  usage_begin();
  spotify_begin();
  pc_begin();
  ui_init();

  return script ? runScript(script) : runWindow();
}
