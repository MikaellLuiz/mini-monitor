#pragma once

// Configuracoes: tudo o que se mexe so na instalacao ou de vez em quando, fora das paginas do
// dia a dia. Contas (Claude, Codex, Antigravity, Spotify, agente do PC), Relogio, Paginas,
// Tema e Sistema (diagnostico, recalibrar o toque, reiniciar, redefinir tudo).
void settings_ui_open();

// Abre direto uma categoria (engrenagem das paginas). O voltar da categoria FECHA, sem passar pela
// lista; telas filhas abertas dela voltam para ela.
enum SettingsCat { SETCAT_ACCOUNTS = 0, SETCAT_SCREEN, SETCAT_SYSTEM };
void settings_ui_open_category(SettingsCat cat);

// Escolha das paginas visiveis e do modo do Uso de IA (junta ou uma pagina por IA).
void pages_ui_open();

// As telas abertas a partir de uma categoria (Paginas, Relogio, contas, agente do PC, diagnostico)
// desmontam a categoria para poupar o pool do LVGL. Ao fechar, cada uma chama isto DEPOIS de
// iniciar a propria exclusao: se veio de uma categoria, ela e remontada; se foi aberta de fora
// (engrenagem das paginas), nao faz nada e o voltar so fecha.
void settings_ui_child_closed();
