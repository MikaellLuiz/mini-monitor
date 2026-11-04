#pragma once

// Controle do Spotify, como a interface ve. Placa: src/spotify_esp32.cpp (consulta o
// player pela Web API e envia comandos, sempre dentro da task de uso, uma conexao
// HTTPS por vez). Simulador: sim/spotify_sim.cpp.
//
// Login: o PC faz o PKCE (tools/spotify_login.py) e entrega Client ID + refresh token
// ao painel; a placa so renova o access token. Controle remoto: o Spotify toca no
// dispositivo que ja esta ativo (celular, PC); a placa nao toca audio.

#include <stdint.h>

static constexpr int SPOTIFY_COVER_PX = 64;

enum class SpotifyCmd : uint8_t { None, PlayPause, Next, Prev };

struct SpotifyState {
  bool linked;     // ha Client ID + refresh token
  bool active;     // ha um dispositivo ativo (o player respondeu 200)
  bool playing;
  char title[64];  // ja convertido para ASCII (as fontes embutidas nao tem acentos)
  char artist[48];
  char device[28];
  uint32_t progress_ms;  // no momento de `fetched_at_ms`
  uint32_t duration_ms;
  uint32_t fetched_at_ms;  // board_millis() da leitura (para interpolar o progresso)
  uint16_t cover_seq;      // muda quando a capa mudou
  char error[40];          // ultima falha ("" se ok)
  bool busy;               // comando enviado, esperando a resposta
};

SpotifyState spotify_get();
// RGB565 ja na ordem de bytes do LVGL (LV_COLOR_16_SWAP): 64x64, ou nullptr sem capa.
const uint16_t *spotify_cover();

void spotify_command(SpotifyCmd c);
// A pagina avisa se esta na tela: so consulta o player enquanto visivel.
void spotify_set_visible(bool visible);
// Consulta o player agora, sem esperar o intervalo (toque na faixa).
void spotify_refresh_now();

bool spotify_set_account(const char *client_id, const char *refresh_token);
void spotify_forget_account();

void spotify_begin();  // chamar uma vez, antes da interface
void spotify_step();   // chamado pela task "spotify" (a cada segundo ou ao receber um comando)
// true quando a busca inicial do token terminou (ok ou falha) ou a conta nao esta ligada.
bool spotify_boot_done();
