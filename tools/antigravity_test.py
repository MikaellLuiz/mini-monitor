"""Testa no PC a consulta de cota do Antigravity (o que a placa vai fazer).

Le o refresh token do Gerenciador de Credenciais, renova o access token e chama
fetchAvailableModels. Imprime so o formato e os percentuais, nunca tokens.
"""
import json
import urllib.error
import urllib.parse
import urllib.request

import antigravity_creds

CLIENT_ID = "1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps.googleusercontent.com"
CLIENT_SECRET = "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"  # publico: embutido no proprio IDE

rt = antigravity_creds.refresh_token()
print(f"refresh token lido: {len(rt)} caracteres")

data = urllib.parse.urlencode({"client_id": CLIENT_ID, "client_secret": CLIENT_SECRET,
                               "refresh_token": rt, "grant_type": "refresh_token"}).encode()
try:
    with urllib.request.urlopen(urllib.request.Request("https://oauth2.googleapis.com/token", data=data), timeout=20) as r:
        tok = json.loads(r.read())
except urllib.error.HTTPError as e:
    raise SystemExit(f"renovacao falhou {e.code}: {e.read().decode()[:300]}")
print("renovacao ok | campos:", sorted(tok.keys()), "| expires_in:", tok.get("expires_in"),
      "| refresh_token novo:", "refresh_token" in tok)


def fetch(host, body):
    req = urllib.request.Request(f"https://{host}/v1internal:fetchAvailableModels", data=json.dumps(body).encode(),
                                 headers={"Authorization": "Bearer " + tok["access_token"],
                                          "Content-Type": "application/json", "User-Agent": "antigravity"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")[:300]


for host in ("daily-cloudcode-pa.googleapis.com", "cloudcode-pa.googleapis.com"):
    st, res = fetch(host, {})
    print(f"\n== {host} (corpo vazio) -> {st}")
    if st == 200:
        models = res.get("models", {})
        print(f"{len(models)} modelos")
        for name, m in list(models.items()):
            q = m.get("quotaInfo")
            if q is not None:
                print(f"  {name}: quotaInfo={json.dumps(q)}")
    else:
        print("  ", res)
