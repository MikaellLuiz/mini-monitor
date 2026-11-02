"""Agente do PC para o painel CYD: metricas (CPU, RAM, GPU), atalhos e pareamento.

Uso:
    python agent.py              roda o agente (deixe aberto, ou instale na inicializacao)
    python agent.py pair <codigo>   pareia com o painel pelo Wi-Fi (codigo de 6 digitos da tela do painel)
    python agent.py status       mostra a configuracao e os paineis pareados

Pelo cabo USB (a placa e o PC se acham sozinhos; o USB tem prioridade sobre o Wi-Fi):
    python agent.py setup           autoriza este PC na placa (cria o PIN de 6 digitos ou o pede)
    python agent.py ports           lista as portas que parecem a placa
    python agent.py board           estado da placa (Wi-Fi, PC mostrado, PCs, brilho)
    python agent.py wifi <rede> [senha]   salva uma rede Wi-Fi na placa
    python agent.py select <nome|auto>    fixa o PC mostrado na placa, ou volta ao automatico
    python agent.py brightness <10-255>
    python agent.py restart         reinicia a placa
    python agent.py clients         PCs autorizados na placa      (revoke <nome> tira um)
    python agent.py reset           redefinicao completa da placa (pede o PIN e confirmacao)

Com o agente rodando, estes comandos passam por ele (a porta serial so abre uma vez).

A placa consulta este agente pelo Wi-Fi (HTTP simples, rede local, com um token por painel).
O agente so executa atalhos que estao no SEU arquivo de configuracao: o painel envia
apenas o numero do atalho. Configuracao: %APPDATA%\\painel-agent\\config.json
"""
import concurrent.futures
import hmac
import http.server
import json
import os
import secrets
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

import psutil

import platform_pc as plat
from platform_pc import DEFAULT_SHORTCUTS, gpu_metrics, run_action

VERSION = "0.2"
CONFIG_DIR = plat.config_dir()
CONFIG_PATH = os.path.join(CONFIG_DIR, "config.json")


def load_config():
    cfg = {"name": socket.gethostname(), "port": 8765, "boards": {}, "shortcuts": DEFAULT_SHORTCUTS,
           "usb_enabled": True, "usb_port": "", "usb_token": "", "control_port": 8766}
    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH, encoding="utf-8") as f:
            cfg.update(json.load(f))
    else:
        save_config(cfg)
    return cfg


def save_config(cfg):
    os.makedirs(CONFIG_DIR, exist_ok=True)
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2, ensure_ascii=False)


# ---- metricas ----------------------------------------------------------------

_metrics_cache = {"t": 0, "v": {}}
_metrics_lock = threading.Lock()
_cpu = 0.0
_sampler_started = False


def _cpu_sampler():
    # psutil guarda a "ultima leitura" POR THREAD: o servidor cria uma thread por pedido, e
    # cada uma seria a primeira leitura (sempre 0). Uma thread propria mede continuamente.
    global _cpu
    while True:
        _cpu = psutil.cpu_percent(interval=1.0)


def start_cpu_sampler():
    global _sampler_started
    if not _sampler_started:
        _sampler_started = True
        threading.Thread(target=_cpu_sampler, daemon=True).start()


def metrics():
    with _metrics_lock:
        if time.time() - _metrics_cache["t"] < 1.5:
            return _metrics_cache["v"]
        vm = psutil.virtual_memory()
        start_cpu_sampler()
        v = {"cpu": _cpu, "ram": vm.percent, "uptime": int(time.time() - psutil.boot_time())}
        v.update(gpu_metrics())
        _metrics_cache.update(t=time.time(), v=v)
        return v


# ---- servidor ----------------------------------------------------------------

CFG = None
_cfg_mtime = 0


