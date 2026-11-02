#pragma once

// Pagina de contas na rede local: com o Wi-Fi conectado, a placa serve em
// http://<ip>/ um formulario para colar os tokens (ex.: o de `claude setup-token`).
// So fica no ar enquanto a tela de conta esta aberta, e exige o codigo de 6
// digitos mostrado no painel (protege contra outro aparelho da rede).
// Placa: src/account_portal_esp32.cpp. Simulador: sim/account_portal_sim.cpp.

#include <stdint.h>

struct AccountPortalInfo {
  bool active;
  char url[32];
  char code[8];
  bool saved;              // um token foi salvo pela pagina
  uint8_t saved_provider;  // qual (valor de enum Provider)
  uint16_t saved_attempts; // usage attempts no momento do salvamento
};

bool account_portal_start();  // false se nao houver Wi-Fi conectado
void account_portal_stop();
AccountPortalInfo account_portal_info();
void account_portal_loop();  // chamar no loop principal
