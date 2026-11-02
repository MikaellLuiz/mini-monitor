"""Grava o firmware na CYD, que nao tem auto-reset funcional.

Uso: flash.py <porta> <firmware.bin>
Antes de rodar (ou durante a espera): segure BOOT, toque RST, solte o RST e
mantenha BOOT apertado ate a gravacao terminar.
"""
import http.client
import json
import os
import subprocess
import sys
import time

def agent_usb(cmd, **fields):
    """Pede ao Painel CYD aberto que solte (ou retome) a porta serial. Sem ele rodando, nao faz nada."""
    cfg = os.path.join(os.environ.get("APPDATA") or os.path.join(os.path.expanduser("~"), ".config"),
                       "painel-agent", "config.json")
    try:
        with open(cfg, encoding="utf-8") as f:
            ctl = json.load(f).get("control_port", 8766)
        c = http.client.HTTPConnection("127.0.0.1", ctl, timeout=3)
        c.request("POST", "/board", json.dumps({"cmd": cmd, **fields}), {"X-Painel": "1"})
        c.getresponse().read()
        c.close()
        return True
    except (OSError, ValueError):
        return False


port, firmware = sys.argv[1], sys.argv[2]
build_dir = os.path.dirname(firmware)
boot_app0 = os.path.join(
    os.path.expanduser("~"), ".platformio", "packages",
    "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin")

print("\n>>> Segure BOOT, toque RST, solte o RST e mantenha BOOT apertado.\n", flush=True)

cmd = [
    sys.executable, "-m", "esptool",
    "--port", port, "--baud", "460800",
    "--before", "no-reset", "--after", "hard-reset",
    "--connect-attempts", "400",
    "write-flash", "--flash-mode", "dio", "--flash-freq", "40m", "--flash-size", "4MB",
    "0x1000", os.path.join(build_dir, "bootloader.bin"),
    "0x8000", os.path.join(build_dir, "partitions.bin"),
    "0xe000", boot_app0,
    "0x10000", firmware,
]
if agent_usb("pause_usb", seconds=180):
    print(">>> Painel CYD solto da porta; ele reconecta ao final.", flush=True)
    time.sleep(1)  # o agente fecha a porta antes de o esptool abrir
try:
    rc = subprocess.call(cmd)
finally:
    agent_usb("resume_usb")
if rc == 0:
    print("\n>>> Gravado. Solte o BOOT; se a placa nao reiniciar, toque RST.")
sys.exit(rc)
