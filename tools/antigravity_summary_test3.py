import json, urllib.parse, urllib.request, urllib.error
import antigravity_creds
CID="1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps.googleusercontent.com"; CS="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"
rt=antigravity_creds.refresh_token()
d=urllib.parse.urlencode({"client_id":CID,"client_secret":CS,"refresh_token":rt,"grant_type":"refresh_token"}).encode()
at=json.loads(urllib.request.urlopen(urllib.request.Request("https://oauth2.googleapis.com/token",data=d),timeout=20).read())["access_token"]
HOST="daily-cloudcode-pa.googleapis.com"
def call(m, body, ua):
    r=urllib.request.Request(f"https://{HOST}/v1internal:{m}",data=json.dumps(body).encode(),headers={"Authorization":"Bearer "+at,"Content-Type":"application/json","User-Agent":ua})
    try:
        with urllib.request.urlopen(r,timeout=20) as x: return x.status, json.loads(x.read())
    except urllib.error.HTTPError as e: return e.code, e.read().decode()[:200]
meta={"ideType":"ANTIGRAVITY","ideVersion":"2.25.0","platform":"WINDOWS_AMD64","pluginType":"GEMINI"}
for ua in ["antigravity","antigravity/2.25.0 windows/amd64","antigravity/2.25.0 (Windows NT 10.0; Win64; x64) Electron"]:
    st,la=call("loadCodeAssist",{"metadata":meta},ua)
    proj=la.get("cloudaicompanionProject") if isinstance(la,dict) else None
    if isinstance(proj,dict): proj=proj.get("id")
    st2,sm=call("retrieveUserQuotaSummary",{"project":proj},ua)
    groups=[g.get("displayName") for g in sm.get("groups",[])] if isinstance(sm,dict) else sm
    st3,fm=call("fetchAvailableModels",{"project":proj},ua)
    names=sorted(fm.get("models",{}).keys()) if isinstance(fm,dict) else []
    new=[n for n in names if "gemini-4" in n or "argon" in n.lower()]
    print(f"UA={ua!r}\n  grupos={groups}\n  modelos={len(names)} novos={new}")

ua="antigravity/2.25.0 windows/amd64"
st,la=call("loadCodeAssist",{"metadata":meta},ua)
proj=la.get("cloudaicompanionProject"); proj=proj.get("id") if isinstance(proj,dict) else proj
st,sm=call("retrieveUserQuotaSummary",{"project":proj},ua)
print("\n== resumo por grupo (UA novo)")
for g in sm.get("groups",[]):
    print(g.get("displayName"), "|", g.get("description"))
    for b in g.get("buckets",[]):
        print("   ",b.get("bucketId"),"| janela",b.get("window"),"| restante",b.get("remainingFraction"),"| quantidade",b.get("remainingAmount"),"| zera",b.get("resetTime"),"| disabled",b.get("disabled"))
st,fm=call("fetchAvailableModels",{"project":proj},ua)
print("\n== modelos gemini no fetchAvailableModels:", sorted(n for n in fm["models"] if "gemini" in n))
print("resposta do resumo tem", len(json.dumps(sm)), "bytes")
