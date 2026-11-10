import json,glob,re,sys,collections
pref=sys.argv[1]; v=set(json.load(open(sys.argv[2]))["creators"])
C=collections.Counter(); EX={}
RX=re.compile(r'(?i)private\s+(?:creator|group|owner)(?:\s+(?:identification|identifier|id|code|name|string))?\s*[:\-–(]?\s*["“(]?\s*([A-Za-z][A-Za-z0-9_ .:/\-]{1,63}?)\s*["”)]?\s*(?:\.{3,}|\s{2,}|$)')
for f in glob.glob("raw/*.json"):
    d=json.load(open(f))
    if not re.match(pref,d["path"]): continue
    for r in d["lines"]:
        if not r.get("h"): continue
        m=RX.search(r["line"])
        if m:
            val=m.group(1).strip()
            if val not in v: C[val]+=1; EX.setdefault(val,(d["path"][:40],r["p"],re.sub(r'\s+',' ',r["line"])[:110]))
for k,n in C.most_common(): print(n,repr(k),EX[k])
