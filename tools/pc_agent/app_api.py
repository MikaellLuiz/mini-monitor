"""O que a janela do Painel CYD pode fazer. Cada metodo publico vira uma chamada da interface
(pywebview js_api, ou HTTP em 127.0.0.1 quando nao ha pywebview) e devolve um dict simples:
{"ok": True, ...} ou {"ok": False, "msg": "texto para mostrar"}.
"""
import re
import sys

import agent
import autostart
import platform_pc as plat
from usb_bridge import LinkError

NO_BOARD = "Nenhuma placa encontrada. Conecte o cabo USB (e feche outros programas que usem a porta)."
KEY_RE = re.compile(r"^[a-z0-9]$")


def _fail(msg):
    return {"ok": False, "msg": msg}


def valid_keys(combo):
    parts = [p.strip().lower() for p in combo.split("+")]
    return bool(parts) and all(p in plat.KEY_NAMES or KEY_RE.match(p) for p in parts)


class Api:
    # ---- chamada a placa --------------------------------------------------------

    def _call(self, cmd, **fields):
        b = agent.BRIDGE
        if b is None:
            return {"ok": False, "err": "usb", "msg": "O USB esta desligado nas configuracoes do agente."}
        try:
            if not b.link or b.link.dead:
                b.connect()
            if not b.link:
                return {"ok": False, "err": "usb", "msg": NO_BOARD}
            return b.request(cmd, **fields)
        except LinkError as e:
            return {"ok": False, "err": "usb", "msg": str(e)}

    def _result(self, r):
        """Resposta da placa -> o formato da interface."""
        if r.get("ok"):
            return {"ok": True}
        return {"ok": False, "err": r.get("err"), "retry_s": r.get("retry_s", 0), "msg": agent.explain(r)}

    # ---- estado -----------------------------------------------------------------

    def state(self):
        cfg = agent.CFG
        b = agent.BRIDGE
        usb = {"state": "desligado", "port": "", "fw": "", "has_pin": False, "retry_s": 0}
        board = None
        if b is not None:
            if (not b.link or b.link.dead) and b.state != "autenticado":
                b.connect()
            usb.update(state=b.state, port=b.link.port if b.link and not b.link.dead else "",
                       fw=b.info.get("fw", ""), has_pin=bool(b.info.get("has_pin")), retry_s=b.retry_s)
            if b.authed:
                r = self._call("status")
                if r.get("ok"):
                    board = {k: r[k] for k in ("fw", "bright", "wifi", "source", "online", "active", "pinned", "pcs", "clients", "heap")
                             if k in r}
        return {
            "app": {"version": agent.VERSION, "name": cfg["name"], "platform": sys.platform,
                    "autostart": autostart.is_enabled()},
            "usb": usb,
            "board": board,
            "wifi_boards": sorted(cfg["boards"].keys()),
            "shortcuts": cfg["shortcuts"],
            "key_names": plat.KEY_NAMES,
        }

    # ---- PIN e autorizacao ------------------------------------------------------

    def login(self, pin):
        r = self._call("auth", pin=str(pin), name=agent.CFG["name"])
        return self._result(r)

    def create_pin(self, pin):
        pin = str(pin)
        if not re.fullmatch(r"\d{6}", pin):
            return _fail("O PIN precisa ter exatamente 6 digitos.")
        return self._result(self._call("pin_create", pin=pin, name=agent.CFG["name"]))

    def revoke(self, name):
        return self._result(self._call("revoke", name=str(name)))

    def reset_board(self, pin):
        """Redefinicao completa. A interface so chama depois de uma confirmacao."""
        return self._result(self._call("reset", pin=str(pin)))

    # ---- placa ------------------------------------------------------------------

    def wifi_set(self, ssid, password):
        return self._result(self._call("wifi_set", ssid=str(ssid), **{"pass": str(password or "")}))

    def brightness(self, value):
        return self._result(self._call("brightness", v=int(value)))

    def restart(self):
        return self._result(self._call("restart"))

    def select_pc(self, name):
        return self._result(self._call("pc_select", name=str(name) or "auto"))

    def pair_wifi(self, code):
        """Pareia com a placa pelo Wi-Fi (codigo de 6 digitos que aparece na tela dela)."""
        code = str(code).strip()
        if not re.fullmatch(r"\d{6}", code):
            return _fail("O codigo tem 6 digitos e aparece na tela do painel (PC > engrenagem > Parear um PC).")
        try:
            ip = agent.pair_with_board(code)
        except RuntimeError as e:
            return _fail(str(e))
        return {"ok": True, "ip": ip}

    def forget_wifi_board(self, ip):
        cfg = agent.load_config()
        cfg["boards"].pop(str(ip), None)
        agent.save_config(cfg)
        return {"ok": True}

    # ---- atalhos ----------------------------------------------------------------

    @staticmethod
    def _clean_shortcut(sc, i):
        """Valida um atalho. Devolve (atalho, None) ou (None, mensagem)."""
        label = str(sc.get("label", "")).strip()[:12]
        if not label:
            return None, f"O atalho {i + 1} precisa de um nome."
        if sc.get("action") == "lock":
            return {"label": label, "action": "lock"}, None
        if sc.get("keys"):
            keys = str(sc["keys"]).strip().lower()
            if not valid_keys(keys):
                return None, f"Teclas invalidas em '{label}'. Use, por exemplo: ctrl+shift+esc"
            return {"label": label, "keys": keys}, None
        if sc.get("run"):
            return {"label": label, "run": str(sc["run"]).strip()}, None
        return None, f"O atalho '{label}' precisa de teclas, um programa ou 'bloquear'."

    def save_shortcuts(self, shortcuts):
        if not isinstance(shortcuts, list) or not 0 < len(shortcuts) <= 6:
            return _fail("A placa mostra de 1 a 6 atalhos.")
        clean = []
        for i, sc in enumerate(shortcuts):
            item, err = self._clean_shortcut(sc, i)
            if err:
                return _fail(err)
            clean.append(item)
        cfg = agent.load_config()
        cfg["shortcuts"] = clean
        agent.save_config(cfg)
        agent.reload_config_if_changed()
        if agent.BRIDGE:
            agent.BRIDGE._last_shortcuts = 0.0  # a ponte reenvia os nomes na proxima volta
        return {"ok": True}

    def test_shortcut(self, shortcut):
        """Executa um atalho neste PC sem salvar (para testar o que esta na tela)."""
        item, err = self._clean_shortcut(shortcut, 0)
        if err:
            return _fail(err)
        try:
            plat.run_action(item)
        except (ValueError, OSError) as e:
            return _fail(str(e))
        return {"ok": True}

    # ---- aplicativo -------------------------------------------------------------

    def set_autostart(self, on):
        try:
            return {"ok": True, "autostart": autostart.set_enabled(bool(on))}
        except OSError as e:
            return _fail(f"Nao consegui mudar a inicializacao com o sistema: {e}")

    def ports(self):
        from usb_bridge import find_ports
        return {"ok": True, "ports": find_ports()}
