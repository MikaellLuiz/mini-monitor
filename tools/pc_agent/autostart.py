"""Iniciar o Painel CYD com o sistema, sem administrador e desligado por padrao.

Windows: valor por usuario em HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run.
Linux:   ~/.config/autostart/painel-cyd.desktop (vale em GNOME, KDE, XFCE...; o icone da
         bandeja precisa da sessao grafica, entao nao serve um servico systemd).
"""
import os
import shlex
import sys

NAME = "PainelCYD"
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
DESKTOP = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "autostart", "painel-cyd.desktop")


def command():
    """Como o sistema deve abrir o app: o executavel empacotado, ou o Python com este script."""
    if getattr(sys, "frozen", False):
        return [sys.executable, "--minimized"]
    py = sys.executable
    if sys.platform == "win32":  # pythonw: sem janela de console
        pyw = os.path.join(os.path.dirname(py), "pythonw.exe")
        py = pyw if os.path.exists(pyw) else py
    return [py, os.path.join(os.path.dirname(os.path.abspath(__file__)), "painel_app.py"), "--minimized"]


def _windows_line(cmd):
    return " ".join(f'"{c}"' if " " in c else c for c in cmd)


def is_enabled():
    if sys.platform == "win32":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
                winreg.QueryValueEx(k, NAME)
            return True
        except OSError:
            return False
    return os.path.exists(DESKTOP)


def enable():
    cmd = command()
    if sys.platform == "win32":
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
            winreg.SetValueEx(k, NAME, 0, winreg.REG_SZ, _windows_line(cmd))
        return
    os.makedirs(os.path.dirname(DESKTOP), exist_ok=True)
    with open(DESKTOP, "w", encoding="utf-8") as f:
        f.write("[Desktop Entry]\nType=Application\nName=Painel CYD\nComment=Agente e configurador do Painel CYD\n"
                f"Exec={' '.join(shlex.quote(c) for c in cmd)}\nTerminal=false\nX-GNOME-Autostart-enabled=true\n")


def disable():
    if sys.platform == "win32":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
                winreg.DeleteValue(k, NAME)
        except OSError:
            pass
        return
    try:
        os.remove(DESKTOP)
    except OSError:
        pass


def set_enabled(on):
    enable() if on else disable()
    return is_enabled()
