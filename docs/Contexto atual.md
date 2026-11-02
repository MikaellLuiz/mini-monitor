---
tipo: contexto
data: 2026-10-31
tags: [tema/estado]
---

# Contexto atual

## Estado

- Marcos 1 a 7 prontos e validados na placa:
  - display e toque;
  - Wi-Fi com portal;
  - TLS;
  - uso de Claude, Codex e Antigravity;
  - Spotify;
  - agente do PC com métricas e atalhos.
- Novo design das telas aplicado (`src/theme.cpp`, página Relógio, Ajustes em
  lista, páginas configuráveis `pg_hide`/`ia_sep`).

## Em andamento

- [[2026-10-31 - Programa configurador Windows e Linux]]: decisão aceita; fase 1
  implementada e **ainda não testada na placa**:
  - firmware compila (flash em 88 %) e a tela nova foi conferida no simulador
    (`sim/scripts/pc_usb.txt`);
  - o lado do PC (`tools/pc_agent/usb_bridge.py`, comandos `setup`, `board`,
    `wifi`, `select`, `reset`...) passa em 15 testes contra a placa falsa
    (`tools/pc_agent/test_usb_bridge.py`), mais um teste ponta a ponta com o
    agente e a porta de controle;
  - falta gravar o firmware (BOOT + RST), rodar `agent.py setup` na placa real e
    conferir PIN, bloqueio, USB vivo/perdido e a troca para o Wi-Fi.
- Programa do PC (fase 1b): `tools/pc_agent/painel_app.py` (janela pywebview, bandeja, agente
  e inicialização com o sistema), empacotado em `dist/app/PainelCYD.exe` por `build_app.py`.
  26 testes passam contra a placa falsa. Testado na placa real em 2026-10-31: PIN criado,
  token salvo, sessão por USB e PC mostrado "via USB". Falta testar o bloqueio por PIN errado,
  a queda do USB para o Wi-Fi, o PC fixado e o Linux.
- Ainda adiado: HTTP de configuração na placa com o mesmo PIN e token; contas (fase 2).

## Pendências

- O repositório git foi criado em 2026-10-31 (`git init`), sem commit inicial.
- Instalar o agente na inicialização: `tools/pc_agent/install_startup.ps1`
  existe mas não foi executado.
- Marco 8: atualização de firmware pela rede (OTA) e relay.
- Atalhos sem PC por BLE HID.
- Fase de layout: fontes com acento e página web do portal Wi-Fi.
- Proteção contra toques repetidos no botão Atualizar.
