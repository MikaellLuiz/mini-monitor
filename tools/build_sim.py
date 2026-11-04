"""Compila o simulador da interface (sim.exe) com o Zig do venv.

Uso: .venv\\Scripts\\python tools\\build_sim.py [--run] [--script roteiro.txt]
Precisa de um `pio run` antes, para o LVGL estar em .pio/libdeps/cyd/lvgl.
Objetos ficam em .sim_build/ e so recompilam quando o fonte (ou lv_conf.h) muda.
"""
import concurrent.futures
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LVGL = os.path.join(ROOT, ".pio", "libdeps", "cyd", "lvgl")
# --m32: compila em 32 bits (ponteiros de 4 bytes, como o ESP32) para medir o uso real do
# pool do LVGL com o comando "mem" dos roteiros. Objetos e exe ficam em .sim_build32/.
M32 = "--m32" in sys.argv
IS_WIN = os.name == "nt"
BUILD = os.path.join(ROOT, ".sim_build32" if M32 else ".sim_build")
EXE = os.path.join(BUILD, "sim.exe" if IS_WIN else "sim")
if IS_WIN:
    TARGET = ["-target", "x86-windows-gnu"] if M32 else []
else:
    TARGET = ["-m32"] if M32 else []

# So os arquivos de interface portaveis: o resto de src/ e codigo da placa.
UI_SOURCES = ["src/ui.cpp", "src/quickpanel.cpp", "src/wifi_ui.cpp", "src/diag_ui.cpp",
              "src/usage_ui.cpp", "src/spotify_ui.cpp", "src/pc_ui.cpp",
              "src/theme.cpp", "src/clock_ui.cpp", "src/clock_font.c", "src/settings_ui.cpp"]
SIM_SOURCES = glob.glob(os.path.join(ROOT, "sim", "*.cpp"))

INCLUDES = ["-I" + os.path.join(ROOT, d) for d in ("include", "src", "sim")] + ["-I" + LVGL]
DEFINES = ["-DLV_CONF_INCLUDE_SIMPLE"]
ZIG = [sys.executable, "-m", "ziglang"] if IS_WIN else None


def obj_path(src):
    rel = os.path.relpath(src, ROOT).replace(os.sep, "_").replace(".", "_")
    return os.path.join(BUILD, "obj", rel + ".o")


def needs_build(src, obj, deps_mtime):
    return not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(src), deps_mtime)


def compile_one(src):
    obj = obj_path(src)
    is_cpp = src.endswith(".cpp")
    if ZIG:
        cc = ZIG + ["c++" if is_cpp else "cc"]
    else:
        cc = ["g++" if is_cpp else "gcc"]
    cmd = cc + TARGET + ["-c", src, "-o", obj, "-O2", "-g0"]
    cmd += ["-std=c++17"] if is_cpp else ["-std=gnu99", "-w"]
    cmd += INCLUDES + DEFINES
    r = subprocess.run(cmd, capture_output=True, text=True)
    return src, r.returncode, r.stderr


def main():
    if not os.path.isdir(LVGL):
        sys.exit("LVGL nao encontrado: rode `pio run` uma vez antes.")
    os.makedirs(os.path.join(BUILD, "obj"), exist_ok=True)

    lvgl_sources = glob.glob(os.path.join(LVGL, "src", "**", "*.c"), recursive=True)
    app_sources = [os.path.join(ROOT, s) for s in UI_SOURCES] + SIM_SOURCES
    # Cabecalhos do projeto: mudou um, recompila a interface (o LVGL so depende do lv_conf).
    lv_conf_mtime = os.path.getmtime(os.path.join(ROOT, "include", "lv_conf.h"))
    headers = glob.glob(os.path.join(ROOT, "src", "*.h")) + glob.glob(os.path.join(ROOT, "sim", "*.h"))
    headers += glob.glob(os.path.join(ROOT, "include", "*.h"))
    app_deps_mtime = max(os.path.getmtime(h) for h in headers)

    todo = [s for s in lvgl_sources if needs_build(s, obj_path(s), lv_conf_mtime)]
    todo += [s for s in app_sources if needs_build(s, obj_path(s), app_deps_mtime)]
    print(f"compilando {len(todo)} de {len(lvgl_sources) + len(app_sources)} arquivos...", flush=True)

    failed = False
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for src, rc, err in pool.map(compile_one, todo):
            if rc != 0:
                failed = True
                print(f"ERRO em {os.path.relpath(src, ROOT)}:\n{err}")
            elif err.strip() and src.endswith(".cpp"):
                print(f"aviso em {os.path.relpath(src, ROOT)}:\n{err}")
    if failed:
        sys.exit(1)

    objs = [obj_path(s) for s in lvgl_sources + app_sources]
    if ZIG:
        link = ZIG + ["c++"] + TARGET + ["-o", EXE] + objs + ["-lgdi32", "-luser32"]
    else:
        link = ["g++"] + TARGET + ["-o", EXE] + objs
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("ERRO no link:\n" + r.stderr)
    print("ok:", os.path.relpath(EXE, ROOT))

    args = sys.argv[1:]
    if "--run" in args or "--script" in args:
        run = [EXE]
        if "--script" in args:
            i = args.index("--script")
            run += ["--script", os.path.abspath(args[i + 1]), "--out", os.path.join(ROOT, "sim_shots")]
        for i, a in enumerate(args):  # --set chave=valor (configuracao antes de montar a interface)
            if a == "--set" and i + 1 < len(args):
                run += ["--set", args[i + 1]]
        sys.exit(subprocess.call(run, cwd=BUILD))


if __name__ == "__main__":
    main()
