---
tipo: referência
data: 2026-10-31
tags: [tema/configurador, tema/agente-pc]
---

# Projetos parecidos e sensores do PC

Pesquisa feita para [[2026-10-31 - Programa configurador Windows e Linux]].

## Projetos com tela de uso de IA

| Projeto | Programa do PC | Ligação com a placa | Contas de IA |
| --- | --- | --- | --- |
| [esp32-ai-monitor](https://github.com/tobymarks/esp32-ai-monitor) | Tauri 2 (Rust e React) no Windows, Swift no Mac; bandeja e autostart; sem Linux | só USB, protocolo de quadros com confirmação; **não alterna DTR/RTS, então abre sem reset**; atualiza o firmware com o crate `espflash` | não fala com as IAs: usa o CLI do CodexBar |
| [LimitBuddy](https://github.com/catorendal-a11y/limitbuddy) | Python em terminal, ZIP com Python embutido; autostart opcional pela chave Run, sem admin | só USB | Claude por ponte de statusline do Claude Code (`rate_limits.five_hour` e `seven_day`, edita `~/.claude/settings.json`); recusa copiar cookies e tokens |
| [Desk Duo](https://github.com/peerapach/desk-duo-monitor) | script Python acionado por hook do CodexBar | Wi-Fi, `POST /api/usage` com Bearer; portal na placa com token aleatório | a placa só guarda o próprio token; OTA pelo admin web com rollback |

Nenhum dos três combina USB e rede, tem janela no Linux ou mede o hardware do
PC.

## Projetos com métricas do PC, USB e vários PCs

Segunda rodada (resumo de leitura dos READMEs e de parte do código; não é uma
auditoria completa):

| Projeto | Ligação | Vários PCs | O que aproveitar |
| --- | --- | --- | --- |
| [cydrust](https://github.com/liohtml/cydrust) (mesma placa CYD) | USB **ou** Wi-Fi, escolhido na compilação (`--features wifi`); sem regra de prioridade | um PC por placa; para juntar vários há um agregador que une os dados antes da placa | placa dá o PC como offline após **6 s sem linha válida**, sem handshake; abre a porta com DTR/RTS desligados |
| [StatNode](https://github.com/Keralots/StatNode) | métricas só por Wi-Fi (UDP 4210); USB só para gravar e Improv | não documentado | companheiro em Python com pywebview, autostart `HKCU\...\Run` e `systemd --user`; mDNS "instável em algumas redes" |
| [ESP32-PC-Control-Deck](https://github.com/lepczynski-cloud/ESP32-PC-Control-Deck) | só USB, linhas JSON com prefixo `DD:` | não (uma porta serial por ponte) | a placa manda só o número do atalho e o PC procura a ação na própria config |
| [sysmo](https://zff.dev/dmpop/sysmo) | só USB, só Linux | não | temperatura por `/sys/class/hwmon` |
| Token Monitor ([CNX](https://www.cnx-software.com/2026/08/07/token-monitor-an-esp32-s3-desktop-display-that-tracks-ai-coding-assistant-usage/)) | Wi-Fi com mDNS e código de 6 dígitos na tela; USB para provisionar | não documentado | pareamento por código de 6 dígitos; OTA assinada |

Conclusões:

- Ninguém faz "USB tem prioridade, senão Wi-Fi" em tempo de execução, nem deixa
  escolher entre vários PCs. Essas duas regras foram desenhadas aqui (ver a
  decisão).
- O cydrust compila o token no binário, então quem tem o firmware o lê. Aqui o
  token fica na NVS.
- O cydrust reconhece o ACK por substring, e uma linha de log com o mesmo texto é
  tomada como ACK. Por isso o nosso protocolo só aceita linhas que **começam**
  com `@PC `.
- No Linux a porta serial exige o usuário no grupo `dialout`.

## Credenciais

- O [CodexBar](https://github.com/steipete/CodexBar) lê `~/.codex/auth.json` e
  `~/.claude/.credentials.json` só para consultar e nunca grava tokens
  renovados. Usar o refresh token em outro lugar desloga o CLI
  ([issue #1161](https://github.com/steipete/CodexBar/issues/1161)).

## Configurar sem reset

- O padrão **Improv Serial** é usado pelo ESP Web Tools e pelo Home Assistant
  para configurar o Wi-Fi por USB.
- A biblioteca [improv-wifi-busware](https://github.com/tostmann/improv-wifi-busware)
  divide a porta serial com o resto do firmware.
- A abertura da porta sem reset já foi confirmada nesta placa por
  `tools/serial_log.py`.

## Sensores

- **Windows:** o driver WinRing0 é sinalizado pelo Defender como vulnerável
  ([aviso da Microsoft](https://support.microsoft.com/en-us/windows/security/threat-malware-protection/microsoft-defender-antivirus-alert-vulnerabledriver-winnt-winring0)).
  - O LibreHardwareMonitor trocou o WinRing0 pelo PawnIO na versão 0.9.6
    ([PR #1857](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/pull/1857)).
    O PawnIO exige uma instalação com administrador.
  - O LibreHardwareMonitor serve um `data.json` na porta 8085, usado por
    [Glances](https://github.com/nicolargo/glances/pull/3613) e
    [yasb](https://github.com/amnweb/yasb/wiki/(Widget)-Libre-HW-Monitor).
- **Linux:**
  - `/sys/class/hwmon`: `k10temp` na AMD, `coretemp` na Intel.
  - `amdgpu`: `gpu_busy_percent`.
  - NVML para NVIDIA. Nenhum deles exige driver extra.
- O [turing-smart-screen-python](https://github.com/mathoudebine/turing-smart-screen-python/wiki/System-monitor-:-hardware-support)
  usa essa mesma divisão: LibreHardwareMonitor no Windows e bibliotecas Python
  no Linux.
