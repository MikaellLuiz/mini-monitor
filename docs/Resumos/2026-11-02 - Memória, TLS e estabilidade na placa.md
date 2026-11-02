---
tipo: resumo
data: 2026-11-02
tags: [tema/memoria, tema/firmware]
---

# Memória, TLS e estabilidade na placa

Sessão de testes na placa real (2026-11-01 e 2026-11-02) com o firmware da
[[2026-10-31 - Programa configurador Windows e Linux]] e as telas novas. O log
da placa foi lido pelo programa do PC, que grava tudo o que a UART imprime em
`%APPDATA%\painel-agent\board.log`.

## Medições que valem para as próximas decisões

- Cada conexão TLS pega **~41-42 KB** de heap (`[tls] ... a TLS pegou N B`).
- O Codex (`chatgpt.com`) é a conexão mais pesada: a verificação da cadeia
  falhava com `[x509] FATAL ... sig=-16 (-0x0010)` (alocação de bignum) quando
  sobravam ~5,9 KB de heap. Passou a funcionar com ~72 KB livres antes de
  conectar.
- `heap_caps_get_largest_free_block()` é **arredondado** pelo alocador TLSF:
  os valores do log (32756, 34804, 40948, 17396...) são sempre um degrau menos
  12 B. Não use como limiar exato; um bloco livre de 34.000 B aparece como
  32.756.
- Folga de pilha medida (`uxTaskGetStackHighWaterMark`, em bytes no ESP-IDF):
  `usage` pico ~7,1 KB com a TLS do chatgpt.com; `spotify` estoura com 4 KB
  quando renova o token (a TLS roda nessa task); `pcstep` pico ~1,4 KB, sem TLS.
- Pool do LVGL (60 KB fixos): com páginas sob demanda o boot usa 33-40 %
  (era 81 % com todas as páginas montadas e IA separada).

## O que causou reinícios e travamentos

| Sintoma no log | Causa | Correção |
| --- | --- | --- |
| `task_wdt: IDLE0 (CPU 0) ... CPU 0: usage` | TLS no núcleo 0 sem ceder CPU por > 5 s | watchdog das tasks em 15 s (`esp_task_wdt_init`) |
| `Interrupt wdt timeout` no meio de `[heapmap]` | `heap_caps_dump` imprimindo ~200 linhas com o heap travado | removido; nunca fazer dump do heap pela serial |
| `Stack canary watchpoint triggered (spotify)` | pilha reduzida sem medir o caminho com TLS | `spotify` 8 KB, `usage` 10 KB |
| Telas de Configurações que não abrem | pool do LVGL cheio com páginas e telas empilhadas | páginas sob demanda e Configurações sem empilhar |
| Atalhos e comandos do Spotify lentos | enviados pela mesma task das consultas de IA | atalho USB direto; tasks `pcstep` e `spotify` próprias |

## Estado do código ao fim da sessão

- Reserva de heap para a TLS: um bloco único de 34 KB, liberado só durante a
  conexão (`src/https_client.cpp`). A versão em dois blocos de 17 KB falhava
  quando os blocos não ficavam lado a lado.
- Buffers de desenho em 8 linhas por padrão; a tela Configurações > Tela >
  "Fluidez e transição" escolhe 8, 12 ou 16 linhas (vale após reiniciar) e a
  transição entre páginas (Deslizar, Esmaecer, Sem animação). Ainda não
  testada na placa.
- Resultado medido: Codex, Claude e Antigravity com `HTTP 200`, sem reinícios.

## Lições

- Medir antes de fixar limiar: os limiares de 33-34 KB vieram de números
  arredondados e travaram todas as conexões numa versão.
- Pilha se mede no pior caminho real (o que abre TLS), não no caminho comum.
- Instrumento de diagnóstico também pode derrubar a placa: nada longo dentro de
  seção crítica ou com o heap travado.

## Pendências

Estão em [[Contexto atual]].
