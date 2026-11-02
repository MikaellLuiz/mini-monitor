import json, urllib.parse, urllib.request, urllib.error
import antigravity_creds
CID="1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps.googleusercontent.com"; CS="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"
rt=antigravity_creds.refresh_token()
d=urllib.parse.urlencode({"client_id":CID,"client_secret":CS,"refresh_token":rt,"grant_type":"refresh_token"}).encode()
at=json.loads(urllib.request.urlopen(urllib.request.Request("https://oauth2.googleapis.com/token",data=d),timeout=20).read())["access_token"]
def call(m, body, host="daily-cloudcode-pa.googleapis.com"):
    r=urllib.request.Request(f"https://{host}/v1internal:{m}",data=json.dumps(body).encode(),headers={"Authorization":"Bearer "+at,"Content-Type":"application/json","User-Agent":"antigravity"})
    try:
        with urllib.request.urlopen(r,timeout=20) as x: return x.status, json.loads(x.read())
    except urllib.error.HTTPError as e: return e.code, e.read().decode()[:300]
st,la=call("loadCodeAssist",{"metadata":{"ideType":"ANTIGRAVITY"}})
print("loadCodeAssist",st, "chaves:", sorted(la.keys()) if isinstance(la,dict) else la)
proj=la.get("cloudaicompanionProject") if isinstance(la,dict) else None
if isinstance(proj,dict): proj=proj.get("id")
print("projeto:", (proj[:8]+"...") if proj else None, "| tier:", (la.get("currentTier") or {}).get("id") if isinstance(la,dict) else None)
st,sm=call("retrieveUserQuotaSummary",{"project":proj})
print("summary com projeto ->",st)
if isinstance(sm,dict):
    for g in sm.get("groups",[]):
        print(" grupo:",g.get("displayName"))
        for b in g.get("buckets",[]):
            print("   ",b.get("bucketId"),b.get("window"),"restante",b.get("remainingFraction"),"reset",b.get("resetTime"),"disabled",b.get("disabled"))
else: print(sm)
