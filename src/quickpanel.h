#pragma once

// Painel de ajustes que desce do topo. Abre com um gesto curto para baixo em
// qualquer ponto da tela (ver gestureCb em ui.cpp) e fecha com um gesto curto
// para cima sobre ele, ou tocando na alca de baixo.
// Chamar depois de criar as paginas, para ficar por cima de tudo.
void quickpanel_init();
void quickpanel_open();

// Le o brilho salvo no NVS e aplica no backlight. Chamar logo apos tft.init().
void quickpanel_apply_saved_brightness();
