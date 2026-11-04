---
tipo: índice
---

# Índice

Porta de entrada do contexto do Painel CYD. Comandos e organização do código
estão no [README](../README.md).

- [[Contexto atual]]: estado, pendências e próximo passo.

## Decisões

- [[2026-11-03 - Reserva TLS de dois blocos entregue direto ao mbedtls_calloc]]: entrega reserve[0..1] direto ao mbedtls_ssl_setup para eliminar o split de 668 B do TLSF
- [[2026-10-31 - Programa configurador Windows e Linux]] (aceita; fases 1 e 1b feitas; contas no programa do PC pendentes)

## Resumos

- [[2026-11-04 - Redesenho da interface, unificação de IA e limpeza de telemetria]]: redesenho dos cards de IA, novos temas, remoção do contador de FPS e validação de memória
- [[2026-11-03 - Fim dos reinícios espúrios e reserva TLS direta no mbedTLS]]: correção dos 4 gatilhos de reset da placa CYD, alocador direto da reserva TLS no mbedTLS e +5,2 KB livres no handshake do Codex
- [[2026-11-02 - Memória, TLS e estabilidade na placa]]: medições de heap,
  pilha e TLS, causas dos reinícios e o estado ao fim dos testes.

## Referências

- [[2026-10-31 - Projetos parecidos e sensores do PC]]
