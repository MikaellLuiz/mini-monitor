"""Envia o refresh token do Antigravity (Gerenciador de Credenciais do Windows) direto
para o painel, sem passar por copiar e colar.

Na placa: toque no card Antigravity > Configurar pelo navegador. A tela mostra o
codigo de 6 digitos. Depois:

    .venv\\Scripts\\python tools\\send_antigravity.py <codigo>

O script acha a placa sozinho na rede local (se preferir, passe o endereco como
segundo argumento). O token sai do seu PC direto para a placa; nao e impresso nem gravado.
"""
import concurrent.futures
import json
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

import antigravity_creds

if len(sys.argv) not in (2, 3):
    raise SystemExit(__doc__)
code = sys.argv[1]


def local_prefix():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("8.8.8.8", 80))  # nao envia nada: so descobre qual interface sai para a rede
    ip = s.getsockname()[0]
    s.close()
    return ip.rsplit(".", 1)[0]


def is_panel(ip):
    try:
        with urllib.request.urlopen(f"http://{ip}/status?p=agy", timeout=0.8) as r:
            return "attempts" in json.loads(r.read())
    except Exception:
        return False


if len(sys.argv) == 3:
    host = sys.argv[2].replace("http://", "").strip("/")
else:
    prefix = local_prefix()
    print(f"procurando o painel em {prefix}.0/24 ...")
    with concurrent.futures.ThreadPoolExecutor(max_workers=64) as pool:
        ips = [f"{prefix}.{i}" for i in range(1, 255)]
        found = [ip for ip, ok in zip(ips, pool.map(is_panel, ips)) if ok]
    if not found:
        raise SystemExit("nao achei o painel: toque em 'Configurar pelo navegador' no card do Antigravity "
                         "e confira que o PC esta na mesma rede (ou passe o endereco como 2o argumento)")
    host = found[0]
    print(f"painel encontrado em {host}")

rt = antigravity_creds.refresh_token()
print(f"refresh token lido ({len(rt)} caracteres), enviando...")
data = urllib.parse.urlencode({"code": code, "agy": rt}).encode()
try:
    with urllib.request.urlopen(urllib.request.Request(f"http://{host}/save", data=data), timeout=15) as r:
        r.read()
except urllib.error.HTTPError as e:
    raise SystemExit(f"a placa recusou ({e.code}): confira o codigo")

print("enviado; esperando a primeira consulta...")
status = None
for _ in range(40):
    time.sleep(1.5)
    try:
        with urllib.request.urlopen(f"http://{host}/status?p=agy", timeout=10) as r:
            status = json.loads(r.read())
    except Exception:
        continue
    if status["attempts"] > 0 and (status["ok"] or status["error"]):
        break
if status and status["ok"]:
    print(f"funcionou: {status['a_name']} {status['a']}% | {status['b_name']} {status['b']}%")
else:
    print("nao funcionou:", status and status["error"])