def reload_config_if_changed():
    """Pareamentos e atalhos novos valem na hora, sem reiniciar o agente."""
    global _cfg_mtime
    try:
        m = os.path.getmtime(CONFIG_PATH)
    except OSError:
        return
    if m != _cfg_mtime:
        _cfg_mtime = m
        CFG.clear()
        CFG.update(load_config())


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def _json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=True).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _authorized(self):
        reload_config_if_changed()
        auth = self.headers.get("Authorization", "")
        token = auth[7:] if auth.startswith("Bearer ") else ""
        return any(hmac.compare_digest(token, t) for t in CFG["boards"].values())

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/hello":
            return self._json(200, {"agent": "painel-cyd", "name": CFG["name"], "version": VERSION})
        if not self._authorized():
            return self._json(401, {"error": "nao autorizado"})
        if path == "/metrics":
            return self._json(200, metrics())
        if path == "/shortcuts":
            return self._json(200, [{"id": i, "label": s.get("label", f"Atalho {i + 1}")[:12]}
                                    for i, s in enumerate(CFG["shortcuts"])])
        self._json(404, {"error": "nao encontrado"})

    def do_POST(self):
        if not self._authorized():
            return self._json(401, {"error": "nao autorizado"})
        if self.path.startswith("/action/"):
            try:
                idx = int(self.path.rsplit("/", 1)[1])
                run_action(CFG["shortcuts"][idx])
                print(f"atalho {idx}: {CFG['shortcuts'][idx].get('label')}")
                return self._json(200, {"ok": True})
            except (ValueError, IndexError) as e:
                return self._json(400, {"error": str(e)})
        self._json(404, {"error": "nao encontrado"})


def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("8.8.8.8", 80))
    ip = s.getsockname()[0]
    s.close()
    return ip


def advertise(port):
    """Anuncia o agente por mDNS (_painelcyd._tcp) para a placa achar mesmo se o IP mudar."""
    try:
        from zeroconf import ServiceInfo, Zeroconf
        info = ServiceInfo("_painelcyd._tcp.local.", f"{CFG['name']}._painelcyd._tcp.local.",
                           addresses=[socket.inet_aton(local_ip())], port=port,
                           properties={"v": VERSION}, server=f"{socket.gethostname()}.local.")
        zc = Zeroconf()
        zc.register_service(info)
        return zc
    except Exception as e:
        print("aviso: mDNS indisponivel:", e)
        return None


# ---- USB ---------------------------------------------------------------------

BRIDGE = None


def usb_metrics():
    m = metrics()
    return {k: m[k] for k in ("cpu", "ram", "gpu", "vram", "temp") if k in m}


def usb_shortcut_labels():
    return [s.get("label", f"Atalho {i + 1}")[:12] for i, s in enumerate(CFG["shortcuts"])]


def usb_action(idx):
    reload_config_if_changed()
    try:
        sc = CFG["shortcuts"][idx]
        run_action(sc)
        print(f"atalho {idx} (USB): {sc.get('label')}")
    except (ValueError, IndexError) as e:
        print("atalho invalido:", e)


def make_bridge(cfg):
    from usb_bridge import BoardLog, UsbBridge
    return UsbBridge(cfg, save_config, usb_metrics, usb_shortcut_labels, usb_action,
                     log=lambda m: print("[usb]", m), board_log=BoardLog(os.path.join(CONFIG_DIR, "board.log")))  # CONFIG_DIR lido agora: os testes o trocam


class ControlHandler(http.server.BaseHTTPRequestHandler):
    """Passa comandos para a placa pelo agente (so ele abre a porta serial). Somente 127.0.0.1.

    Uma pagina da web tambem consegue chamar 127.0.0.1, entao exige um cabecalho proprio (o que
    forca um pre-voo que nao respondemos) e confere o Host (contra DNS rebinding)."""
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def _json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=True).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        port = CFG.get("control_port", 8766)
        try:  # le o corpo antes de responder (recusar sem le-lo faz o Windows cortar a conexao)
            raw = self.rfile.read(min(int(self.headers.get("Content-Length", "0")), 4096))
        except (ValueError, OSError):
            raw = b""
        if (self.path != "/board" or self.headers.get("X-Painel") != "1"
                or self.headers.get("Host", "") not in (f"127.0.0.1:{port}", f"localhost:{port}")):
            return self._json(403, {"ok": False, "err": "proibido"})
        try:
            body = json.loads(raw)
            cmd = body.pop("cmd")
        except (ValueError, KeyError, TypeError):
            return self._json(400, {"ok": False, "err": "pedido invalido"})
        from usb_bridge import LinkError
        try:
            if cmd in ("pause_usb", "resume_usb"):  # tools/flash.py: grava sem fechar o programa
                if BRIDGE:
                    BRIDGE.pause(int(body.get("seconds", 120))) if cmd == "pause_usb" else BRIDGE.resume()
                return self._json(200, {"ok": True})
            if cmd == "bridge_status":
                return self._json(200, {"ok": True, "state": BRIDGE.state, "authed": BRIDGE.authed, "info": BRIDGE.info})
            if not BRIDGE.link or BRIDGE.link.dead:
                BRIDGE.connect()
            if not BRIDGE.link:
                return self._json(200, {"ok": False, "err": "usb", "msg": "nenhuma placa encontrada (cabo USB?)"})
            self._json(200, BRIDGE.request(cmd, **body))
        except LinkError as e:
            self._json(200, {"ok": False, "err": "usb", "msg": str(e)})
        except TypeError as e:
            self._json(400, {"ok": False, "err": f"campos invalidos: {e}"})


