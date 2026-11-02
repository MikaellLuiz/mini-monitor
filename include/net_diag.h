#pragma once

// Diagnostico de rede: abre HTTPS (com validacao de certificado) para cada servico
// que o painel vai usar, sem enviar credencial nenhuma, e mede tempo e memoria.
// Responder 401/403 de API e "OK": o que importa e o TLS e o HTTP funcionarem.
// Placa: src/net_diag_esp32.cpp (task propria). Simulador: sim/net_diag_sim.cpp.

#include <stdint.h>

enum class DiagState : uint8_t { Pending, Running, Ok, Blocked, Failed };

struct NetDiagResult {
  char label[16];
  char host[40];
  DiagState state;
  int http_status;
  uint32_t handshake_ms;
  uint32_t total_ms;
  uint32_t tls_heap_bytes;  // heap consumido com a conexao aberta
  char detail[48];
};

struct NetDiagSummary {
  uint32_t tls_heap_max;     // maior consumo de uma conexao
  uint32_t heap_min_free;    // menor heap livre ja visto (desde o boot)
  uint32_t largest_block;    // maior bloco livre ao final
  uint32_t stack_free_min;   // folga minima da pilha da task (bytes)
};

void net_diag_start();  // ignora se ja estiver rodando
bool net_diag_running();
int net_diag_count();
bool net_diag_get(int idx, NetDiagResult *out);
NetDiagSummary net_diag_summary();
