#pragma once

// Enlace USB com o programa do PC (tools/pc_agent). Usa a mesma UART dos logs, entao
// so vale a linha que COMECA com "@PC " seguida de um JSON; o resto e log e e ignorado
// (dos dois lados). Placa: src/usb_link_esp32.cpp. O simulador nao tem USB.
//
//   PC -> placa  {"id":7,"cmd":"hello"}                 (id opcional; sem id nao ha resposta)
//   placa -> PC  {"id":7,"ok":true,...}                 resposta
//   placa -> PC  {"evt":"action","idx":3}               atalho tocado na tela
//
// Sem sessao autenticada so valem hello, auth e pin_create. A sessao expira depois de
// USB_SESSION_TIMEOUT_MS sem nenhuma linha valida (as metricas a mantem viva).

#include <stdint.h>

static constexpr uint32_t USB_SESSION_TIMEOUT_MS = 60000;
static constexpr int USB_PROTOCOL = 1;

void usb_link_begin();
void usb_link_loop();  // sem bloqueio; chamar no loop principal
bool usb_link_authed();
// Leva o atalho tocado ao PC do cabo. false se nao ha sessao.
bool usb_link_send_action(int idx);
