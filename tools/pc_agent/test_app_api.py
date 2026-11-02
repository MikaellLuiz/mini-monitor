"""Testes da API da janela (app_api.py e ui_server.py) contra a placa falsa. Rode:

    python -m unittest tools/pc_agent/test_app_api.py -v
"""
import json
import os
import sys
import tempfile
import unittest
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import agent  # noqa: E402
from app_api import Api, valid_keys  # noqa: E402
from fake_board import FakeBoard  # noqa: E402
from ui_server import UiServer  # noqa: E402


class AppApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        agent.CONFIG_DIR = cls.tmp
        agent.CONFIG_PATH = os.path.join(cls.tmp, "config.json")
        cls.board = FakeBoard()
        cfg = agent.load_config()
        cfg.update(usb_port=cls.board.url, port=0, control_port=18776, name="PC-TESTE")
        agent.save_config(cfg)
        agent.CFG = agent.load_config()
        agent._cfg_mtime = os.path.getmtime(agent.CONFIG_PATH)
        agent.start_usb()
        cls.api = Api()
        cls.ui = UiServer(cls.api)
        cls.ui.start()

    @classmethod
    def tearDownClass(cls):
        agent.BRIDGE.stop()
        cls.ui.stop()
        cls.board.close()

    def post(self, method, args=(), token=None, host=None):
        req = urllib.request.Request(f"{self.ui.url}api/{method}", data=json.dumps(list(args)).encode(),
                                     headers={"X-Token": token if token is not None else self.ui.token})
        if host:
            req.add_header("Host", host)
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read())

    def test_1_new_board_asks_to_create_the_pin(self):
        s = self.api.state()
        self.assertEqual(s["usb"]["state"], "precisa_criar_pin")
        self.assertIsNone(s["board"])

    def test_2_create_pin_validates_and_authorizes(self):
        self.assertFalse(self.api.create_pin("12")["ok"])
        self.assertTrue(self.api.create_pin("123456")["ok"])
        s = self.api.state()
        self.assertEqual(s["usb"]["state"], "autenticado")
        self.assertEqual(s["board"]["clients"], ["PC-TESTE"])
        self.assertEqual(len(agent.CFG["usb_token"]), 32)

    def test_3_wifi_pc_selection_and_brightness(self):
        self.assertTrue(self.api.wifi_set("Casa", "senha1234")["ok"])
        self.assertEqual(self.board.wifi[-1], ("Casa", "senha1234"))
        self.assertTrue(self.api.select_pc("NOTEBOOK")["ok"])
        self.assertEqual(self.board.pinned, "NOTEBOOK")
        self.assertTrue(self.api.select_pc("")["ok"])
        self.assertEqual(self.board.pinned, "")

    def test_4_shortcuts_are_validated_and_saved(self):
        bad = self.api.save_shortcuts([{"label": "X", "keys": "ctrl+naoexiste"}])
        self.assertFalse(bad["ok"])
        self.assertFalse(self.api.save_shortcuts([])["ok"])
        self.assertFalse(self.api.save_shortcuts([{"label": " ", "keys": "f5"}])["ok"])
        ok = self.api.save_shortcuts([{"label": "Tarefas", "keys": "Ctrl+Shift+Esc"}, {"label": "Bloq", "action": "lock"}])
        self.assertTrue(ok["ok"])
        self.assertEqual(self.api.state()["shortcuts"], [{"label": "Tarefas", "keys": "ctrl+shift+esc"},
                                                          {"label": "Bloq", "action": "lock"}])

    def test_5_key_validation(self):
        self.assertTrue(valid_keys("ctrl+shift+esc"))
        self.assertTrue(valid_keys("a"))
        self.assertFalse(valid_keys("ctrl+"))
        self.assertFalse(valid_keys("ctrl+naoexiste"))

    def test_6_pair_code_must_have_six_digits(self):
        self.assertFalse(self.api.pair_wifi("12")["ok"])

    def test_7_http_api_needs_the_token_and_the_host(self):
        for kwargs in ({"token": "errado"}, {"token": ""}, {"host": "evil.example.com"}):
            with self.assertRaises(urllib.error.HTTPError) as cm:
                self.post("state", **kwargs)
            self.assertEqual(cm.exception.code, 403)
        self.assertEqual(self.post("state")["app"]["name"], "PC-TESTE")

    def test_8_private_methods_are_not_exposed(self):
        with self.assertRaises(urllib.error.HTTPError) as cm:
            self.post("_call", ["hello"])
        self.assertEqual(cm.exception.code, 404)

    def test_9_index_carries_the_token_and_static_files_are_whitelisted(self):
        with urllib.request.urlopen(self.ui.url, timeout=5) as r:
            html = r.read().decode()
        self.assertIn(self.ui.token, html)
        self.assertNotIn("__TOKEN__", html)
        with urllib.request.urlopen(self.ui.url + "tokens.css", timeout=5) as r:
            self.assertIn("--bg", r.read().decode())
        for bad in ("agent.py", "../agent.py", "ui_server.py"):
            with self.assertRaises(urllib.error.HTTPError):
                urllib.request.urlopen(self.ui.url + bad, timeout=5)

    def test_zz_reset_needs_the_pin(self):
        self.assertFalse(self.api.reset_board("000000")["ok"])
        self.assertTrue(self.api.reset_board("123456")["ok"])
        self.assertEqual(agent.BRIDGE.cfg["usb_token"], "")


if __name__ == "__main__":
    unittest.main()
