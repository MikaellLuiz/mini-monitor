// Agente do PC falso do simulador: estados escolhidos pelo roteiro ("pc <estado>").

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "pcagent.h"
#include "sim.h"

static PcMetrics m;
static int nShort = 6;
static const char *LABELS[PC_MAX_SHORTCUTS] = {"Mudo", "Vol -", "Vol +", "Print", "Tarefas", "Bloquear"};
// PCs de exemplo para a tela "Escolher PC": o primeiro esta no cabo, o segundo no Wi-Fi.
static const char *CHOICES[] = {"Mikael-Luiz-PC", "Notebook"};
static const uint8_t CHOICE_MASK[] = {1 | 2, 2};
static char pinned[PC_NAME_MAX] = "";

static void online() {
  m = {};
  m.paired = m.online = true;
  m.source = PC_SRC_USB;
  snprintf(m.name, sizeof(m.name), "Mikael-Luiz-PC");
  m.cpu = 23; m.ram = 85; m.gpu = 41; m.vram = 26; m.temp = 54;
  nShort = 6;
}

void pc_begin() { online(); }
PcMetrics pc_get() {
  PcMetrics r = m;
  r.pinned = pinned[0] != 0;
  return r;
}
int pc_shortcut_count() { return m.paired && m.online ? nShort : 0; }
bool pc_shortcut_label(int i, char *out, int cap) {
  if (i < 0 || i >= pc_shortcut_count()) return false;
  snprintf(out, cap, "%s", LABELS[i]);
  return true;
}
void pc_shortcut_press(int i) { board_log("[sim] atalho do PC %d", i); }
void pc_set_visible(bool, bool) {}
void pc_refresh_now() {}
bool pc_pair(const char *, const char *, const char *, int, const char *) { online(); return true; }
void pc_forget_all() { m = {}; m.cpu = m.ram = m.gpu = m.vram = -1; m.temp = -1; nShort = 0; pinned[0] = 0; }
int pc_paired_count() { return m.paired ? 1 : 0; }

int pc_choice_count() { return 2; }
bool pc_choice_name(int i, char *out, int cap, uint8_t *mask) {
  if (i < 0 || i > 1) return false;
  snprintf(out, cap, "%s", CHOICES[i]);
  if (mask) *mask = CHOICE_MASK[i];
  return true;
}
bool pc_pinned_name(char *out, int cap) {
  if (!pinned[0]) return false;
  snprintf(out, cap, "%s", pinned);
  return true;
}
void pc_select(const char *name) { snprintf(pinned, sizeof(pinned), "%s", name ? name : ""); }
void pc_usb_feed(const char *, float, float, float, float, int) {}
void pc_usb_shortcuts(const char *const *, int) {}
bool pc_usb_alive() { return true; }

void pc_step() {}

// Roteiro: pc <online|wifi|high|offline|none>
bool pc_sim_set_state(const char *name) {
  if (!strcmp(name, "online")) online();
  else if (!strcmp(name, "wifi")) { online(); m.source = PC_SRC_WIFI; }
  else if (!strcmp(name, "high")) { online(); m.cpu = 92; m.gpu = 97; m.temp = 81; }
  else if (!strcmp(name, "offline")) { online(); m.online = false; m.source = PC_SRC_NONE; snprintf(m.error, sizeof(m.error), "PC offline"); }
  else if (!strcmp(name, "none")) pc_forget_all();
  else return false;
  return true;
}