def start_usb():
    global BRIDGE
    if not CFG.get("usb_enabled", True):
        return
    try:
        BRIDGE = make_bridge(CFG)
    except ImportError as e:
        print("aviso: USB desligado (instale o pyserial: pip install pyserial):", e)
        return
    BRIDGE.start()
    try:
        ctl = http.server.ThreadingHTTPServer(("127.0.0.1", CFG["control_port"]), ControlHandler)
        threading.Thread(target=ctl.serve_forever, daemon=True).start()
    except OSError as e:
        print(f"aviso: porta de controle {CFG['control_port']} ocupada ({e}): os comandos 'board', 'wifi'... nao passam por este agente")


_services = {"server": None, "zc": None}


def start_services():
    """Liga o agente em segundo plano: HTTP para a placa, mDNS e a ponte USB. Idempotente."""
    global CFG, _cfg_mtime
    if _services["server"]:
        return
    CFG = load_config()
    _cfg_mtime = os.path.getmtime(CONFIG_PATH)
    start_cpu_sampler()
    port = CFG["port"]
    server = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True, name="agent-http").start()
    _services["server"] = server
    _services["zc"] = advertise(port)
    print(f"agente '{CFG['name']}' em http://{local_ip()}:{port} | paineis pareados: {len(CFG['boards'])}")
    start_usb()


def stop_services():
    global BRIDGE
    if BRIDGE:
        BRIDGE.stop()
        BRIDGE = None
    if _services["server"]:
        _services["server"].shutdown()
        _services["server"].server_close()
        _services["server"] = None
    if _services["zc"]:
        _services["zc"].close()
        _services["zc"] = None


def cmd_run():
    start_services()
    if not CFG["boards"]:
        print("nenhum painel pareado por Wi-Fi: toque na engrenagem da pagina PC no painel e rode:  python agent.py pair <codigo>")
    print("(Ctrl+C para sair)")
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        stop_services()


# ---- comandos da placa pelo USB ----------------------------------------------

_direct = None


