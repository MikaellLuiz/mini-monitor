"""Placa falsa para testar a ponte USB sem hardware: fala o protocolo de include/usb_link.h
por um socket TCP (o pyserial abre como socket://127.0.0.1:porta).

Segue as mesmas regras do firmware (src/usb_link_esp32.cpp e src/auth_esp32.cpp): PIN de
6 digitos, bloqueio apos 5 erros, um token por PC e sessao so autenticada. Tambem solta linhas
de log no meio do trafego, para provar que so vale a linha que COMECA com "@PC ".
"""
import hashlib
import json
import secrets
import socket
import threading

PREFIX = "@PC "
FREE_TRIES = 5


class FakeBoard:
    def __init__(self):
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.pin = None
        self.fails = 0
        self.locked_until = 0.0
        self.clients = {}  # nome -> sha256(token)
        self.authed = False
        self.metrics = []   # ultimas metricas recebidas
        self.shortcuts = []
        self.pinned = ""
        self.wifi = []
        self.resets = 0
        self.fake_clock = 0.0  # segundos; os testes avancam isto em vez de esperar
        self.conn = None
        self._stop = False
        threading.Thread(target=self._accept, daemon=True).start()

    @property
    def url(self):
        return f"socket://127.0.0.1:{self.port}"

    def _accept(self):
        while not self._stop:
            try:
                conn, _ = self.srv.accept()
            except OSError:
                return
            self.conn = conn
            self.authed = False  # sessao nova a cada conexao
            self.send_log("boot: painel")
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def send_raw(self, text):
        try:
            self.conn.sendall(text.encode())
        except (OSError, AttributeError):
            pass

    def send_log(self, text):
        self.send_raw(text + "\n")

    def send(self, obj):
        self.send_raw(PREFIX + json.dumps(obj, separators=(",", ":")) + "\n")

    def press_shortcut(self, idx):
        self.send_log('[pc] "ack":"lixo de log que nao e comando"')
        self.send({"evt": "action", "idx": idx})

    def _serve(self, conn):
        buf = b""
        while not self._stop:
            try:
                data = conn.recv(1024)
            except OSError:
                return
            if not data:
                return
            buf += data
            while b"\n" in buf:
                raw, _, buf = buf.partition(b"\n")
                line = raw.decode("utf-8", "replace").rstrip("\r")
                if line.startswith(PREFIX):
                    try:
                        self._handle(json.loads(line[len(PREFIX):]))
                    except ValueError:
                        self.send({"ok": False, "err": "json"})

    # ---- regras do firmware --------------------------------------------------

    def _penalty(self):
        if self.fails < FREE_TRIES:
            return 0
        return min(30 << min(self.fails - FREE_TRIES, 7), 3600)

    def _lock_left(self):
        return max(0, int(self.locked_until - self.fake_clock))

    def _check_pin(self, pin):
        if self._lock_left():
            return "locked", self._lock_left()
        if pin == self.pin:
            self.fails = 0
            return "ok", 0
        self.fails += 1
        pen = self._penalty()
        self.locked_until = self.fake_clock + pen if pen else 0
        return "wrong", pen

    def _issue(self, rid, name):
        if name not in self.clients and len(self.clients) >= 6:
            return self.send({"id": rid, "ok": False, "err": "clients_full"})
        token = secrets.token_hex(16)
        self.clients[name] = hashlib.sha256(token.encode()).hexdigest()
        self.authed = True
        self.send({"id": rid, "ok": True, "token": token})

    def _handle(self, c):
        cmd, rid = c.get("cmd", ""), c.get("id")

        def reply(ok, err=None, **kw):
            if rid is None:
                return
            msg = {"id": rid, "ok": ok, **kw}
            if err and not ok:
                msg["err"] = err
            self.send(msg)

        if cmd == "hello":
            return reply(True, proto=1, fw="fake", has_pin=self.pin is not None,
                         locked_s=self._lock_left(), authed=self.authed)
        if cmd == "auth":
            if c.get("token"):
                h = hashlib.sha256(c["token"].encode()).hexdigest()
                who = [n for n, th in self.clients.items() if th == h]
                if not who:
                    return reply(False, "token")
                self.authed = True
                return reply(True, client=who[0])
            if self.pin is None:
                return reply(False, "no_pin")
            res, retry = self._check_pin(c.get("pin", ""))
            if res != "ok":
                return self.send({"id": rid, "ok": False, "err": res if res == "locked" else "pin", "retry_s": retry})
            if not c.get("name"):
                self.authed = True
                return reply(True)
            return self._issue(rid, c["name"])
        if cmd == "pin_create":
            pin = c.get("pin", "")
            if self.pin is not None:
                return reply(False, "has_pin")
            if len(pin) != 6 or not pin.isdigit():
                return reply(False, "invalid")
            self.pin = pin
            return self._issue(rid, c.get("name", "PC"))
        if not self.authed:
            return reply(False, "auth")
        if cmd == "metrics":
            self.metrics.append(c)
            return reply(True)
        if cmd == "shortcuts":
            self.shortcuts = c.get("labels", [])
            return reply(True)
        if cmd == "status":
            return reply(True, fw="fake", bright=200, wifi={"state": "connected", "ssid": "Casa", "ip": "10.0.0.2",
                                                           "rssi": -50, "saved": len(self.wifi)},
                         source="usb", online=True, active="PC", pinned=self.pinned,
                         pcs=[{"name": "PC", "usb": True, "wifi": False}], clients=list(self.clients), heap=100000)
        if cmd == "wifi_set":
            self.wifi.append((c.get("ssid"), c.get("pass")))
            return reply(True)
        if cmd == "pc_select":
            n = c.get("name", "")
            self.pinned = "" if n == "auto" else n
            return reply(True)
        if cmd == "revoke":
            return reply(self.clients.pop(c.get("name", ""), None) is not None, "not_found")
        if cmd == "reset":
            res, retry = self._check_pin(c.get("pin", ""))
            if res != "ok":
                return self.send({"id": rid, "ok": False, "err": res if res == "locked" else "pin", "retry_s": retry})
            self.pin, self.clients, self.authed, self.resets = None, {}, False, self.resets + 1
            return reply(True)
        if cmd == "bye":
            self.authed = False
            return reply(True)
        reply(False, "unknown")

    def close(self):
        self._stop = True
        for s in (self.conn, self.srv):
            try:
                s.close()
            except (OSError, AttributeError):
                pass
