#pragma once

// Agente do PC, como a interface ve. Placa: src/pcagent_esp32.cpp. Simulador:
// sim/pcagent_sim.cpp. O agente (tools/pc_agent/agent.py) roda no PC; a placa o
// consulta por HTTP simples na rede local com um token por painel (guardado na NVS).
// Ate PC_MAX_AGENTS PCs pareados por Wi-Fi. Um PC tambem pode alimentar a placa pelo cabo
// USB (src/usb_link_esp32.cpp). Qual aparece, nesta ordem:
//   1. o PC fixado pelo usuario (pc_select), esteja conectado ou nao;
//   2. o PC que esta enviando metricas pelo USB;
//   3. o primeiro PC do Wi-Fi que respondeu (a placa fica nele; so troca se ele cair).
// O mesmo nome por USB e por Wi-Fi e um PC so.

#include <stdint.h>

static constexpr int PC_MAX_AGENTS = 3;
static constexpr int PC_MAX_SHORTCUTS = 6;
static constexpr int PC_NAME_MAX = 24;           // com o \0
static constexpr uint32_t PC_USB_TIMEOUT_MS = 6000;  // sem linha de metricas ha mais que isso = USB perdido

enum PcSource : uint8_t { PC_SRC_NONE = 0, PC_SRC_USB = 1, PC_SRC_WIFI = 2 };

struct PcMetrics {
  bool paired;   // ha ao menos um PC pareado
  bool online;   // o PC ativo respondeu na ultima consulta
  bool pinned;   // o usuario fixou este PC
  uint8_t source;  // PcSource de onde vem os numeros
  char name[PC_NAME_MAX];
  float cpu, ram, gpu, vram;  // 0-100; < 0 = sem leitura
  int temp;                   // graus C da GPU; < 0 = sem leitura
  char error[40];
  uint32_t updated_ms;  // board_millis() da ultima leitura boa
};

PcMetrics pc_get();

// Atalhos definidos no agente do PC ativo (rotulos curtos).
int pc_shortcut_count();
bool pc_shortcut_label(int i, char *out, int cap);
void pc_shortcut_press(int i);

// A interface avisa quais paginas estao na tela: so consulta o que e visivel.
void pc_set_visible(bool metrics_page, bool shortcuts_page);
// Consulta as metricas agora, sem esperar o intervalo (toque no cartao da pagina PC).
void pc_refresh_now();

// Pareamento recebido pelo portal (/pair). false se os dados nao servem.
bool pc_pair(const char *name, const char *host, const char *ip, int port, const char *token);
void pc_forget_all();
int pc_paired_count();

// ---- escolha do PC -------------------------------------------------------------
// PCs conhecidos: os pareados por Wi-Fi, o do USB (mesmo desconectado, depois de visto) e
// o fixado. src_mask: bit 0 = USB, bit 1 = Wi-Fi.
int pc_choice_count();
bool pc_choice_name(int i, char *out, int cap, uint8_t *src_mask);
// false = automatico (sem PC fixado).
bool pc_pinned_name(char *out, int cap);
// nullptr ou "" volta ao automatico.
void pc_select(const char *name);

// ---- fonte USB (alimentada por usb_link_esp32.cpp) -----------------------------
void pc_usb_feed(const char *name, float cpu, float ram, float gpu, float vram, int temp);
void pc_usb_shortcuts(const char *const *labels, int n);
bool pc_usb_alive();

void pc_begin();
void pc_step();  // chamado a cada segundo pela task de uso
