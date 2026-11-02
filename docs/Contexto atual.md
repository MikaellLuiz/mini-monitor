---
tipo: contexto
data: 2026-11-02
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

## Estado em 2026-11-02

Feito e testado na placa:

- PIN, USB e agente do PC funcionando.
- Codex, Claude e Antigravity com HTTP 200, depois de:
  - reserva de heap para a TLS em bloco único;
  - pilhas medidas (usage 10 KB, spotify 8 KB, pcstep 3,5 KB);
  - `BUF_LINES` 8;
  - watchdog das tasks em 15 s.
- O `maior_bloco` do heap é arredondado pelo alocador TLSF (32756 = 32768 - 12 etc.):
  não usar como limiar exato.

Gravado mas **não testado na placa** (ela não estava conectada para gravar):

- Configuracoes > Tela > "Fluidez e transicao" (commit a9fea01).

Pendente:

- Testar na placa a TLS do Spotify com a pilha de 8 KB.
- Fluidez com 12/16 linhas e o Codex com 16.
- Latência dos atalhos e comandos do Spotify (log "toque->envio").
- Fase 2 (contas no programa do PC).
- HTTP de configuração com PIN.
- Reduzir buffers do mbedTLS se a memória voltar a faltar.

### Como continuar em outro PC (Windows)

1. Clonar o repositório e criar o ambiente:
   `python -m venv .venv` e
   `.venv/Scripts/pip install platformio esptool ziglang pyserial psutil zeroconf pywebview pystray pyinstaller`.
2. Compilar: `.venv/Scripts/pio run`.
3. Gravar: a porta COM muda de PC para PC (`platformio.ini` tem `upload_port = COM5`), então
   `.venv/Scripts/pio run -t upload --upload-port COMx`, segurando BOOT, tocando RST e mantendo
   BOOT até o fim. Fechar o `PainelCYD.exe` antes de gravar.
4. O programa do PC é gerado com `python tools/pc_agent/build_app.py` (`dist/` não vai para o git).
5. Contas e PIN ficam na placa; o token do PC fica em `%APPDATA%\painel-agent\config.json`
   (fora do git). No outro PC o PIN será pedido uma vez.

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

- Instalar o agente na inicialização: `tools/pc_agent/install_startup.ps1`
  existe mas não foi executado.
- Marco 8: atualização de firmware pela rede (OTA) e relay.
- Atalhos sem PC por BLE HID.
- Fase de layout: fontes com acento e página web do portal Wi-Fi.
- Proteção contra toques repetidos no botão Atualizar.
