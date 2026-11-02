"""Empacota o Painel CYD num executavel unico com o PyInstaller (sem Python na maquina de destino).

    python tools/pc_agent/build_app.py          -> dist/app/PainelCYD.exe   (Windows)
                                                  dist/app/PainelCYD       (Linux; instale com tools/pc_agent/install_linux.sh)

Roda no sistema de destino: o PyInstaller nao compila para outro sistema.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BUILD = os.path.join(ROOT, ".build_app")
sys.path.insert(0, HERE)


def make_icon(path):
    from painel_app import tray_image
    img = tray_image().resize((256, 256))
    img.save(path, sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    img.save(os.path.join(os.path.dirname(path), "icon.png"))  # o instalador do Linux usa este


def main():
    os.makedirs(BUILD, exist_ok=True)
    icon = os.path.join(BUILD, "icon.ico")
    make_icon(icon)
    sep = ";" if sys.platform == "win32" else ":"
    cmd = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile", "--windowed",
           "--name", "PainelCYD", "--icon", icon,
           "--distpath", os.path.join(ROOT, "dist", "app"), "--workpath", os.path.join(BUILD, "work"),
           "--specpath", BUILD, "--paths", HERE,
           "--add-data", f"{os.path.join(HERE, 'ui')}{sep}ui",
           "--hidden-import", "serial.tools.list_ports_windows" if sys.platform == "win32" else "serial.tools.list_ports_linux",
           "--hidden-import", "zeroconf",
           "--collect-submodules", "webview", "--collect-submodules", "pystray",
           os.path.join(HERE, "painel_app.py")]
    sys.exit(subprocess.call(cmd))


if __name__ == "__main__":
    main()