def board_call(cmd, **fields):
    """Manda um comando a placa: pelo agente em execucao, ou direto pela porta serial."""
    global _direct
    cfg = load_config()
    payload = json.dumps({"cmd": cmd, **fields}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{cfg['control_port']}/board", data=payload,
                                 headers={"X-Painel": "1", "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        raise SystemExit(f"o agente recusou o pedido ({e.code})")
    except OSError:
        pass  # agente nao esta rodando: usa a porta direto
    from usb_bridge import LinkError
    if _direct is None:
        _direct = make_bridge(cfg)
        _direct.connect()
    if not _direct.link:
        return {"ok": False, "err": "usb", "msg": "nenhuma placa encontrada (cabo USB conectado?)"}
    try:
        return _direct.request(cmd, **fields)
    except LinkError as e:
        return {"ok": False, "err": "usb", "msg": str(e)}


ERRORS = {
    "pin": "PIN errado.",
    "locked": "A placa bloqueou novas tentativas por um tempo.",
    "no_pin": "A placa ainda nao tem PIN: rode  python agent.py setup.",
    "has_pin": "A placa ja tem PIN.",
    "invalid": "Dado invalido (o PIN tem exatamente 6 digitos).",
    "name": "Nome de PC invalido.",
    "clients_full": "A placa ja tem 6 PCs autorizados. Tire um com 'revoke <nome>' (use um PC que ainda tenha acesso).",
    "token": "O token deste PC nao vale mais: rode  python agent.py setup.",
    "auth": "Este PC nao esta autorizado: rode  python agent.py setup.",
    "wifi_invalid": "Rede invalida (nome de 1 a 32 caracteres; senha vazia ou de 8 a 63).",
    "range": "Valor fora do limite.",
    "not_found": "Nao encontrado.",
    "unknown": "Comando desconhecido para este firmware (atualize o firmware).",
}


def explain(r):
    msg = r.get("msg") or ERRORS.get(r.get("err"), f"erro: {r.get('err')}")
    if r.get("retry_s"):
        msg += f" Espere {r['retry_s']} s."
    return msg


def board_login():
    """Autoriza este PC na placa: cria o PIN (placa nova) ou pede o PIN (PC novo)."""
    import getpass
    import re
    hello = board_call("hello")
    if not hello.get("ok"):
        raise SystemExit(explain(hello))
    if hello.get("authed"):
        return
    if hello.get("locked_s"):
        raise SystemExit(f"A placa bloqueou novas tentativas. Espere {hello['locked_s']} s.")
    name = load_config()["name"]
    if not hello.get("has_pin"):
        print("A placa ainda nao tem PIN. Crie um de 6 digitos. Guarde-o: se esquecer, so a")
        print("redefinicao completa (apaga Wi-Fi, contas e PCs) deixa criar outro.")
        while True:
            a, b = getpass.getpass("novo PIN: "), getpass.getpass("repita o PIN: ")
            if a == b and re.fullmatch(r"\d{6}", a):
                break
            print("O PIN precisa ter 6 digitos e os dois campos devem ser iguais.")
        r = board_call("pin_create", pin=a, name=name)
    else:
        r = {"ok": False}
        for _ in range(3):
            r = board_call("auth", pin=getpass.getpass("PIN da placa: "), name=name)
            if r.get("ok") or r.get("err") != "pin":
                break
            print(explain(r))
    if not r.get("ok"):
        raise SystemExit(explain(r))
    print(f"PC '{name}' autorizado.")


def board_cmd(cmd, **fields):
    r = board_call(cmd, **fields)
    if r.get("err") in ("auth", "token"):
        board_login()
        r = board_call(cmd, **fields)
    if not r.get("ok"):
        raise SystemExit(explain(r))
    return r


def cmd_setup():
    board_login()
    r = board_cmd("status")
    print(f"pronto. Wi-Fi da placa: {r['wifi']['state']} {r['wifi']['ssid']} | PC mostrado: {r['active'] or 'nenhum'}")
    print("Deixe o agente rodando (python agent.py) para enviar as metricas pelo cabo.")


def cmd_ports():
    from usb_bridge import find_ports
    ports = find_ports()
    print("\n".join(ports) if ports else "nenhuma porta de placa encontrada")


def cmd_board():
    r = board_cmd("status")
    w = r["wifi"]
    print(f"firmware {r['fw']} | brilho {r['bright']}")
    print(f"Wi-Fi: {w['state']} {w['ssid']} {w['ip']} ({w['rssi']} dBm, {w['saved']} redes salvas)")
    print(f"PC mostrado: {r['active'] or 'nenhum'} via {r['source']} ({'online' if r['online'] else 'offline'})"
          f" | fixado: {r['pinned'] or 'nao (automatico)'}")
    for p in r["pcs"]:
        via = " + ".join(x for x, on in (("USB", p["usb"]), ("Wi-Fi", p["wifi"])) if on) or "fora de alcance"
        print(f"  - {p['name']} ({via})")
    print("PCs autorizados:", ", ".join(r["clients"]) or "nenhum")


def cmd_reset():
    import getpass
    print("Isto apaga TUDO da placa: PIN, redes Wi-Fi, tokens das contas, PCs pareados e a calibracao do toque.")
    if input("Digite REDEFINIR para continuar: ").strip() != "REDEFINIR":
        raise SystemExit("cancelado")
    board_cmd("reset", pin=getpass.getpass("PIN da placa: "))
    cfg = load_config()
    cfg["usb_token"] = ""
    save_config(cfg)
    print("placa redefinida; ela vai reiniciar e pedir a calibracao do toque.")


# ---- pareamento --------------------------------------------------------------

def find_board():
    prefix = local_ip().rsplit(".", 1)[0]

    def is_board(ip):
        try:
            with urllib.request.urlopen(f"http://{ip}/status?p=agy", timeout=0.8) as r:
                return "attempts" in json.loads(r.read())
        except Exception:
            return False

    with concurrent.futures.ThreadPoolExecutor(max_workers=64) as pool:
        ips = [f"{prefix}.{i}" for i in range(1, 255)]
        found = [ip for ip, ok in zip(ips, pool.map(is_board, ips)) if ok]
    return found[0] if found else None


def pair_with_board(code):
    """Pareia com o painel pelo Wi-Fi usando o codigo da tela. Devolve o IP; RuntimeError se falhar."""
    cfg = load_config()
    ip = find_board()
    if not ip:
        raise RuntimeError("nao achei o painel: toque na engrenagem da pagina PC e confira que o PC esta na mesma rede")
    token = secrets.token_hex(16)
    data = urllib.parse.urlencode({"code": code, "name": cfg["name"], "host": socket.gethostname(),
                                   "port": cfg["port"], "token": token}).encode()
    try:
        with urllib.request.urlopen(urllib.request.Request(f"http://{ip}/pair", data=data), timeout=15) as r:
            r.read()
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"o painel recusou ({e.code}): confira o codigo")
    except OSError as e:
        raise RuntimeError(f"sem resposta do painel: {e}")
    cfg["boards"][ip] = token
    save_config(cfg)
    return ip


def cmd_pair(code):
    print("procurando o painel na rede...")
    try:
        ip = pair_with_board(code)
    except RuntimeError as e:
        raise SystemExit(str(e))
    print(f"pareado com o painel em {ip}. Agora rode (ou reinicie) o agente: python agent.py")


def cmd_status():
    cfg = load_config()
    print("config:", CONFIG_PATH)
    print("nome:", cfg["name"], "| porta:", cfg["port"])
    print("paineis pareados:", list(cfg["boards"].keys()) or "nenhum")
    print("atalhos:")
    for i, s in enumerate(cfg["shortcuts"]):
        print(f"  {i}: {s.get('label')} -> {s.get('keys') or s.get('run') or s.get('action')}")
    start_cpu_sampler()
    time.sleep(1.3)  # tempo da primeira medicao de CPU
    print("metricas agora:", metrics())


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        cmd_run()
    elif args[0] == "pair" and len(args) == 2:
        cmd_pair(args[1])
    elif args[0] == "status":
        cmd_status()
    elif args[0] == "setup":
        cmd_setup()
    elif args[0] == "ports":
        cmd_ports()
    elif args[0] == "board":
        cmd_board()
    elif args[0] == "wifi" and len(args) in (2, 3):
        board_cmd("wifi_set", **{"ssid": args[1], "pass": args[2] if len(args) == 3 else ""})  # "pass" e palavra reservada
        print("rede salva; a placa vai tentar conectar.")
    elif args[0] == "select" and len(args) == 2:
        board_cmd("pc_select", name=args[1])
        print("PC fixado" if args[1] != "auto" else "automatico")
    elif args[0] == "brightness" and len(args) == 2:
        board_cmd("brightness", v=int(args[1]))
    elif args[0] == "restart":
        board_cmd("restart")
        print("reiniciando a placa")
    elif args[0] == "clients":
        print("\n".join(board_cmd("status")["clients"]) or "nenhum")
    elif args[0] == "revoke" and len(args) == 2:
        board_cmd("revoke", name=args[1])
        print("PC removido da placa")
    elif args[0] == "reset":
        cmd_reset()
    else:
        raise SystemExit(__doc__)
