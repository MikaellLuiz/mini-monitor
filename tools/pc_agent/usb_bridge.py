"""Ponte USB entre o programa do PC e a placa (protocolo em include/usb_link.h).

Cada linha util comeca com "@PC " e leva um JSON; o resto da UART sao logs e e ignorado.
A porta abre com DTR e RTS desligados: a placa nao reinicia (como tools/serial_log.py).

    Link         uma porta aberta: leitura numa thread e pedidos com resposta por id
    UsbBridge    acha a placa, autentica (token ou PIN), manda metricas e recebe os atalhos
"""
import itertools
import json
import os
import threading
import time

import serial
from serial.tools import list_ports

PREFIX = "@PC "
KNOWN_VIDS = {0x1A86, 0x10C4, 0x0403}  # CH340 (esta placa), CP210x, FTDI
METRICS_EVERY_S = 2.0
SHORTCUTS_EVERY_S = 30.0
RETRY_S = 3.0
MAX_MISSES = 3  # respostas de metricas perdidas em seguida antes de dar a placa como perdida


class LinkError(Exception):
    pass


class BoardLog:
    """Guarda o que a placa imprime (logs) e o que a ponte faz, para diagnostico: um arquivo de texto
    com hora, que passa para .1 ao chegar em 1 MB. O programa de janela nao tem console."""

    def __init__(self, path, limit=1_000_000):
        self.path, self.limit = path, limit
        self._lock = threading.Lock()

    def write(self, line):
        stamp = time.strftime("%H:%M:%S")
        with self._lock:
            try:
                os.makedirs(os.path.dirname(self.path), exist_ok=True)
                if os.path.exists(self.path) and os.path.getsize(self.path) > self.limit:
                    os.replace(self.path, self.path + ".1")
                with open(self.path, "a", encoding="utf-8") as f:
                    f.write(f"{stamp} {line}\n")
            except OSError:
                pass  # diagnostico nunca pode derrubar a ponte


def find_ports():
    """Portas seriais que parecem uma placa (ponte USB-serial conhecida)."""
    return [p.device for p in list_ports.comports() if p.vid in KNOWN_VIDS]


class Link:
    """Uma porta aberta. `port` pode ser COM5, /dev/ttyUSB0 ou uma URL do pyserial (socket://...)."""

    def __init__(self, port, on_event=None, baud=115200, on_line=None):
        self.port = port
        self.on_event = on_event
        self.on_line = on_line  # recebe as linhas que nao sao do protocolo (logs da placa)
        self.dead = False
        self._ids = itertools.count(1)
        self._pending = {}  # id -> [Event, resposta]
        self._lock = threading.Lock()
        self._write_lock = threading.Lock()
        try:
            self.ser = serial.serial_for_url(port, baudrate=baud, timeout=0.3, do_not_open=True)
            if not port.startswith(("socket:", "loop:")):
                self.ser.dtr = self.ser.rts = False  # antes de abrir: assim a placa nao reinicia
            self.ser.open()
            if not port.startswith(("socket:", "loop:")):
                self.ser.dtr = self.ser.rts = False
        except (serial.SerialException, OSError) as e:
            raise LinkError(f"nao consegui abrir {port}: {e}")
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def _reader(self):
        buf = bytearray()
        try:
            while not self.dead:
                chunk = self.ser.read(self.ser.in_waiting or 1)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    raw, _, rest = bytes(buf).partition(b"\n")
                    buf = bytearray(rest)
                    self._handle(raw.decode("utf-8", "replace").rstrip("\r"))
                if len(buf) > 4096:  # lixo sem fim de linha
                    buf.clear()
        except Exception:  # cabo puxado ou porta fechada por nos: o pyserial varia o tipo do erro
            pass
        finally:
            self.dead = True
            with self._lock:
                for ev, _ in self._pending.values():
                    ev.set()

    def _handle(self, line):
        if not line.startswith(PREFIX):  # log da placa
            at = line.find(PREFIX)
            if at < 0:
                if self.on_line and line.strip():
                    self.on_line(line)
                return
            if self.on_line and line[:at].strip():
                self.on_line(line[:at])
            line = line[at:]
        try:
            msg = json.loads(line[len(PREFIX):])
        except ValueError:
            return
        if not isinstance(msg, dict):
            return
        if "id" in msg:
            with self._lock:
                slot = self._pending.get(msg["id"])
            if slot:
                slot[1] = msg
                slot[0].set()
        elif "evt" in msg and self.on_event:
            try:
                self.on_event(msg)
            except Exception as e:  # um atalho que falha nao derruba a leitura
                print("aviso: evento da placa falhou:", e)

    def request(self, cmd, timeout=3.0, **fields):
        if self.dead:
            raise LinkError("porta fechada")
        rid = next(self._ids)
        slot = [threading.Event(), None]
        with self._lock:
            self._pending[rid] = slot
        line = PREFIX + json.dumps({"id": rid, "cmd": cmd, **fields}, ensure_ascii=True, separators=(",", ":")) + "\n"
        try:
            with self._write_lock:
                self.ser.write(line.encode())
                self.ser.flush()
            if not slot[0].wait(timeout) or slot[1] is None:
                raise LinkError(f"a placa nao respondeu a '{cmd}'")
            return slot[1]
        except (serial.SerialException, OSError) as e:
            self.dead = True
            raise LinkError(str(e))
        finally:
            with self._lock:
                self._pending.pop(rid, None)

    def close(self):
        self.dead = True
        try:
            self.ser.close()
        except Exception:
            pass


