#!/usr/bin/env bash
# Instala o Painel CYD no Linux (sem root): compila o executavel e cria o atalho no menu.
#   tools/pc_agent/install_linux.sh             compila e instala
#   tools/pc_agent/install_linux.sh --uninstall remove tudo (inclusive o autostart)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$HOME/.local/bin/painel-cyd"
ICON="$HOME/.local/share/icons/painel-cyd.png"
DESK="$HOME/.local/share/applications/painel-cyd.desktop"
AUTO="${XDG_CONFIG_HOME:-$HOME/.config}/autostart/painel-cyd.desktop"

if [ "${1:-}" = "--uninstall" ]; then
  rm -f "$BIN" "$ICON" "$DESK" "$AUTO"
  echo "Removido. A configuracao (~/.config/painel-agent) foi mantida."
  exit 0
fi

id -nG | grep -qw dialout || echo "AVISO: seu usuario nao esta no grupo dialout (USB). Rode: sudo usermod -aG dialout $USER e reabra a sessao."

cd "$ROOT"
[ -d .venv ] || python3 -m venv .venv
.venv/bin/pip install -q -r tools/pc_agent/requirements.txt pillow pystray pywebview pyinstaller
.venv/bin/python tools/pc_agent/build_app.py

mkdir -p "$(dirname "$BIN")" "$(dirname "$ICON")" "$(dirname "$DESK")"
install -m 755 dist/app/PainelCYD "$BIN"
install -m 644 .build_app/icon.png "$ICON"
cat > "$DESK" <<D
[Desktop Entry]
Type=Application
Name=Painel CYD
Comment=Agente e configurador do Painel CYD
Exec=$BIN
Icon=$ICON
Terminal=false
Categories=Utility;
D
echo "Instalado: $BIN (procure 'Painel CYD' no menu). Autostart: chave dentro do programa."
