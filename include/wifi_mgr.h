#pragma once

// API de Wi-Fi usada pela interface. Placa: src/wifi_esp32.cpp. Simulador:
// sim/wifi_sim.cpp (falso, controlado pelo roteiro).
//
// Redes salvas: ate WIFI_MAX_SAVED, e a ordem da lista e a prioridade. No boot
// (e ao cair) a placa varre e tenta as salvas que estao visiveis, nessa ordem;
// o hotspot do celular e so mais uma rede salva. Sem nenhuma: offline, com nova
// tentativa em 30 s, 1 min e depois a cada 5 min.

#include <stdint.h>

static constexpr int WIFI_MAX_SAVED = 5;

enum class WifiState : uint8_t { Off, Scanning, Connecting, Connected, Offline, Portal };

struct WifiStatus {
  WifiState state;
  char ssid[33];        // conectado ou tentando
  int8_t rssi;          // dBm, quando conectado
  char ip[16];
  uint32_t retry_in_s;  // offline: segundos ate a proxima tentativa (0 = nenhuma)
  uint8_t saved_count;
  char ap_ssid[20];     // portal: rede que a placa abriu
  char ap_pass[12];
};

void wifi_begin();
void wifi_loop();
WifiStatus wifi_status();

// Copia o nome da rede salva idx (0 = maior prioridade). false se nao existe.
bool wifi_saved_name(int idx, char *out, int out_size);
// Salva uma rede (no topo da lista) e tenta conectar. false se os dados nao servem
// (SSID de 1 a 32 caracteres; senha vazia ou de 8 a 63).
bool wifi_add_network(const char *ssid, const char *pass);
void wifi_connect_now();          // varre e conecta na melhor salva
void wifi_connect_saved(int idx); // tenta essa rede direto
void wifi_forget(int idx);
void wifi_move_up(int idx);

// Portal pelo celular: abre a rede Painel-XXXX com senha aleatoria e uma pagina
// em 192.168.4.1 para escolher a rede e digitar a senha. Fecha sozinho em 5 min.
void wifi_portal_start();
void wifi_portal_stop();
