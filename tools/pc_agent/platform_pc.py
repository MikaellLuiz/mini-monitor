"""O que muda entre Windows e Linux no agente: pasta de configuracao, atalhos, teclas, bloqueio e GPU.

Windows: teclas por keybd_event, bloqueio por LockWorkStation, GPU por nvidia-smi.
Linux:   volume por pactl, midia por playerctl, outras teclas por xdotool (so X11),
         bloqueio por loginctl, GPU por nvidia-smi ou pelo amdgpu em /sys.
"""
import ctypes
import glob
import os
import re
import shlex
import shutil
import subprocess
import sys

IS_WINDOWS = sys.platform == "win32"
NO_WINDOW = 0x08000000 if IS_WINDOWS else 0  # CREATE_NO_WINDOW: sem janela de console piscando


def config_dir():
    if IS_WINDOWS:
        return os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")), "painel-agent")
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "painel-agent")


if IS_WINDOWS:
    DEFAULT_SHORTCUTS = [
        {"label": "Mudo", "keys": "volume_mute"},
        {"label": "Vol -", "keys": "volume_down"},
        {"label": "Vol +", "keys": "volume_up"},
        {"label": "Print", "keys": "win+shift+s"},
        {"label": "Tarefas", "keys": "ctrl+shift+esc"},
        {"label": "Bloquear", "action": "lock"},
    ]
else:
    DEFAULT_SHORTCUTS = [
        {"label": "Mudo", "keys": "volume_mute"},
        {"label": "Vol -", "keys": "volume_down"},
        {"label": "Vol +", "keys": "volume_up"},
        {"label": "Pausa", "keys": "media_play_pause"},
        {"label": "Print", "keys": "print"},
        {"label": "Bloquear", "action": "lock"},
    ]

# ---- teclas ------------------------------------------------------------------

KEY_NAMES = ["ctrl", "shift", "alt", "win", "esc", "enter", "tab", "space", "backspace", "delete", "home", "end",
             "pageup", "pagedown", "left", "up", "right", "down", "print", "volume_mute", "volume_down", "volume_up",
             "media_next", "media_prev", "media_stop", "media_play_pause"] + [f"f{i}" for i in range(1, 13)]

VK = {"ctrl": 0x11, "shift": 0x10, "alt": 0x12, "win": 0x5B, "esc": 0x1B, "enter": 0x0D, "tab": 0x09,
      "space": 0x20, "backspace": 0x08, "delete": 0x2E, "home": 0x24, "end": 0x23, "pageup": 0x21,
      "pagedown": 0x22, "left": 0x25, "up": 0x26, "right": 0x27, "down": 0x28, "print": 0x2C,
      "volume_mute": 0xAD, "volume_down": 0xAE, "volume_up": 0xAF, "media_next": 0xB0,
      "media_prev": 0xB1, "media_stop": 0xB2, "media_play_pause": 0xB3}
VK.update({f"f{i}": 0x6F + i for i in range(1, 13)})
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_EXTENDEDKEY = 0x0001

XDO = {"ctrl": "ctrl", "shift": "shift", "alt": "alt", "win": "super", "esc": "Escape", "enter": "Return",
       "tab": "Tab", "space": "space", "backspace": "BackSpace", "delete": "Delete", "home": "Home", "end": "End",
       "pageup": "Prior", "pagedown": "Next", "left": "Left", "up": "Up", "right": "Right", "down": "Down",
       "print": "Print", "volume_mute": "XF86AudioMute", "volume_down": "XF86AudioLowerVolume",
       "volume_up": "XF86AudioRaiseVolume", "media_next": "XF86AudioNext", "media_prev": "XF86AudioPrev",
       "media_stop": "XF86AudioStop", "media_play_pause": "XF86AudioPlay"}
XDO.update({f"f{i}": f"F{i}" for i in range(1, 13)})

# Teclas de volume e midia nao dependem do X: pactl e playerctl funcionam no Wayland tambem.
LINUX_DIRECT = {
    "volume_mute": ["pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle"],
    "volume_down": ["pactl", "set-sink-volume", "@DEFAULT_SINK@", "-5%"],
    "volume_up": ["pactl", "set-sink-volume", "@DEFAULT_SINK@", "+5%"],
    "media_next": ["playerctl", "next"],
    "media_prev": ["playerctl", "previous"],
    "media_stop": ["playerctl", "stop"],
    "media_play_pause": ["playerctl", "play-pause"],
}


# Sem pactl/playerctl/xdotool (Ubuntu com PipeWire e Wayland): wpctl, MPRIS pelo D-Bus e o portal de captura.
WPCTL = {
    "volume_mute": ["wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"],
    "volume_down": ["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-"],
    "volume_up": ["wpctl", "set-volume", "-l", "1.5", "@DEFAULT_AUDIO_SINK@", "5%+"],
}
MPRIS_METHOD = {"media_next": "Next", "media_prev": "Previous", "media_stop": "Stop", "media_play_pause": "PlayPause"}


def _mpris(method):
    """Manda o comando ao primeiro player MPRIS aberto (Spotify, navegador...)."""
    out = subprocess.run(["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus", "--object-path",
                          "/org/freedesktop/DBus", "--method", "org.freedesktop.DBus.ListNames"],
                         capture_output=True, text=True, timeout=3).stdout
    names = [n for n in re.findall(r"'(org\.mpris\.MediaPlayer2\.[^']+)'", out)]
    if not names:
        raise ValueError("nenhum player de midia aberto")
    subprocess.Popen(["gdbus", "call", "--session", "--dest", names[0], "--object-path", "/org/mpris/MediaPlayer2",
                      "--method", f"org.mpris.MediaPlayer2.Player.{method}"], close_fds=True,
                     stdout=subprocess.DEVNULL)


