#pragma once

// Uso das IAs, como a interface ve. Placa: src/usage_esp32.cpp (task que consulta
// cada servico, uma requisicao HTTPS por vez). Simulador: sim/usage_sim.cpp.
//
// Cadeia de fontes (por enquanto so as duas primeiras existem):
//   Direto (a placa consulta o servico) -> Cache (ultimo valor bom, com idade)
//   -> Falha (sem valor nenhum). PC e Nuvem entram nos marcos 7 e 8.

#include <stdint.h>

enum Provider : uint8_t { PROV_CLAUDE, PROV_CODEX, PROV_ANTIGRAVITY, PROV_COUNT };

enum class UsageSource : uint8_t {
  NoAccount,  // sem credencial configurada
  Waiting,    // tem credencial, esperando Wi-Fi/relogio/primeira consulta
  Direct,     // valor fresco, consultado pela placa
  Cache,      // ultimo valor bom (a consulta mais recente falhou ou ainda nao rodou)
  Failed,     // consulta falhou e nao ha valor guardado
};

// Cada provedor mostra duas barras; o nome vem do dado (o plano free do Codex so tem
// uma janela de 30 dias, o Antigravity agrupa por familia de modelos).
struct UsageWindow {
  char name[8];          // "5h", "Semana", "Mes", "Claude", "Gemini"; "" = sem janela
  float pct;             // 0-100 usado; < 0 = desconhecido
  uint32_t reset_epoch;  // quando zera (0 = desconhecido)
};

struct ProviderUsage {
  UsageSource source;
  UsageWindow win[2];
  uint32_t updated_epoch;  // quando os valores mostrados foram obtidos (0 = nunca)
  char error[40];          // motivo da ultima falha ("" se a ultima consulta deu certo)
  uint16_t attempts;       // consultas concluidas desde o boot (muda a cada resultado)
};

void usage_begin();  // carrega credenciais/cache e inicia a consulta periodica
ProviderUsage usage_get(Provider p);
void usage_refresh_now();
// true entre o pedido de atualizacao e a resposta (ou falha) desse provedor.
bool usage_is_refreshing(Provider p);

bool usage_has_account(Provider p);
// Claude: token do `claude setup-token`. Antigravity: refresh token (1//...). Codex nao
// usa isto: entra pelo login por codigo abaixo.
bool usage_set_account(Provider p, const char *token);
void usage_forget_account(Provider p);

// Login do Codex por codigo (device code): a placa mostra um codigo, voce o digita em
// auth.openai.com/codex/device no celular, e ela recebe os tokens.
enum class LoginState : uint8_t { Idle, Requesting, Waiting, Exchanging, Done, Failed };
struct CodexLogin {
  LoginState state;
  char code[16];
  char url[40];
  char error[48];
  uint32_t seconds_left;  // validade do codigo
};
void codex_login_start();
void codex_login_cancel();
CodexLogin codex_login_status();
