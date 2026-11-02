"""Testa retrieveUserQuotaSummary (o endpoint da tela 'View Usage' do Antigravity).
Imprime so a estrutura e os numeros de cota, nunca tokens."""
import json
import urllib.error
import urllib.parse
import urllib.request

import antigravity_creds

CLIENT_ID = "1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps.googleusercontent.com"
CLIENT_SECRET = "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"

rt = antigravity_creds.refresh_token()
data = urllib.parse.urlencode({"client_id": CLIENT_ID, "client_secret": CLIENT_SECRET,
                               "refresh_token": rt, "grant_type": "refresh_token"}).encode()
with urllib.request.urlopen(urllib.request.Request("https://oauth2.googleapis.com/token", data=data), timeout=20) as r:
    at = json.loads(r.read())["access_token"]


def call(host, method, body):
    req = urllib.request.Request(f"https://{host}/v1internal:{method}", data=json.dumps(body).encode(),
                                 headers={"Authorization": "Bearer " + at, "Content-Type": "application/json",
                                          "User-Agent": "antigravity"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")


for host in ("daily-cloudcode-pa.googleapis.com", "cloudcode-pa.googleapis.com"):
    for method in ("retrieveUserQuotaSummary", "retrieveUserQuota"):
        st, body = call(host, method, {})
        print(f"\n== {host} {method} -> {st} ({len(body)} bytes)")
        print(body[:2500])
