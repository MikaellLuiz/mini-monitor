#pragma once

// HTTPS da placa: uma conexao por vez (cada uma reserva ~42 KB de heap), com os
// certificados validados pelo pacote de CAs. Bloqueia por segundos: chamar so de
// tasks proprias, nunca do loop do LVGL.

#include <Arduino.h>
#include <functional>

struct HttpsHeader {
  const char *name;  // em minusculas
  String value;      // preenchido se a resposta trouxer o header
};

struct HttpsRequest {
  const char *host;
  const char *method;
  const char *path;
  String headers;  // linhas extras "Nome: valor\r\n"
  String body;
  HttpsHeader *want = nullptr;  // headers da resposta a capturar
  int want_count = 0;
  size_t max_body = 512;
  // HTTP/1.0 evita resposta "chunked" (o corpo chega sem tamanhos de bloco no meio),
  // necessario para ler JSON em fluxo.
  bool http10 = false;
  // Se definido, recebe o corpo como Stream (ex.: ArduinoJson com filtro) em vez de
  // guarda-lo numa String. Roda com a conexao aberta.
  std::function<void(Stream &)> onBody;
};

struct HttpsResult {
  int status = 0;  // 0 = sem resposta HTTP
  String error;    // falha de conexao/TLS
  String body;     // ate max_body bytes
  String statusLine;  // primeira linha crua da resposta (diagnostico)
  uint32_t handshake_ms = 0;
  uint32_t total_ms = 0;
  // A falha foi X509 FATAL ou "Memory allocation failed": repetir nao muda o resultado (so segura a
  // trava). Quem consulta deve adiar a proxima tentativa (ver MEMFAIL_RETRY_MS em usage_esp32.cpp).
  bool memOrCertFail = false;
};

// Prioridade interativa: a task que chama https_mark_interactive_task() (a do Spotify) e a unica que
// nao cede. Enquanto https_interactive(true), https_send de qualquer OUTRA task desiste das
// retentativas que faltam e devolve a trava para o comando entrar na frente.
// true (e limpa) se o ultimo https_send de uma task nao interativa terminou em X509 FATAL ou falta de
// memoria: a task de uso usa isto para adiar a proxima consulta do provedor.
bool https_take_memcert_fail();
void https_mark_interactive_task();
void https_interactive(bool on);

// Exclusao mutua para quem abre conexoes TLS por conta propria (ex.: diagnostico).
void https_lock();
void https_unlock();

// Auditoria de heap: loga "[heap] passo: livre, maior bloco, delta desde o ultimo passo". Qualquer task.
void heap_mark(const char *step);
// Fragmentacao: numero de blocos livres e o maior bloco livre (so leitura das estatisticas).
void https_heap_frag(unsigned *freeBlocks, unsigned *largest);

// Lastro de heap para a TLS (ver https_client.cpp). init: uma vez no boot, com o heap inteiro.
// check: a cada ~1 s no loop; solta o lastro se o heap livre ficar baixo. Nao bloqueia.
void https_reserve_init();
void https_reserve_check();
int https_reserve_held();  // 1 se o bloco unico do lastro esta em maos, 0 se nao

bool https_send(HttpsRequest &rq, HttpsResult &rs);

extern const uint8_t ca_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");
