#pragma once

// Carrega a calibracao do touch do NVS; se nao houver, roda a calibracao
// interativa (4 cantos) e guarda o resultado. Chamar depois de setRotation().
void touchcal_init();

// Apaga a calibracao salva e reinicia; a proxima partida recalibra.
void touchcal_request_recalibration();
