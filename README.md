# Painel CYD

Painel de bolso numa ESP32-2432S028 ("Cheap Yellow Display" 2.8"): uso de IA,
Spotify, atalhos, status do PC. PlatformIO + Arduino + LVGL 8.3 + LovyanGFX.

Decisões, contexto atual e referências: [docs/Índice.md](docs/Índice.md).

## Placa

- ESP32-D0WD-V3, 4 MB de flash, **sem PSRAM**, ponte CH340 (COM5).
- Display ST7789 (sem inversao, RGB) em paisagem 320x240; toque XPT2046.
- **Sem auto-reset**: para gravar, segure BOOT, toque RST, solte o RST e mantenha
  BOOT ate a gravacao terminar.

## Comandos

Tudo pelo venv do projeto (`.venv`, criado com `python -m venv .venv` e
`pip install platformio esptool ziglang`).

```bash
.venv/Scripts/pio run                 # compila o firmware
.venv/Scripts/pio run -t upload       # grava (espera o modo download, ver acima)
.venv/Scripts/python tools/serial_log.py COM5 20   # le o serial sem resetar
```

## Simulador no PC

A mesma interface (`src/ui.cpp`, `quickpanel.cpp`, `wifi_ui.cpp`) roda numa janela
do Windows, compilada com o Zig do venv. Precisa de um `pio run` antes (usa o LVGL
baixado pelo PlatformIO).

```bash
.venv/Scripts/python tools/build_sim.py --run                          # janela (mouse = toque, P = screenshot)
.venv/Scripts/python tools/build_sim.py --script sim/scripts/tour.txt  # roteiro -> PNGs em sim_shots/
.venv/Scripts/python tools/build_sim.py --script sim/scripts/tour_ia_separada.txt --set ia_sep=1
.venv/Scripts/python tools/build_sim.py --script sim/scripts/tour.txt --set theme=1 --set bright=255   # tema claro
```

Memoria do LVGL: o pool (`LV_MEM_SIZE`, 60 KB, estatico) e o limite real; se encher, o assert
reinicia o painel. Nao aumente: 80 KB no heap deixou ~69 KB livres e quebrou o TLS ("SSL - Memory
allocation failed"), e estatico acima de ~62 KB nao cabe no link. Cada conexao TLS precisa de ~42 KB
de heap. Para medir sem a placa, compile em 32 bits (mesmo tamanho de ponteiro do ESP32) e rode o
roteiro de memoria, que imprime o uso em cada tela:

```bash
.venv/Scripts/python tools/build_sim.py --m32 --script sim/scripts/mem.txt [--set ia_sep=1]
```

Telas pesadas (Configuracoes, Paginas, opcoes do Relogio) checam `ui_mem_ok()` antes de abrir.
Componentes novos devem usar os estilos compartilhados de `theme.cpp` em vez de varios
`lv_obj_set_style_*` por objeto.

O simulador serve para layout, navegacao e textos. **Nao** reproduz velocidade do
ESP32, SPI, tearing nem o toque resistivo: FPS e CPU so valem medidos na placa.
O pool do LVGL no PC e maior (64 bits); o uso real aparece no serial da placa
(`[lvgl] mem`).

## Organizacao

- `include/board.h`, `include/wifi_mgr.h`: o que a interface pede ao hardware.
  Placa: `src/board_esp32.cpp`, `src/wifi_esp32.cpp`. PC: `sim/`.
- `src/ui.cpp`: paginas e troca por gesto. Quais paginas aparecem e o modo do uso de
  IA (junta ou uma pagina por IA) ficam na NVS (`pg_hide`, `ia_sep`) e valem no boot.
- `src/theme.cpp`: tema claro/escuro (NVS `theme`, vale no boot) e componentes comuns.
  Cores novas entram na lista `TH_TOKENS` de `theme.h`, com valor escuro e claro.
- `src/clock_ui.cpp`: Relogio com 4 estilos (Digital, Analogico, Minimo, Anel) e opcoes
  na NVS (`clk_*`) que valem na hora. Digitos grandes: `src/clock_font.c`, gerado por
  `tools/gen_clock_font.py` (Montserrat Medium 80 e 100 px, so "0123456789:").
- Toque nos cartoes (Uso de IA, Spotify, PC): com conta atualiza na hora, sem conta abre a
  configuracao. Conectar, trocar e remover contas fica so em Configuracoes.
- `src/quickpanel.cpp`: ajustes rapidos (gesto para baixo): brilho, tema, Wi-Fi, Paginas e
  Configuracoes. `src/settings_ui.cpp`: Configuracoes (contas, Relogio, Paginas, Tema,
  em tres categorias (Contas, Tela, Sistema; Sistema tem diagnostico, recalibrar, reiniciar e
  redefinir tudo, estes dois com confirmacao). Uma lista unica estourava o pool do LVGL.
- `src/wifi_ui.cpp`: tela Wi-Fi, redes salvas, portal com QR, barra de status.

## Painel CYD: o programa do PC (Windows e Linux)

Janela de configuracao, icone na bandeja e o agente (metricas e atalhos por USB e Wi-Fi) num
programa so, sem rodar script. O visual usa os mesmos tokens de cor do aparelho
(`ui/tokens.css` e gerado de `src/theme.h` por `tools/pc_agent/ui/gen_tokens.py`).

```bash
.venv/Scripts/python tools/pc_agent/build_app.py     # gera dist/app/PainelCYD.exe (Windows)
dist/app/PainelCYD.exe                               # abre a janela; fechar a janela deixa na bandeja
dist/app/PainelCYD.exe --minimized                   # so a bandeja (o que "Iniciar com o sistema" usa)
```

Sem empacotar: `pip install -r tools/pc_agent/requirements.txt` e `python tools/pc_agent/painel_app.py`.
No Linux o programa abre a interface no navegador padrao, com a bandeja; a janela pywebview
(`--window`) exige GTK ou Qt. Empacote no proprio Linux com `build_app.py` (o PyInstaller nao
compila para outro sistema). Atalhos no Linux: volume por `pactl`, midia por `playerctl`, demais
teclas por `xdotool` (so X11), bloqueio por `loginctl`.

Abas: **Placa** (PIN, Wi-Fi, brilho, reiniciar), **PCs** (qual PC a placa mostra, PCs autorizados,
pareamento por Wi-Fi), **Atalhos** (editor), **Sistema** (iniciar com o sistema, redefinir a placa).

### Linha de comando (opcional)

```bash
python tools/pc_agent/agent.py setup            # autoriza este PC na placa (cria ou pede o PIN)
python tools/pc_agent/agent.py board            # estado da placa
python tools/pc_agent/agent.py wifi Rede senha
python tools/pc_agent/agent.py select NOTEBOOK  # fixa o PC mostrado (auto volta ao automatico)
```

A lista completa esta em `python tools/pc_agent/agent.py --help`. Com o programa rodando, os
comandos passam por ele, porque so um processo abre a porta serial. **Feche o programa antes de
gravar o firmware** (`tools/flash.py` usa a mesma COM).

Testes, sem placa (usam `tools/pc_agent/fake_board.py`):

```bash
.venv/Scripts/python -m unittest tools/pc_agent/test_usb_bridge.py tools/pc_agent/test_app_api.py -v
```
