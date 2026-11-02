"""Login no Spotify (PKCE) pelo PC, teste do player e envio do token para a placa.

Pre-requisito (uma vez): em https://developer.spotify.com/dashboard crie um app com
  Redirect URI: http://127.0.0.1:8888/callback      (localhost NAO e aceito)
  APIs usadas: Web API
O dono do app precisa ter Spotify Premium (regra do modo desenvolvimento, 2026).

Uso:
    .venv\\Scripts\\python tools\\spotify_login.py <client_id>              (so testa)
    .venv\\Scripts\\python tools\\spotify_login.py <client_id> <codigo>     (testa e envia a placa)

<codigo> e o de 6 digitos da tela do painel (card Spotify > Configurar pelo navegador).
O PKCE nao usa segredo. Tokens nao sao impressos nem gravados no PC.
"""
import base64
import concurrent.futures
import hashlib
import http.server
import json
import secrets
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

if len(sys.argv) not in (2, 3):
    raise SystemExit(__doc__)
CLIENT_ID = sys.argv[1]
BOARD_CODE = sys.argv[2] if len(sys.argv) == 3 else None
REDIRECT = "http://127.0.0.1:8888/callback"
SCOPES = "user-read-playback-state user-modify-playback-state user-read-currently-playing"

verifier = base64.urlsafe_b64encode(secrets.token_bytes(64)).rstrip(b"=").decode()
challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
state = secrets.token_urlsafe(12)
result = {}


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        if "code" in q or "error" in q:
            result.update({k: v[0] for k, v in q.items()})
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write("<h2>Pronto. Pode fechar esta aba e voltar ao terminal.</h2>".encode())
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *a):
        pass


server = http.server.HTTPServer(("127.0.0.1", 8888), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()

url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
    "response_type": "code", "client_id": CLIENT_ID, "scope": SCOPES, "redirect_uri": REDIRECT,
    "code_challenge_method": "S256", "code_challenge": challenge, "state": state})
print("abrindo o navegador para voce entrar no Spotify...")
webbrowser.open(url)
for _ in range(240):
    if result:
        break
    time.sleep(0.5)
server.shutdown()
if "code" not in result or result.get("state") != state:
    raise SystemExit(f"login nao concluido: {result.get('error', 'tempo esgotado')}")


def post_form(url, body):
    req = urllib.request.Request(url, data=urllib.parse.urlencode(body).encode(), method="POST")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")[:300]


st, tok = post_form("https://accounts.spotify.com/api/token", {
    "grant_type": "authorization_code", "code": result["code"], "redirect_uri": REDIRECT,
    "client_id": CLIENT_ID, "code_verifier": verifier})
if st != 200:
    raise SystemExit(f"troca do codigo falhou ({st}): {tok}")
print("login ok | campos:", sorted(tok.keys()), "| expira em", tok.get("expires_in"), "s | refresh_token:", "refresh_token" in tok)


def api(path, method="GET"):
    req = urllib.request.Request("https://api.spotify.com/v1" + path, method=method,
                                 headers={"Authorization": "Bearer " + tok["access_token"]})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            body = r.read()
            return r.status, (json.loads(body) if body else None)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")[:300]


st, me = api("/me")
print("GET /me ->", st, "| plano:", me.get("product") if isinstance(me, dict) else me)
st, pl = api("/me/player")
print("GET /me/player ->", st)
if st == 200 and pl:
    item = pl.get("item") or {}
    print("  tocando:", pl.get("is_playing"), "| faixa:", item.get("name"), "| artistas:",
          ", ".join(a["name"] for a in item.get("artists", [])), "| dispositivo:", (pl.get("device") or {}).get("name"))
    imgs = (item.get("album") or {}).get("images", [])
    print("  capas:", [(i.get("width"), i.get("height")) for i in imgs])
    small = min(imgs, key=lambda i: i.get("width") or 9999, default=None)
    if small:
        data = urllib.request.urlopen(small["url"], timeout=20).read()
        print(f"  menor capa {small.get('width')}px: {len(data)} bytes, JPEG={data[:2] == b'\xff\xd8'}")
elif st == 204:
    print("  nenhum dispositivo ativo (abra o Spotify e toque algo para o teste mostrar a faixa)")

if BOARD_CODE:
    print("procurando a placa na rede e enviando...")

    def local_prefix():
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip.rsplit(".", 1)[0]

    def is_panel(ip):
        try:
            with urllib.request.urlopen(f"http://{ip}/status?p=agy", timeout=0.8) as r:
                return "attempts" in json.loads(r.read())
        except Exception:
            return False

    prefix = local_prefix()
    with concurrent.futures.ThreadPoolExecutor(max_workers=64) as pool:
        ips = [f"{prefix}.{i}" for i in range(1, 255)]
        found = [ip for ip, ok in zip(ips, pool.map(is_panel, ips)) if ok]
    if not found:
        raise SystemExit("nao achei o painel: abra 'Configurar pelo navegador' no card do Spotify e confira a rede")
    host = found[0]
    data = urllib.parse.urlencode({"code": BOARD_CODE, "spotify_client": CLIENT_ID,
                                   "spotify_rt": tok["refresh_token"]}).encode()
    try:
        with urllib.request.urlopen(urllib.request.Request(f"http://{host}/save", data=data), timeout=15) as r:
            r.read()
    except urllib.error.HTTPError as e:
        raise SystemExit(f"a placa recusou ({e.code}): confira o codigo")
    print(f"enviado para {host}.")
