// Validacao HTTPS pelo pacote de CAs (data/cert/x509_crt_bundle.bin).
//
// Copia adaptada de libraries/WiFiClientSecure/src/esp_crt_bundle.c do
// Arduino-ESP32 2.0.x (Copyright 2018-2019 Espressif Systems, Apache License 2.0).
// Define as mesmas funcoes arduino_esp_crt_bundle_*, entao o linker usa esta versao
// e nao puxa a da biblioteca.
//
// Mudanca: a original so procurava no pacote o EMISSOR do certificado do topo da
// cadeia. Servidores do Google (api.anthropic.com, *.googleapis.com) mandam o
// "GTS Root R4" assinado em cruz pelo "GlobalSign Root CA", que a Mozilla removeu:
// o emissor nao esta no pacote, mas o proprio GTS Root R4 esta. Agora, se o
// certificado do topo e uma raiz do pacote (mesmo subject e mesma chave publica),
// ele e aceito. E o que o ESP-IDF 5 passou a fazer.

#include <esp32-hal-log.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"

#define BUNDLE_HEADER_OFFSET 2
#define CRT_HEADER_OFFSET 4

// Certificado vazio: so para o ponteiro de CA nao ser NULL no handshake.
static mbedtls_x509_crt s_dummy_crt;

static struct {
  const uint8_t **crts;
  uint16_t num_certs;
} s_bundle;

static size_t name_len(const uint8_t *crt) { return crt[0] << 8 | crt[1]; }
static size_t key_len(const uint8_t *crt) { return crt[2] << 8 | crt[3]; }
static const uint8_t *crt_name(const uint8_t *crt) { return crt + CRT_HEADER_OFFSET; }
static const uint8_t *crt_key(const uint8_t *crt) { return crt + CRT_HEADER_OFFSET + name_len(crt); }

// Busca binaria pelo subject DER (o pacote e gerado ordenado por esses bytes).
static const uint8_t *find_by_name(const uint8_t *name, size_t len) {
  int start = 0, end = s_bundle.num_certs - 1;
  while (start <= end) {
    int middle = (start + end) / 2;
    const uint8_t *crt = s_bundle.crts[middle];
    size_t nl = name_len(crt);
    int cmp = memcmp(name, crt_name(crt), len < nl ? len : nl);
    if (cmp == 0) cmp = (len > nl) - (len < nl);
    if (cmp == 0) return crt;
    if (cmp < 0) end = middle - 1;
    else start = middle + 1;
  }
  return NULL;
}

static int check_signature(mbedtls_x509_crt *child, const uint8_t *pub_key, size_t pub_key_len) {
  mbedtls_x509_crt parent;
  unsigned char hash[MBEDTLS_MD_MAX_SIZE];
  int ret;

  mbedtls_x509_crt_init(&parent);
  if ((ret = mbedtls_pk_parse_public_key(&parent.pk, pub_key, pub_key_len)) != 0) goto cleanup;
  if (!mbedtls_pk_can_do(&parent.pk, child->sig_pk)) {
    ret = -1;
    goto cleanup;
  }
  const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(child->sig_md);
  if ((ret = mbedtls_md(md_info, child->tbs.p, child->tbs.len, hash)) != 0) goto cleanup;
  ret = mbedtls_pk_verify_ext(child->sig_pk, child->sig_opts, &parent.pk, child->sig_md, hash,
                              mbedtls_md_get_size(md_info), child->sig.p, child->sig.len);
cleanup:
  mbedtls_x509_crt_free(&parent);
  return ret;
}

// Chamado para cada certificado da cadeia. Os intermediarios ja sao verificados
// pelo mbedTLS contra o pai; aqui so se resolve o primeiro elo "nao confiavel".
static int verify_callback(void *buf, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
  // Hash fraco numa raiz confiavel nao importa: a raiz ja e confiavel.
  uint32_t filtered = *flags & ~(MBEDTLS_X509_BADCERT_BAD_MD);
  if (filtered != MBEDTLS_X509_BADCERT_NOT_TRUSTED) return 0;
  if (s_bundle.crts == NULL) {
    printf("[x509] FATAL: pacote de CAs vazio (prof=%d)\n", depth);
    return MBEDTLS_ERR_X509_FATAL_ERROR;
  }

  // 1) O proprio certificado e uma raiz do pacote (raiz assinada em cruz).
  const uint8_t *self = find_by_name(crt->subject_raw.p, crt->subject_raw.len);
  if (self && key_len(self) == crt->pk_raw.len && !memcmp(crt_key(self), crt->pk_raw.p, crt->pk_raw.len)) {
    *flags = 0;
    return 0;
  }

  // 2) O emissor e uma raiz do pacote e a assinatura confere (comportamento original).
  const uint8_t *issuer = find_by_name(crt->issuer_raw.p, crt->issuer_raw.len);
  int sigRet = issuer ? check_signature(crt, crt_key(issuer), key_len(issuer)) : 1;
  if (issuer && sigRet == 0) {
    *flags = 0;
    return 0;
  }

  // Diagnostico (so na falha): separa "o certificado nao confere" de "faltou memoria na hora de verificar".
  // sigRet: 1 = emissor fora do pacote; negativo = codigo do mbedTLS (alloc: PK -0x3F80, MPI -0x0010,
  // ECP -0x4D80). Se o heap estiver folgado e o codigo nao for de alloc, a causa nao e memoria.
  // printf e nao log_e: o projeto compila com CORE_DEBUG_LEVEL=0 e log_e some do binario.
  printf("[x509] FATAL: certificado nao validado pelo pacote de CAs | prof=%d flags=0x%x emissor_no_pacote=%d sig=%d(-0x%04x) "
        "pk=%d sig_md=%d heap=%u maior_bloco=%u\n",
        depth, (unsigned)*flags, issuer != NULL, sigRet, (unsigned)(sigRet < 0 ? -sigRet : sigRet), (int)crt->sig_pk,
        (int)crt->sig_md, (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  return MBEDTLS_ERR_X509_FATAL_ERROR;
}

esp_err_t arduino_esp_crt_bundle_attach(void *conf) {
  if (s_bundle.crts == NULL) {
    log_e("pacote de CAs nao definido");
    return ESP_OK;
  }
  if (conf) {
    mbedtls_ssl_config *ssl_conf = (mbedtls_ssl_config *)conf;
    mbedtls_x509_crt_init(&s_dummy_crt);
    mbedtls_ssl_conf_ca_chain(ssl_conf, &s_dummy_crt, NULL);
    mbedtls_ssl_conf_verify(ssl_conf, verify_callback, NULL);
  }
  return ESP_OK;
}

void arduino_esp_crt_bundle_detach(mbedtls_ssl_config *conf) {
  free(s_bundle.crts);
  s_bundle.crts = NULL;
  if (conf) mbedtls_ssl_conf_verify(conf, NULL, NULL);
}

void arduino_esp_crt_bundle_set(const uint8_t *x509_bundle) {
  free(s_bundle.crts);
  s_bundle.num_certs = (x509_bundle[0] << 8) | x509_bundle[1];
  s_bundle.crts = calloc(s_bundle.num_certs, sizeof(*s_bundle.crts));
  if (s_bundle.crts == NULL) {
    log_e("sem memoria para o indice do pacote de CAs");
    return;
  }
  const uint8_t *cur = x509_bundle + BUNDLE_HEADER_OFFSET;
  for (int i = 0; i < s_bundle.num_certs; i++) {
    s_bundle.crts[i] = cur;
    cur += CRT_HEADER_OFFSET + name_len(cur) + key_len(cur);
  }
}