class UsbBridge:
    """Dono da conexao USB. Estados em `state`:
    sem_placa | precisa_pin | precisa_criar_pin | bloqueada | autenticado
    """

    def __init__(self, cfg, save_cfg, metrics_fn, shortcuts_fn, action_fn, log=print, board_log=None):
        self.cfg, self.save_cfg = cfg, save_cfg
        self.board_log = board_log
        self.metrics_fn, self.shortcuts_fn, self.action_fn = metrics_fn, shortcuts_fn, action_fn
        self.log = (lambda m: (log(m), board_log.write("[app] " + m))) if board_log else log
        self.link = None
        self.state = "sem_placa"
        self.info = {}
        self.retry_s = 0
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._last_shortcuts = 0.0
        self._misses = 0
        self._paused_until = 0.0

    # ---- conexao ----------------------------------------------------------------

    @property
    def authed(self):
        return self.state == "autenticado" and self.link is not None and not self.link.dead

    def _candidates(self):
        return [self.cfg["usb_port"]] if self.cfg.get("usb_port") else find_ports()

    def _hello(self, link, tries=5):
        """Abrir a serial reinicia a placa (DTR/RTS): ela leva alguns segundos para atender.
        Repete na mesma conexao; fechar e reabrir reiniciaria a placa de novo, num ciclo sem fim."""
        for i in range(tries):
            try:
                return link.request("hello")
            except LinkError:
                if i == tries - 1 or link.dead:
                    raise

    def connect(self):
        """Abre a placa e tenta o token salvo. Devolve o estado."""
        with self._lock:
            if self.link and not self.link.dead:
                return self.state
            self.drop()
            if time.time() < self._paused_until:  # gravando o firmware: a porta e do esptool
                return self.state
            for port in self._candidates():
                link = None
                try:
                    link = Link(port, on_event=self._on_event, on_line=self.board_log.write if self.board_log else None)
                    hello = self._hello(link)
                except LinkError:
                    if link:  # sem fechar, a porta ficaria presa e nenhuma nova tentativa abriria
                        link.close()
                    continue
                if not hello.get("ok"):
                    link.close()
                    continue
                self.link, self.info = link, hello
                self.log(f"placa em {port} (firmware {hello.get('fw')})")
                self._login_with_token()
                return self.state
            self.state = "sem_placa"
            return self.state

    def _login_with_token(self):
        hello = self.info
        token = self.cfg.get("usb_token", "")
        if token:
            r = self.link.request("auth", token=token)
            if r.get("ok"):
                self.state = "autenticado"
                self._last_shortcuts = 0.0
                self.log(f"autenticado como {r.get('client')}")
                return
            if r.get("err") == "token":  # a placa foi redefinida ou este PC foi revogado
                self.log("o token deste PC nao vale mais: informe o PIN")
                self.cfg["usb_token"] = ""
                self.save_cfg(self.cfg)
        if hello.get("locked_s"):
            self.state, self.retry_s = "bloqueada", hello["locked_s"]
        else:
            self.state = "precisa_pin" if hello.get("has_pin") else "precisa_criar_pin"

    def pause(self, seconds=120):
        """Solta a porta para o esptool gravar. Expira sozinha, caso o gravador morra no meio."""
        with self._lock:
            self._paused_until = time.time() + seconds
            self.drop()

    def resume(self):
        self._paused_until = 0.0

    def drop(self):
        if self.link:
            self.link.close()
        self.link, self.state = None, "sem_placa"

    def stop(self):
        self._stop.set()
        self.drop()

    # ---- pedidos ----------------------------------------------------------------

    def request(self, cmd, **fields):
        """Manda um comando. Guarda o token quando a placa autoriza este PC."""
        with self._lock:
            if not self.link or self.link.dead:
                raise LinkError("sem placa conectada")
            r = self.link.request(cmd, **fields)
            if cmd in ("auth", "pin_create") and r.get("ok"):
                self.state = "autenticado"
                if r.get("token"):
                    self.cfg["usb_token"] = r["token"]
                    self.save_cfg(self.cfg)
            elif cmd in ("auth", "pin_create"):
                if r.get("err") == "locked":
                    self.state, self.retry_s = "bloqueada", r.get("retry_s", 0)
            elif r.get("err") == "auth":  # a sessao expirou ou a placa reiniciou
                self.state = "precisa_pin"
            if cmd == "reset" and r.get("ok"):
                self.cfg["usb_token"] = ""
                self.save_cfg(self.cfg)
                self.drop()
            return r

    def _on_event(self, msg):
        if msg.get("evt") == "action":
            self.action_fn(int(msg.get("idx", -1)))

    # ---- laco em segundo plano --------------------------------------------------

    def _tick(self):
        now = time.time()
        try:
            if now - self._last_shortcuts > SHORTCUTS_EVERY_S:
                self.request("shortcuts", labels=self.shortcuts_fn())
                self._last_shortcuts = now
            r = self.request("metrics", name=self.cfg["name"], **self.metrics_fn())
        except LinkError:
            # A placa fica ocupada por alguns segundos durante uma conexao TLS (consulta de uso): uma
            # resposta atrasada nao e cabo solto. So desiste depois de varias seguidas ou com a porta morta.
            self._misses += 1
            if (self.link and self.link.dead) or self._misses >= MAX_MISSES:
                raise
            return
        self._misses = 0
        if not r.get("ok") and r.get("err") == "auth":
            self._login_with_token()  # sessao caiu: tenta o token de novo

    def loop(self):
        last_state = None
        while not self._stop.is_set():
            try:
                if not self.link or self.link.dead:
                    self.connect()
                elif self.state == "precisa_pin" and self.cfg.get("usb_token"):
                    self._login_with_token()  # a sessao expirou ou a placa reiniciou
                if self.state == "autenticado":
                    self._tick()
                    self._stop.wait(METRICS_EVERY_S)
                    continue
            except Exception as e:  # LinkError ou uma leitura de config no meio de uma recarga
                self.log(f"USB: {e}")
                self.drop()
            if self.state != last_state:
                last_state = self.state
                if self.state == "precisa_pin":
                    self.log("a placa pede o PIN: rode  python agent.py setup")
                elif self.state == "precisa_criar_pin":
                    self.log("a placa ainda nao tem PIN: rode  python agent.py setup")
                elif self.state == "bloqueada":
                    self.log(f"placa bloqueada por tentativas erradas ({self.retry_s} s)")
            self._stop.wait(RETRY_S)

    def start(self):
        threading.Thread(target=self.loop, daemon=True, name="usb-bridge").start()
