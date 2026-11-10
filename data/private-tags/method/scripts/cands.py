import os, re, sys, json, glob, collections
sys.path.insert(0, os.path.dirname(__file__))
from parse import creator_like
W = os.path.expanduser("~/work")
pref = sys.argv[1]
cnt = collections.Counter(); srcs = collections.defaultdict(set); ex = {}; nd = collections.defaultdict(set)
for f in sorted(glob.glob(os.path.join(W, "raw", "*.json"))):
    d = json.load(open(f))
    if not re.match(pref, d["path"]): continue
    for c, s in d["cand"]:
        c = re.sub(r'^Value:\s*', '', c).strip()
        cnt[c] += 1; srcs[c].add(s); nd[c].add(d["path"])
        if c not in ex:
            for r in d["lines"]:
                if c in r["line"]: ex[c] = (d["path"][:50], r["p"], re.sub(r'\s+', ' ', r["line"])[:150]); break
for c, n in sorted(cnt.items(), key=lambda x: -x[1]):
    print("%d\t%d\t%s\t%s\t%s" % (n, len(nd[c]), "|".join(sorted(srcs[c])), c, ex.get(c, "")))
