"""Testa no PC o login por codigo do Codex (o mesmo fluxo que a placa vai fazer).

Pede um codigo a auth.openai.com, voce o digita no celular em
https://auth.openai.com/codex/device, e o script troca por tokens e consulta o uso.
Cria uma sessao NOVA e independente: nao mexe no login do seu Codex CLI.
Nao imprime nem grava tokens; mostra so o resultado.

Uso: .venv\\Scripts\\python tools\\codex_device_test.py
"""
import base64
import json
import time
import urllib.error
import urllib.parse
import urllib.request

ISSUER = "https://auth.openai.com"
CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann"
UA = "painel-cyd-test/0.1"


def post(url, body, form=False, extra=None):
    data = urllib.parse.urlencode(body).encode() if form else json.dumps(body).encode()
    headers = {"Content-Type": "application/x-www-form-urlencoded" if form else "application/json",
               "User-Agent": UA}
    headers.update(extra or {})
    req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")


def jwt_claims(token):
    payload = token.split(".")[1]
    payload += "=" * (-len(payload) % 4)
    return json.loads(base64.urlsafe_b64decode(payload))


print("1. pedindo codigo...")
st, body = post(f"{ISSUER}/api/accounts/deviceauth/usercode", {"client_id": CLIENT_ID})
print("   status", st)
if st != 200:
    print("   ", body[:300])
    raise SystemExit("o login por codigo nao esta disponivel (ative em Configuracoes do ChatGPT > Seguranca)")
uc = json.loads(body)
interval = int(str(uc.get("interval", 5)).strip() or 5)
print(f"\n2. No celular abra {ISSUER}/codex/device e digite o codigo:\n\n      {uc.get('user_code') or uc.get('usercode')}\n")

print("3. esperando voce confirmar (ate 15 min)...")
deadline = time.time() + 15 * 60
auth = None
while time.time() < deadline:
    time.sleep(max(interval, 3))
    st, body = post(f"{ISSUER}/api/accounts/deviceauth/token",
                    {"device_auth_id": uc["device_auth_id"], "user_code": uc.get("user_code") or uc["usercode"]})
    if st == 200:
        auth = json.loads(body)
        break
    if st not in (403, 404):
        raise SystemExit(f"   erro {st}: {body[:300]}")
if not auth:
    raise SystemExit("   tempo esgotado")
print("   confirmado; campos:", sorted(auth.keys()))

print("4. trocando o codigo por tokens...")
exch = {"grant_type": "authorization_code", "code": auth["authorization_code"],
        "redirect_uri": f"{ISSUER}/deviceauth/callback", "client_id": CLIENT_ID,
        "code_verifier": auth["code_verifier"]}
st, body = post(f"{ISSUER}/oauth/token", exch, form=True)
print("   form ->", st)
if st != 200:
    print("   ", body[:300])
    raise SystemExit("troca falhou")
tok = json.loads(body)
print("   campos:", sorted(tok.keys()))
claims = jwt_claims(tok["id_token"])
account_id = claims.get("https://api.openai.com/auth", {}).get("chatgpt_account_id") or claims.get("chatgpt_account_id")
print("   account_id presente:", bool(account_id), "| refresh_token presente:", bool(tok.get("refresh_token")))

print("5. consultando o uso...")
req = urllib.request.Request("https://chatgpt.com/backend-api/wham/usage", headers={
    "Authorization": "Bearer " + tok["access_token"], "ChatGPT-Account-Id": account_id or "",
    "User-Agent": UA, "Accept": "application/json"})
try:
    with urllib.request.urlopen(req, timeout=20) as r:
        usage = json.loads(r.read())
except urllib.error.HTTPError as e:
    raise SystemExit(f"   uso falhou {e.code}: {e.read().decode(errors='replace')[:300]}")
print(json.dumps(usage, indent=2)[:1500])

print("6. testando a renovacao do token (refresh)...")
st, body = post(f"{ISSUER}/oauth/token", {"client_id": CLIENT_ID, "grant_type": "refresh_token",
                                          "refresh_token": tok["refresh_token"]})
print("   refresh ->", st, "| campos:", sorted(json.loads(body).keys()) if st == 200 else body[:200])
if st == 200:
    new = json.loads(body)
    print("   refresh_token mudou (rotacao):", new.get("refresh_token") != tok["refresh_token"])
