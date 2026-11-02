#pragma once

// Autorizacao dos programas do PC (USB e rede): PIN de 6 digitos criado uma vez e um
// token por PC. So guarda hashes (SHA-256 com sal); o PIN e os tokens nao ficam em
// texto na NVS. Placa: src/auth_esp32.cpp.
//
// Tentativas erradas: o contador fica na NVS e sobrevive a reinicio. Da 5a falha em
// diante vem um bloqueio crescente (30 s, 1 min, 2 min... ate 1 h). Nunca apaga nada
// sozinho: quem estivesse na rede poderia apagar as contas de proposito.

#include <stdbool.h>
#include <stdint.h>

static constexpr int AUTH_MAX_CLIENTS = 6;
static constexpr int AUTH_NAME_MAX = 24;    // com o \0
static constexpr int AUTH_TOKEN_CHARS = 32; // hexadecimal, sem o \0

enum class AuthResult : uint8_t { Ok, Wrong, Locked, Invalid };

void auth_begin();
bool auth_has_pin();

// Cria o PIN. false se ja existe um ou se nao sao exatamente 6 digitos. Trocar o PIN
// so apagando tudo (board_factory_reset).
bool auth_create_pin(const char *pin);

// Confere o PIN. Locked: retry_s diz quanto falta. Wrong: errou e o contador subiu.
AuthResult auth_check_pin(const char *pin, uint32_t *retry_s);
uint32_t auth_lock_remaining_s();

// Registra um PC e devolve o token dele (AUTH_TOKEN_CHARS + \0). Mesmo nome substitui.
// false se o nome e invalido ou se os AUTH_MAX_CLIENTS lugares estao ocupados.
bool auth_issue_client(const char *name, char *token_out);
// Confere o token; devolve o nome do PC.
bool auth_check_token(const char *token, char *name_out, int name_cap);

int auth_client_count();
bool auth_client_name(int i, char *out, int cap);
bool auth_revoke_client(const char *name);
