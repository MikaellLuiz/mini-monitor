"""Painel CYD: o programa do PC. Agente em segundo plano (metricas e atalhos por USB e Wi-Fi), icone
na bandeja e uma janela para configurar a placa, sem rodar script nenhum.

    python painel_app.py              abre a janela
    python painel_app.py --minimized  so a bandeja (e o que a inicializacao com o sistema usa)
    python painel_app.py --browser    usa o navegador padrao no lugar da janela

Windows: janela pywebview (WebView2). Linux: o navegador padrao e a bandeja por padrao, porque a
janela pywebview exige pacotes GTK ou Qt; use --window se os tiver.
"""
import argparse
import os
import socket
import sys
import threading
import time
import webbrowser

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import agent  # noqa: E402
import autostart  # noqa: E402
from app_api import Api  # noqa: E402
from ui_server import UiServer  # noqa: E402

LOCK_PORT = 8767  # so uma copia do programa: a segunda so pede para a primeira mostrar a janela


class Instance:
    """Instancia unica por socket local."""

    def __init__(self, on_show):
        self.on_show = on_show
        self.sock = None

    def acquire(self):
        try:
            s = socket.socket()
            s.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1) if hasattr(socket, "SO_EXCLUSIVEADDRUSE") else None
            s.bind(("127.0.0.1", LOCK_PORT))
            s.listen(2)
        except OSError:
            try:
                with socket.create_connection(("127.0.0.1", LOCK_PORT), timeout=2) as c:
                    c.sendall(b"show\n")
            except OSError:
                pass
            return False
        self.sock = s
        threading.Thread(target=self._serve, daemon=True, name="instance").start()
        return True

    def _serve(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            with conn:
                try:
                    if conn.recv(16).startswith(b"show"):
                        self.on_show()
                except OSError:
                    pass


def tray_image():
    """Icone: o cartao do tema do aparelho com o azul de destaque (mesmos tokens)."""
    from PIL import Image, ImageDraw
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((4, 8, 60, 56), 10, fill=(31, 42, 58, 255), outline=(138, 180, 248, 255), width=3)
    for i, (w, c) in enumerate(((34, (138, 180, 248)), (26, (246, 178, 107)), (40, (109, 213, 140)))):
        y = 20 + i * 10
        d.rounded_rectangle((12, y, 12 + w, y + 5), 2, fill=c + (255,))
    return img


class App:
    def __init__(self, args):
        self.args = args
        self.api = Api()
        self.ui = UiServer(self.api)
        self.window = None
        self.icon = None
        self.quitting = False

    # ---- janela -----------------------------------------------------------------

    def show(self):
        if self.window:
            try:
                self.window.show()
                self.window.restore()
                return
            except Exception:
                pass
        webbrowser.open(self.ui.url)

    def quit(self, *_):
        self.quitting = True
        if self.icon:
            self.icon.stop()
        if self.window:
            try:
                self.window.destroy()
            except Exception:
                pass
        agent.stop_services()
        self.ui.stop()
        if not self.window:
            os._exit(0)

    # ---- bandeja ----------------------------------------------------------------

    def make_tray(self):
        try:
            import pystray
            from pystray import Menu, MenuItem as Item
        except Exception as e:
            print("aviso: sem icone na bandeja:", e)
            return None
        menu = Menu(
            Item("Abrir o Painel CYD", lambda *_: self.show(), default=True),
            Item("Iniciar com o sistema", lambda *_: autostart.set_enabled(not autostart.is_enabled()),
                 checked=lambda _: autostart.is_enabled()),
            Item("Sair", self.quit))
        return pystray.Icon("painel-cyd", tray_image(), "Painel CYD", menu)

    # ---- partida ----------------------------------------------------------------

    def run(self):
        agent.start_services()
        url = self.ui.start()
        print("interface em", url)
        self.icon = None if self.args.no_tray else self.make_tray()

        use_window = not self.args.browser and (sys.platform == "win32" or self.args.window)
        if use_window:
            try:
                import webview
            except Exception as e:
                print("aviso: sem janela (pywebview indisponivel):", e)
                use_window = False
        if use_window:
            if self.icon:
                self.icon.run_detached()
            self.window = webview.create_window("Painel CYD", url, width=760, height=760, min_size=(520, 560),
                                                hidden=self.args.minimized, background_color="#0A0C0F")

            def closing():
                if self.quitting or not self.icon:
                    return True
                self.window.hide()  # fechar a janela so a esconde: o agente segue na bandeja
                return False

            self.window.events.closing += closing
            webview.start()
            if not self.quitting:
                self.quit()
            return

        # navegador + bandeja (Linux) ou so o agente
        if not self.args.minimized:
            webbrowser.open(url)
        if self.icon:
            self.icon.run()  # bloqueia ate "Sair"
        else:
            try:
                while not self.quitting:
                    time.sleep(1)
            except KeyboardInterrupt:
                pass
        self.quit()


def main():
    ap = argparse.ArgumentParser(description="Painel CYD")
    ap.add_argument("--minimized", action="store_true", help="comeca so na bandeja")
    ap.add_argument("--browser", action="store_true", help="usa o navegador padrao no lugar da janela")
    ap.add_argument("--window", action="store_true", help="forca a janela pywebview (Linux com GTK/Qt)")
    ap.add_argument("--no-tray", action="store_true", help="sem icone na bandeja")
    args = ap.parse_args()

    app = App(args)
    if not Instance(app.show).acquire():
        print("O Painel CYD ja esta rodando; pedi para ele mostrar a janela.")
        return
    app.run()


if __name__ == "__main__":
    main()
