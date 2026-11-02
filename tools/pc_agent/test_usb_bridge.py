"""Testes da ponte USB contra a placa falsa (fake_board.py). Rode:

    python -m unittest tools/pc_agent/test_usb_bridge.py -v
"""
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from fake_board import FakeBoard  # noqa: E402
from usb_bridge import PREFIX, Link, LinkError, UsbBridge  # noqa: E402


def wait_for(cond, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.02)
    return False


class Env:
    """Um PC de teste: sua propria config e sua ponte."""

    def __init__(self, board, name="PC-A"):
        self.cfg = {"name": name, "usb_port": board.url, "usb_token": ""}
        self.saved = 0
        self.actions = []
        self.bridge = UsbBridge(self.cfg, self._save, lambda: {"cpu": 12.0, "ram": 40.0, "temp": 55.0},
                                lambda: ["Mudo", "Vol +"], self.actions.append, log=lambda m: None)

    def _save(self, cfg):
        self.saved += 1


class UsbBridgeTests(unittest.TestCase):
    def setUp(self):
        self.board = FakeBoard()
        self.envs = []

    def tearDown(self):
        for e in self.envs:
            e.bridge.stop()
        self.board.close()

    def env(self, name="PC-A"):
        e = Env(self.board, name)
        self.envs.append(e)
        return e

    def test_ignores_log_lines_and_needs_the_prefix(self):
        e = self.env()
        self.assertEqual(e.bridge.connect(), "precisa_criar_pin")  # o "boot: painel" do log nao atrapalhou
        self.board.send_log('{"id":1,"ok":true}')  # JSON sem o prefixo: log, nao resposta
        self.board.send_raw(PREFIX + "isto nao e json\n")
        self.assertEqual(e.bridge.request("hello")["has_pin"], False)

    def test_first_setup_creates_pin_and_saves_token(self):
        e = self.env()
        e.bridge.connect()
        r = e.bridge.request("pin_create", pin="123456", name="PC-A")
        self.assertTrue(r["ok"])
        self.assertEqual(len(e.cfg["usb_token"]), 32)
        self.assertTrue(e.saved)
        self.assertEqual(e.bridge.state, "autenticado")

    def test_invalid_pin_is_refused(self):
        e = self.env()
        e.bridge.connect()
        self.assertEqual(e.bridge.request("pin_create", pin="12345", name="PC-A")["err"], "invalid")
        self.assertEqual(e.bridge.request("pin_create", pin="abcdef", name="PC-A")["err"], "invalid")
        self.assertEqual(e.cfg["usb_token"], "")

    def test_returning_pc_uses_the_saved_token_without_pin(self):
        a = self.env()
        a.bridge.connect()
        a.bridge.request("pin_create", pin="123456", name="PC-A")
        a.bridge.drop()
        self.assertTrue(wait_for(lambda: self.board.conn is not None))
        self.assertEqual(a.bridge.connect(), "autenticado")

    def test_new_pc_needs_the_pin_and_gets_its_own_token(self):
        a = self.env("PC-A")
        a.bridge.connect()
        a.bridge.request("pin_create", pin="123456", name="PC-A")
        a.bridge.drop()
        b = self.env("NOTE")
        self.assertEqual(b.bridge.connect(), "precisa_pin")
        self.assertEqual(b.bridge.request("auth", pin="000000", name="NOTE")["err"], "pin")
        r = b.bridge.request("auth", pin="123456", name="NOTE")
        self.assertTrue(r["ok"])
        self.assertNotEqual(b.cfg["usb_token"], a.cfg["usb_token"])
        self.assertEqual(set(self.board.clients), {"PC-A", "NOTE"})

    def test_wrong_pin_locks_even_for_the_right_pin_then_unlocks(self):
        a = self.env()
        a.bridge.connect()
        a.bridge.request("pin_create", pin="123456", name="PC-A")
        a.bridge.drop()
        b = self.env("NOTE")
        b.bridge.connect()
        for _ in range(4):
            self.assertEqual(b.bridge.request("auth", pin="000000", name="NOTE")["err"], "pin")
        r = b.bridge.request("auth", pin="000000", name="NOTE")  # 5a falha: 30 s de bloqueio
        self.assertEqual(r["err"], "pin")
        self.assertEqual(r["retry_s"], 30)
        r = b.bridge.request("auth", pin="123456", name="NOTE")  # PIN certo durante o bloqueio
        self.assertEqual(r["err"], "locked")
        self.assertEqual(b.bridge.state, "bloqueada")
        self.assertEqual(self.board.clients.keys(), {"PC-A"})
        self.board.fake_clock += 31
        self.assertTrue(b.bridge.request("auth", pin="123456", name="NOTE")["ok"])

    def test_lockout_grows_and_is_capped(self):
        b = self.board
        b.pin = "123456"
        steps = []
        for _ in range(12):
            b.fake_clock += 4000  # sempre fora do bloqueio anterior
            res, pen = b._check_pin("000000")
            steps.append(pen)
        self.assertEqual(steps[:7], [0, 0, 0, 0, 30, 60, 120])
        self.assertEqual(max(steps), 3600)

    def test_commands_need_a_session(self):
        e = self.env()
        e.bridge.connect()
        self.assertEqual(e.bridge.request("wifi_set", ssid="Casa", **{"pass": "senha1234"})["err"], "auth")
        self.assertEqual(self.board.wifi, [])

    def test_background_loop_pushes_metrics_and_shortcuts(self):
        e = self.env()
        e.bridge.connect()
        e.bridge.request("pin_create", pin="123456", name="PC-A")
        e.bridge.start()
        self.assertTrue(wait_for(lambda: self.board.metrics))
        self.assertEqual(self.board.metrics[0]["name"], "PC-A")
        self.assertEqual(self.board.metrics[0]["cpu"], 12.0)
        self.assertEqual(self.board.shortcuts, ["Mudo", "Vol +"])

    def test_shortcut_event_runs_the_local_action(self):
        e = self.env()
        e.bridge.connect()
        e.bridge.request("pin_create", pin="123456", name="PC-A")
        self.board.press_shortcut(1)  # com uma linha de log "ack" no meio, que deve ser ignorada
        self.assertTrue(wait_for(lambda: e.actions == [1]))

    def test_revoked_pc_has_to_enter_the_pin_again(self):
        a = self.env("PC-A")
        a.bridge.connect()
        a.bridge.request("pin_create", pin="123456", name="PC-A")
        self.assertTrue(a.bridge.request("revoke", name="PC-A")["ok"])
        a.bridge.drop()
        self.assertEqual(a.bridge.connect(), "precisa_pin")
        self.assertEqual(a.cfg["usb_token"], "")  # o token morto e descartado

    def test_factory_reset_clears_everything_and_the_local_token(self):
        e = self.env()
        e.bridge.connect()
        e.bridge.request("pin_create", pin="123456", name="PC-A")
        self.assertEqual(e.bridge.request("reset", pin="000000")["err"], "pin")  # pede o PIN de novo
        self.assertTrue(e.bridge.request("reset", pin="123456")["ok"])
        self.assertEqual(e.cfg["usb_token"], "")
        self.assertEqual((self.board.pin, self.board.clients, self.board.resets), (None, {}, 1))

    def test_pick_a_pc_and_back_to_auto(self):
        e = self.env()
        e.bridge.connect()
        e.bridge.request("pin_create", pin="123456", name="PC-A")
        e.bridge.request("pc_select", name="NOTE")
        self.assertEqual(self.board.pinned, "NOTE")
        e.bridge.request("pc_select", name="auto")
        self.assertEqual(self.board.pinned, "")

    def test_request_to_a_dead_port_raises(self):
        e = self.env()
        e.bridge.connect()
        e.bridge.link.close()
        with self.assertRaises(LinkError):
            e.bridge.request("hello")

    def test_a_failed_hello_does_not_leave_the_port_open(self):
        e = self.env()
        opened = []
        real_init = Link.__init__

        def spy(link, *a, **k):
            real_init(link, *a, **k)
            opened.append(link)

        Link.__init__ = spy
        real_request = Link.request
        Link.request = lambda self, cmd, **kw: (_ for _ in ()).throw(LinkError("sem resposta"))
        try:
            self.assertEqual(e.bridge.connect(), "sem_placa")
        finally:
            Link.__init__, Link.request = real_init, real_request
        self.assertTrue(opened and all(l.dead for l in opened))  # fechada, pronta para reabrir
        self.assertEqual(e.bridge.connect(), "precisa_criar_pin")

    def test_one_late_reply_does_not_drop_the_board(self):
        e = self.env()
        e.bridge.connect()
        e.bridge.request("pin_create", pin="123456", name="PC-A")
        real = Link.request
        calls = {"n": 0}

        def flaky(link, cmd, **kw):
            if cmd == "metrics" and calls["n"] < 2:
                calls["n"] += 1
                raise LinkError("a placa nao respondeu a 'metrics'")
            return real(link, cmd, **kw)

        Link.request = flaky
        try:
            e.bridge._tick()  # 1o atraso: segue
            e.bridge._tick()  # 2o atraso: segue
            self.assertTrue(e.bridge.authed)
            e.bridge._tick()  # volta a responder: zera a contagem
            self.assertEqual(e.bridge._misses, 0)
        finally:
            Link.request = real

    def test_three_late_replies_in_a_row_give_up(self):
        e = self.env()
        e.bridge.connect()
        e.bridge.request("pin_create", pin="123456", name="PC-A")
        real = Link.request
        Link.request = lambda link, cmd, **kw: (_ for _ in ()).throw(LinkError("atraso")) if cmd == "metrics" else real(link, cmd, **kw)
        try:
            e.bridge._tick()
            e.bridge._tick()
            with self.assertRaises(LinkError):
                e.bridge._tick()
        finally:
            Link.request = real

    def test_no_board_means_no_board(self):
        e = self.env()
        e.cfg["usb_port"] = "socket://127.0.0.1:1"  # nada escutando
        self.assertEqual(e.bridge.connect(), "sem_placa")


if __name__ == "__main__":
    unittest.main()
