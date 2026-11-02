// PIN e tokens dos programas do PC (ver include/auth.h).

#include <Arduino.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <string.h>

#include "auth.h"
#include "board.h"

static const char *NVS_NS = "auth";
static constexpr int PIN_LEN = 6;
static constexpr int HASH_LEN = 32;
static constexpr int HASH_ROUNDS = 5000;
static constexpr int FREE_TRIES = 5;       // erros sem bloqueio
static constexpr uint32_t MAX_LOCK_S = 3600;

static bool havePin = false;
static uint8_t pinSalt[16], pinHash[HASH_LEN];
static uint16_t fails = 0;
static uint32_t lockUntil = 0;  // millis(); 0 = sem bloqueio

struct AuthClient {
  char name[AUTH_NAME_MAX];
  uint8_t hash[HASH_LEN];
};
static AuthClient clients[AUTH_MAX_CLIENTS];
static int nClients = 0;

// ---- hash --------------------------------------------------------------------

static void sha256(const uint8_t *a, size_t an, const uint8_t *b, size_t bn, const uint8_t *c, size_t cn,
                   uint8_t out[HASH_LEN]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  if (an) mbedtls_sha256_update_ret(&ctx, a, an);
  if (bn) mbedtls_sha256_update_ret(&ctx, b, bn);
  if (cn) mbedtls_sha256_update_ret(&ctx, c, cn);
  mbedtls_sha256_finish_ret(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

// SHA-256 iterado com sal: 6 digitos sao poucos, entao cada tentativa tem de custar.
static void hashPin(const uint8_t salt[16], const char *pin, uint8_t out[HASH_LEN]) {
  sha256(salt, 16, (const uint8_t *)pin, PIN_LEN, nullptr, 0, out);
  uint8_t tmp[HASH_LEN];
  for (int i = 0; i < HASH_ROUNDS; i++) {
    sha256(out, HASH_LEN, salt, 16, (const uint8_t *)pin, PIN_LEN, tmp);
    memcpy(out, tmp, HASH_LEN);
  }
}

static bool sameBytes(const uint8_t *a, const uint8_t *b, int n) {
  uint8_t diff = 0;
  for (int i = 0; i < n; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

static bool validPin(const char *pin) {
  if (!pin || strlen(pin) != PIN_LEN) return false;
  for (int i = 0; i < PIN_LEN; i++)
    if (pin[i] < '0' || pin[i] > '9') return false;
  return true;
}

static uint32_t penaltyS(uint16_t f) {
  if (f < FREE_TRIES) return 0;
  uint32_t s = 30u << min<int>(f - FREE_TRIES, 7);
  return s > MAX_LOCK_S ? MAX_LOCK_S : s;
}

// ---- NVS ---------------------------------------------------------------------

static void saveClients() {
  Preferences p;
  p.begin(NVS_NS, false);
  p.putUChar("nc", nClients);
  for (int i = 0; i < AUTH_MAX_CLIENTS; i++) {
    char k[6];
    snprintf(k, sizeof(k), "cn%d", i);
    if (i < nClients) {
      p.putString(k, clients[i].name);
      snprintf(k, sizeof(k), "ch%d", i);
      p.putBytes(k, clients[i].hash, HASH_LEN);
    } else {
      p.remove(k);
      snprintf(k, sizeof(k), "ch%d", i);
      p.remove(k);
    }
  }
  p.end();
}

static void saveFails() {
  Preferences p;
  p.begin(NVS_NS, false);
  p.putUShort("fails", fails);
  p.end();
}

void auth_begin() {
  Preferences p;
  p.begin(NVS_NS, true);
  havePin = p.getBytesLength("hash") == HASH_LEN && p.getBytesLength("salt") == 16;
  if (havePin) {
    p.getBytes("salt", pinSalt, 16);
    p.getBytes("hash", pinHash, HASH_LEN);
  }
  fails = p.getUShort("fails", 0);
  nClients = min<int>(p.getUChar("nc", 0), AUTH_MAX_CLIENTS);
  for (int i = 0; i < nClients; i++) {
    char k[6];
    snprintf(k, sizeof(k), "cn%d", i);
    String n = p.getString(k, "");
    snprintf(clients[i].name, sizeof(clients[i].name), "%s", n.c_str());
    snprintf(k, sizeof(k), "ch%d", i);
    if (p.getBytes(k, clients[i].hash, HASH_LEN) != HASH_LEN) memset(clients[i].hash, 0, HASH_LEN);
  }
  p.end();
  // Reiniciar nao pula o bloqueio: quem passou das tentativas livres espera de novo.
  uint32_t pen = penaltyS(fails);
  lockUntil = pen ? millis() + pen * 1000 : 0;
  board_log("auth: PIN %s, %d PC(s) autorizado(s), %u falha(s)", havePin ? "definido" : "nao definido", nClients,
            (unsigned)fails);
}

// ---- PIN ---------------------------------------------------------------------

bool auth_has_pin() { return havePin; }

bool auth_create_pin(const char *pin) {
  if (havePin || !validPin(pin)) return false;
  for (int i = 0; i < 16; i++) pinSalt[i] = (uint8_t)esp_random();
  hashPin(pinSalt, pin, pinHash);
  Preferences p;
  p.begin(NVS_NS, false);
  p.putBytes("salt", pinSalt, 16);
  p.putBytes("hash", pinHash, HASH_LEN);
  p.putUShort("fails", 0);
  p.end();
  havePin = true;
  fails = 0;
  lockUntil = 0;
  board_log("auth: PIN criado");
  return true;
}

uint32_t auth_lock_remaining_s() {
  if (!lockUntil) return 0;
  int32_t left = (int32_t)(lockUntil - millis());
  if (left <= 0) return 0;
  return (uint32_t)((left + 999) / 1000);
}

AuthResult auth_check_pin(const char *pin, uint32_t *retry_s) {
  if (retry_s) *retry_s = 0;
  if (!havePin || !validPin(pin)) return AuthResult::Invalid;
  uint32_t left = auth_lock_remaining_s();
  if (left) {
    if (retry_s) *retry_s = left;
    return AuthResult::Locked;
  }
  uint8_t h[HASH_LEN];
  hashPin(pinSalt, pin, h);
  if (sameBytes(h, pinHash, HASH_LEN)) {
    if (fails) {
      fails = 0;
      saveFails();
    }
    lockUntil = 0;
    return AuthResult::Ok;
  }
  fails++;
  saveFails();  // antes de responder: cortar a energia nao devolve a tentativa
  uint32_t pen = penaltyS(fails);
  lockUntil = pen ? millis() + pen * 1000 : 0;
  if (retry_s) *retry_s = pen;
  board_log("auth: PIN errado (%u), bloqueio de %u s", (unsigned)fails, (unsigned)pen);
  return AuthResult::Wrong;
}

// ---- tokens por PC -----------------------------------------------------------

static bool validName(const char *name) {
  size_t n = name ? strlen(name) : 0;
  if (n == 0 || n >= AUTH_NAME_MAX) return false;
  for (size_t i = 0; i < n; i++)
    if ((uint8_t)name[i] < 32 || (uint8_t)name[i] >= 127) return false;
  return true;
}

static void tokenHash(const char *token, uint8_t out[HASH_LEN]) {
  sha256((const uint8_t *)token, strlen(token), nullptr, 0, nullptr, 0, out);
}

bool auth_issue_client(const char *name, char *token_out) {
  if (!validName(name)) return false;
  int slot = -1;
  for (int i = 0; i < nClients; i++)
    if (!strcmp(clients[i].name, name)) slot = i;
  if (slot < 0) {
    if (nClients >= AUTH_MAX_CLIENTS) return false;
    slot = nClients++;
  }
  static const char HEX_DIGITS[] = "0123456789abcdef";
  for (int i = 0; i < AUTH_TOKEN_CHARS; i += 8) {
    uint32_t r = esp_random();
    for (int j = 0; j < 8; j++) token_out[i + j] = HEX_DIGITS[(r >> (4 * j)) & 0xF];
  }
  token_out[AUTH_TOKEN_CHARS] = 0;
  snprintf(clients[slot].name, sizeof(clients[slot].name), "%s", name);
  tokenHash(token_out, clients[slot].hash);
  saveClients();
  board_log("auth: PC autorizado: %s", name);
  return true;
}

bool auth_check_token(const char *token, char *name_out, int name_cap) {
  if (!token || strlen(token) != AUTH_TOKEN_CHARS) return false;
  uint8_t h[HASH_LEN];
  tokenHash(token, h);
  int found = -1;
  for (int i = 0; i < nClients; i++)  // sem parar no primeiro: tempo igual
    if (sameBytes(h, clients[i].hash, HASH_LEN)) found = i;
  if (found < 0) return false;
  if (name_out && name_cap > 0) snprintf(name_out, name_cap, "%s", clients[found].name);
  return true;
}

int auth_client_count() { return nClients; }

bool auth_client_name(int i, char *out, int cap) {
  if (i < 0 || i >= nClients) return false;
  snprintf(out, cap, "%s", clients[i].name);
  return true;
}

bool auth_revoke_client(const char *name) {
  for (int i = 0; i < nClients; i++) {
    if (!name || strcmp(clients[i].name, name)) continue;
    for (int j = i; j < nClients - 1; j++) clients[j] = clients[j + 1];
    nClients--;
    saveClients();
    board_log("auth: PC revogado: %s", name);
    return true;
  }
  return false;
}
