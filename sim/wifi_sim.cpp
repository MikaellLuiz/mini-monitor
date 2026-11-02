// Wi-Fi falso do simulador: redes de exemplo e estado trocado pelo roteiro
// (comando "wifi <estado>"), para ver cada tela sem hardware.

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "board.h"
#include "sim.h"
#include "wifi_mgr.h"

static std::vector<std::string> saved = {"CasaNet", "Hotspot celular", "Escritorio 2G"};
static WifiState state = WifiState::Connected;
static std::string cur = "CasaNet";

void wifi_begin() {}
void wifi_loop() {}

WifiStatus wifi_status() {
  WifiStatus s = {};
  s.state = state;
  snprintf(s.ssid, sizeof(s.ssid), "%s", cur.c_str());
  if (state == WifiState::Connected) {
    s.rssi = -58;
    snprintf(s.ip, sizeof(s.ip), "192.168.0.42");
  }
  if (state == WifiState::Offline && !saved.empty()) s.retry_in_s = 27;
  s.saved_count = (uint8_t)saved.size();
  snprintf(s.ap_ssid, sizeof(s.ap_ssid), "Painel-F5DC");
  snprintf(s.ap_pass, sizeof(s.ap_pass), "48213907");
  return s;
}

bool wifi_saved_name(int idx, char *out, int out_size) {
  if (idx < 0 || idx >= (int)saved.size()) return false;
  snprintf(out, out_size, "%s", saved[idx].c_str());
  return true;
}

bool wifi_add_network(const char *ssid, const char *) {
  if (!ssid || !ssid[0]) return false;
  saved.insert(saved.begin(), ssid);
  return true;
}

void wifi_connect_now() { board_log("[sim] wifi: reconectar"); }

void wifi_connect_saved(int idx) {
  if (idx < 0 || idx >= (int)saved.size()) return;
  cur = saved[idx];
  state = WifiState::Connected;
  board_log("[sim] wifi: conectar a %s", cur.c_str());
}

void wifi_forget(int idx) {
  if (idx < 0 || idx >= (int)saved.size()) return;
  saved.erase(saved.begin() + idx);
}

void wifi_move_up(int idx) {
  if (idx <= 0 || idx >= (int)saved.size()) return;
  std::swap(saved[idx], saved[idx - 1]);
}

void wifi_portal_start() { state = WifiState::Portal; }
void wifi_portal_stop() { state = WifiState::Connected; }

bool wifi_sim_set_state(const char *name) {
  static const struct {
    const char *name;
    WifiState st;
  } map[] = {{"off", WifiState::Off},           {"scanning", WifiState::Scanning},
             {"connecting", WifiState::Connecting}, {"connected", WifiState::Connected},
             {"offline", WifiState::Offline},   {"portal", WifiState::Portal}};
  if (!strcmp(name, "empty")) {
    saved.clear();
    state = WifiState::Offline;
    return true;
  }
  for (auto &m : map)
    if (!strcmp(name, m.name)) {
      state = m.st;
      return true;
    }
  return false;
}
