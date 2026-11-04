---
tipo: decisão
data: 2026-11-03
projetos: [mini-monitor]
status: aceita
tags: []
---

# Reserva TLS de dois blocos entregue direto ao mbedtls_calloc

## Contexto

No [[Mini-monitor]], cada conexão TLS do mbedTLS 2.28 (`mbedtls_ssl_setup`) aloca dois buffers (`in_buf` e `out_buf`) via `mbedtls_calloc(1, 16717)` (ver [[TLS e memória no Painel CYD]]). Para evitar que a fragmentação do heap deixasse a TLS sem blocos contíguos, o firmware reservava dois blocos de `17.396 B` (`RESERVE_BLOCK`) no boot (`https_reserve_init`), liberava ambos para o heap geral logo antes do handshake e tentava realocá-los no `https_unlock`.

Na prática, após o primeiro handshake completo (`api.anthropic.com` ou `chatgpt.com`), o segundo bloco quase sempre encolhia para `16.372 B` (`1/2 blocos`), fazendo as conexões seguintes falharem com `SSL - Memory allocation failed`:

1. No alocador TLSF do ESP-IDF (`fl = 14`, faixa `[16384 .. 32767]` dividida em 16 sub-bins de `1 KB`), `sl = 0` guarda blocos de `[16372 .. 17395]` e `sl = 1` guarda `[17396 .. 18419]`. Como o TLSF arredonda um pedido de `16.717 B` **para cima** até `sl = 1` (`17.396 B`), qualquer bloco livre menor que `17.396 B` é ignorado pelo `malloc(16717)`.
2. Quando o `mbedtls_ssl_setup` alocava `16.717 B` (arredondado para `16.720 B` com alinhamento) dentro do buraco recém-liberado de `17.396 B`, o TLSF **partia** o bloco em `16.720 B` alocados + uma **sobra livre de `668 B`** no heap geral.
3. Durante os 1 a 7 s do handshake e da leitura HTTP, qualquer alocação pequena de outra task (`wifi`, `lwIP`, `lvgl`, `usb_link`, `pcstep`) ou da própria cadeia X509 caía dentro dessa sobra de `668 B`. Ao fechar a TLS (`mbedtls_ssl_free`), o bloco de `16.720 B` não conseguia coalescer de volta para `17.396 B`, permanecendo em `sl = 0` (`16.372 B`), inutilizável tanto pela reserva quanto pelas próximas TLS.

## Decisão

Registrar um alocador customizado no mbedTLS via `mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree)` em `src/https_client.cpp` (`https_reserve_init`):

- Pedidos ao `mbedtls_calloc(n, size)` na faixa `[16000 .. 17396]` (exclusivos de `in_buf` e `out_buf` em `mbedtls_ssl_setup`, confirmado por desmontagem de `firmware.elf`) recebem `reserve[0]` e `reserve[1]` **diretamente**, zerados com `memset`, sem nunca passar pelo heap geral.
- Em `tlsFree(ptr)`, quando `ptr` pertence a `reserve[]`, o slot apenas marca `reserveUsed[i] = false`, mantendo os dois blocos de `17.396 B` retidos permanentemente entre conexões (salvo se o heap livre cair abaixo da válvula de emergência `VALVE_MIN_FREE = 16 KB`).
- Se a válvula tiver soltado algum bloco, o fallback de `tlsCalloc` aloca `RESERVE_BLOCK` inteiro (`17.396 B` em vez de `16.717 B`) para nunca gerar a sobra de `668 B` no TLSF e já readota o ponteiro em `reserve[]`.
- Além disso, o buffer `String all` da requisição HTTP passou a ser montado **após** o handshake TLS (quando os ~6–8 KB temporários da cadeia X509/RSA já foram liberados) e destruído imediatamente após `c.write()`, e as cópias temporárias dos tokens (~2 KB do JWT do Codex e ~1 KB do refresh token) são liberadas antes de `https_send`.

## Alternativas consideradas

- **Soltar `reserve[]` no heap geral imediatamente antes de `mbedtls_ssl_setup` e realocar no `https_unlock`** — descartada porque o próprio `malloc(16717)` do mbedTLS parte o bloco de `17.396 B` no TLSF e expõe a sobra de `668 B` às demais tasks durante toda a conexão.
- **Envolver `malloc`/`calloc` globalmente via `-Wl,--wrap`** — desnecessário: o SDK do Arduino-ESP32 já compila o mbedTLS com `MBEDTLS_PLATFORM_MEMORY` habilitado e expõe `mbedtls_platform_set_calloc_free` em `libmbedcrypto.a`.
- **Recompilar o SDK do Arduino-ESP32 com `MBEDTLS_SSL_IN_CONTENT_LEN` menor** — mantida apenas como opção futura caso seja necessário liberar RAM fixa para outros recursos; não resolve sozinha a disputa de fragmentos no heap geral.

## Consequências

- Os dois blocos de `17.396 B` nunca voltam ao heap geral nem sofrem split no TLSF: `reserva=2` permanece estável antes, durante e após todas as conexões TLS.
- O `contiguousOk()` passa a exigir apenas `4 KB` contíguos no heap geral quando os 2 blocos da reserva estão disponíveis (para estruturas de handshake e certificados), em vez de `16 KB`.
- Ganho de `~5,2 KB` de heap livre durante o handshake do `chatgpt.com` (que antes falhava na 1ª tentativa com `MBEDTLS_ERR_MPI_ALLOC_FAILED` na validação RSA da cadeia X509).
- Como `reserve[]` é compartilhado apenas por `in_buf`/`out_buf`, permanece obrigatório serializar uma conexão TLS por vez com `https_lock`/`https_unlock`.

## Revisar quando

Ao atualizar o core Arduino-ESP32 / ESP-IDF (verificar se `RESERVE_BLOCK = 17396` continua cobrindo `mbedtls_ssl_setup` e se `MBEDTLS_PLATFORM_MEMORY` segue habilitado).
