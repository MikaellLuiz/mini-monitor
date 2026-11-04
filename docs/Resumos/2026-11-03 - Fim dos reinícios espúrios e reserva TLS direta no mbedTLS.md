---
tipo: resumo
data: 2026-11-03
projetos: [mini-monitor]
tags: []
---

# Fim dos reinícios espúrios e reserva TLS direta no mbedTLS

Sessão no Linux (branch `claude/device-firmware-upload-e6e7ef` do [[Mini-monitor]], commit `c7367c3`), continuando a investigação de reinícios da placa CYD e falhas de memória nas consultas TLS dos agentes de IA.

## O que foi feito

**1. Quatro causas de reinício corrigidas (`src/https_client.cpp`, `src/wifi_esp32.cpp`, `src/usage_esp32.cpp`, `src/board_esp32.cpp`, `tools/pc_agent/usb_bridge.py`)**
- **Underflow de 1 ms entre núcleos (`4294967 s`)**: `reqSince = millis() | 1` no núcleo 0 avançava 1 ms quando `millis()` era par; o núcleo 1 lia `millis() - reqSince = 0xFFFFFFFF` (`4294967 s > STUCK_MS`) e disparava `[https] requisicao presa ha 4294967 s: reiniciando`. Trocado para `now ? now : 1` e comparação com sinal `(int32_t)(millis() - t)` em todos os vigias.
- **Vigia de 3 min de falha de memória após queda de Wi-Fi (`noteTls`)**: `memOnlyFrom` não zerava após longos períodos sem tentativas (ex.: Wi-Fi offline por minutos), então a 1ª falha de memória ao reconectar somava o tempo offline e reiniciava a placa na hora. Adicionado `MEM_GAP_RESET_MS = 60 s`.
- **Intercalação serial entre núcleos derrubando a ponte USB**: `board_log()` fazia `Serial.print(buf)` seguido de `Serial.println()` em duas chamadas separadas; quando o núcleo 0 logava no meio de uma resposta `@PC {...}` do núcleo 1, a linha chegava colada ao PC. `board_log()` passou a enviar `buf + "\r\n"` num único `Serial.write()`, e `Link._handle()` no `usb_bridge.py` separa qualquer `@PC ` deslocado no meio da linha.
- **Reabertura da porta em falha isolada de `shortcuts`**: no `usb_bridge.py`, `request("shortcuts")` estava fora do `try/except LinkError` que tolera `MAX_MISSES = 3`. Um único timeout reabria `/dev/ttyUSB0`, pulsando DTR/RTS e causando `POWERON_RESET`. Movido para dentro do bloco tolerante a falhas e reinstalado o binário `~/.local/bin/painel-cyd` (30 testes passando).

**2. Reserva TLS definitiva e folga de heap no handshake (`src/https_client.cpp`, `src/usage_esp32.cpp`, `src/spotify_esp32.cpp`)**
- Identificado por que o 2º bloco de `17.396 B` da reserva virava `16.372 B` após o primeiro handshake: o `mbedtls_calloc(1, 16717)` fazia o TLSF partir o bloco de `17.396 B` em `16.720 B` + uma sobra livre de `668 B`, que era ocupada por alocações pequenas durante a conexão e impedia a coalescência no `free()`.
- Implementado `mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree)` para entregar e recolher `reserve[0]` e `reserve[1]` diretamente no `mbedtls_ssl_setup`/`mbedtls_ssl_free` sem devolvê-los ao heap geral: ver [[2026-11-03 - Reserva TLS de dois blocos entregue direto ao mbedtls_calloc]].
- `String all` em `attempt()` passou a ser montada somente após o handshake TLS e destruída logo após `c.write()`, e as cópias temporárias de tokens (`token`, `access`, `account`, `refresh`, `project`) são liberadas antes de `https_send`, abrindo `~5,2 KB` extras durante a validação X509/RSA do `chatgpt.com`.
- `SO_LINGER = {1, 0}` em `stopNow()` para liberar o `tcp_pcb` do lwIP imediatamente ao fechar a conexão, e ticket de sessão TLS guardado em buffer estático `uint8_t ticket[384]` em `SessSlot`.

## Por quê

Mesmo sem reiniciar, a placa ficava com apenas um dos agentes atualizado e os demais presos em `Cache` porque o segundo bloco da reserva TLS encolhia de `17.396 B` para `16.372 B` logo na primeira consulta, e no `chatgpt.com` a validação RSA da cadeia de 3 certificados falhava no limite do heap (`MBEDTLS_ERR_MPI_ALLOC_FAILED`). O estado atualizado da arquitetura está em [[TLS e memória no Painel CYD]].

Decisão tomada nesta sessão:
- [[2026-11-03 - Reserva TLS de dois blocos entregue direto ao mbedtls_calloc]]

## Pendente

- Fazer merge da branch `claude/device-firmware-upload-e6e7ef` na branch principal quando desejar consolidar os commits.
