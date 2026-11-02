"""Grava o firmware na CYD, que nao tem auto-reset funcional.

Uso: flash.py <porta> <firmware.bin>
Antes de rodar (ou durante a espera): segure BOOT, toque RST, solte o RST e
mantenha BOOT apertado ate a gravacao terminar.
"""
import os
import subprocess
import sys

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
rc = subprocess.call(cmd)
if rc == 0:
    print("\n>>> Gravado. Solte o BOOT; se a placa nao reiniciar, toque RST.")
sys.exit(rc)