def _portal_screenshot():
    """Captura de tela no Wayland: o portal abre o seletor do GNOME/KDE."""
    subprocess.Popen(["gdbus", "call", "--session", "--dest", "org.freedesktop.portal.Desktop", "--object-path",
                      "/org/freedesktop/portal/desktop", "--method", "org.freedesktop.portal.Screenshot.Screenshot",
                      "", "{'interactive': <true>}"], close_fds=True, stdout=subprocess.DEVNULL)


def vk_of(name):
    name = name.lower()
    if name in VK:
        return VK[name]
    if len(name) == 1 and name.isalnum():
        return ord(name.upper())
    raise ValueError(f"tecla desconhecida: {name}")


def _press_windows(combo):
    codes = [vk_of(k) for k in combo.split("+")]
    user32 = ctypes.windll.user32
    for c in codes:
        user32.keybd_event(c, 0, KEYEVENTF_EXTENDEDKEY if c >= 0xA6 or c == 0x5B else 0, 0)
    for c in reversed(codes):
        user32.keybd_event(c, 0, KEYEVENTF_KEYUP | (KEYEVENTF_EXTENDEDKEY if c >= 0xA6 or c == 0x5B else 0), 0)


def _press_linux(combo):
    parts = [p.lower() for p in combo.split("+")]
    if len(parts) == 1 and parts[0] in LINUX_DIRECT and shutil.which(LINUX_DIRECT[parts[0]][0]):
        subprocess.Popen(LINUX_DIRECT[parts[0]], close_fds=True)
        return
    if len(parts) == 1 and parts[0] in WPCTL and shutil.which("wpctl"):
        subprocess.Popen(WPCTL[parts[0]], close_fds=True)
        return
    if len(parts) == 1 and parts[0] in MPRIS_METHOD and shutil.which("gdbus"):
        _mpris(MPRIS_METHOD[parts[0]])
        return
    if parts == ["print"] and not shutil.which("xdotool") and shutil.which("gdbus"):
        _portal_screenshot()
        return
    keys = []
    for p in parts:
        if p in XDO:
            keys.append(XDO[p])
        elif len(p) == 1 and p.isalnum():
            keys.append(p)
        else:
            raise ValueError(f"tecla desconhecida: {p}")
    if not shutil.which("xdotool"):
        raise ValueError("teclas combinadas precisam do xdotool (sudo apt install xdotool) e de uma sessao X11")
    subprocess.Popen(["xdotool", "key", "+".join(keys)], close_fds=True)


def press_keys(combo):
    (_press_windows if IS_WINDOWS else _press_linux)(combo)


def lock_screen():
    if IS_WINDOWS:
        ctypes.windll.user32.LockWorkStation()  # Win+L nao pode ser simulado por teclas
    else:
        subprocess.Popen(["loginctl", "lock-session"], close_fds=True)


def run_action(sc):
    if "keys" in sc:
        press_keys(sc["keys"])
    elif sc.get("action") == "lock":
        lock_screen()
    elif "run" in sc:
        args = sc["run"] if isinstance(sc["run"], list) else shlex.split(sc["run"], posix=not IS_WINDOWS)
        subprocess.Popen(args, close_fds=True)
    else:
        raise ValueError("atalho sem 'keys' nem 'run'")


# ---- GPU ---------------------------------------------------------------------

def _nvidia_smi():
    out = subprocess.run(
        ["nvidia-smi", "--query-gpu=utilization.gpu,temperature.gpu,memory.used,memory.total,name",
         "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=3,
        creationflags=NO_WINDOW).stdout.strip().splitlines()[0]
    util, temp, used, total, name = [x.strip() for x in out.split(",", 4)]
    return {"gpu": float(util), "temp": float(temp), "vram": round(100 * float(used) / float(total), 1),
            "gpu_name": name}


def _read(path):
    with open(path) as f:
        return f.read().strip()


def _amdgpu():
    """amdgpu no Linux: /sys/class/drm/cardN/device/gpu_busy_percent (e vram e temperatura)."""
    for dev in sorted(glob.glob("/sys/class/drm/card[0-9]/device")):
        busy = os.path.join(dev, "gpu_busy_percent")
        if not os.path.exists(busy):
            continue
        out = {"gpu": float(_read(busy))}
        try:
            used, total = float(_read(os.path.join(dev, "mem_info_vram_used"))), float(_read(os.path.join(dev, "mem_info_vram_total")))
            out["vram"] = round(100 * used / total, 1)
        except (OSError, ValueError, ZeroDivisionError):
            pass
        temps = glob.glob(os.path.join(dev, "hwmon", "hwmon*", "temp1_input"))
        if temps:
            try:
                out["temp"] = float(_read(temps[0])) / 1000
            except (OSError, ValueError):
                pass
        return out
    raise OSError("sem amdgpu")


def gpu_metrics():
    for probe in (_nvidia_smi,) if IS_WINDOWS else (_nvidia_smi, _amdgpu):
        try:
            return probe()
        except Exception:
            continue
    return {}
