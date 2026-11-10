import json,glob,re,sys,collections
sys.path.insert(0,"scripts")
from parse import cells, vrcell, VMRE, SKIPCELL
pref=sys.argv[1]; v=set(json.load(open(sys.argv[2]))["creators"]) if len(sys.argv)>2 else set()
C=collections.Counter(); EX={}
for f in glob.glob("raw/*.json"):
    d=json.load(open(f))
    if not re.match(pref,d["path"]): continue
    for r in d["lines"]:
        if r.get("h"): continue
        if not re.match(r'^00([1-9A-F][0-9A-F]|XX)$|^[1-9A-F][0-9A-F]XX$', r["el"]): continue
        if not re.search(r'(?i)creator|owner|reserv|identif|private', r["line"]): continue
        cs=[c for s,e,c in cells(r["line"]) if s>=r["tagpos"][1]-1]
        cs=[c.strip().strip('"“”\'‘’') for c in cs if not vrcell(c) and not VMRE.match(c.strip()) and not SKIPCELL.match(c.strip().strip('"“”'))]
        cs=[c for c in cs if re.search(r'[A-Za-z]{2}',c) and not re.search(r'(?i)^(private|creator|owner|reserv|identification|value:?|code|id)\b',c)]
        if cs:
            val=re.sub(r'^Value:\s*','',cs[-1] if len(cs)==1 else cs[0])
            if val not in v: C[val]+=1; EX.setdefault(val,(d["path"][:45],r["p"],re.sub(r'\s+',' ',r["line"])[:120]))
for k,n in C.most_common(): print(n,repr(k),EX[k])
