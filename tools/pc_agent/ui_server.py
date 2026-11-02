"""Servidor local da interface: serve a pasta ui/ e atende POST /api/<metodo> em 127.0.0.1.

Uma pagina qualquer da web tambem consegue chamar 127.0.0.1, entao a API exige o token desta
execucao (vai so dentro do index.html, que outra origem nao consegue ler) e confere o Host contra
DNS rebinding. Sem o token certo, nada executa.
"""
import http.server
import json
import mimetypes
import os
import secrets
import sys
import threading

BASE = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
UI_DIR = os.path.join(BASE, "ui")
STATIC = {"app.css", "app.js", "tokens.css"}


class UiServer:
    def __init__(self, api):
        self.api = api
        self.token = secrets.token_urlsafe(24)
        self.httpd = None
        self.port = 0

    @property
    def url(self):
        return f"http://127.0.0.1:{self.port}/"

    def start(self):
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"

            def log_message(self, *a):
                pass

            def _host_ok(self):
                return self.headers.get("Host", "") in (f"127.0.0.1:{outer.port}", f"localhost:{outer.port}")

            def _send(self, code, body, ctype):
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if not self._host_ok():
                    return self._send(403, b"proibido", "text/plain")
                name = self.path.split("?")[0].lstrip("/") or "index.html"
                if name == "index.html":
                    with open(os.path.join(UI_DIR, "index.html"), encoding="utf-8") as f:
                        html = f.read().replace("__TOKEN__", outer.token)
                    return self._send(200, html.encode(), "text/html; charset=utf-8")
                if name in STATIC:
                    with open(os.path.join(UI_DIR, name), "rb") as f:
                        return self._send(200, f.read(), (mimetypes.guess_type(name)[0] or "text/plain") + "; charset=utf-8")
                self._send(404, b"nao encontrado", "text/plain")

            def do_POST(self):
                import hmac
                try:  # le o corpo antes de responder: recusar sem le-lo faz o Windows cortar a conexao
                    raw = self.rfile.read(min(int(self.headers.get("Content-Length", "0")), 65536))
                except (ValueError, OSError):
                    raw = b""
                if (not self._host_ok() or not self.path.startswith("/api/")
                        or not hmac.compare_digest(self.headers.get("X-Token", ""), outer.token)):
                    return self._send(403, b'{"ok":false,"msg":"proibido"}', "application/json")
                name = self.path[5:]
                fn = None if name.startswith("_") else getattr(outer.api, name, None)
                if not callable(fn):
                    return self._send(404, b'{"ok":false,"msg":"metodo desconhecido"}', "application/json")
                try:
                    args = json.loads(raw or b"[]")
                    result = fn(*args)
                except Exception as e:  # erro do metodo vira mensagem na tela, nao derruba o servidor
                    result = {"ok": False, "msg": f"Erro interno: {e}"}
                self._send(200, json.dumps(result, ensure_ascii=False).encode(), "application/json; charset=utf-8")

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True, name="ui-http").start()
        return self.url

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
