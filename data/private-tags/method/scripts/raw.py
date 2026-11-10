# pass 1: collect raw tag lines + high-confidence creator candidates per document
import os, re, sys, json, time
sys.path.insert(0, os.path.dirname(__file__))
from parse import TAG, VRS, VMRE, SKIPCELL, vrcell, cells, unq, creator_like, header
W = os.path.expanduser("~/work")
NORM1 = re.compile(r'\(([0-9A-Fa-f]{3}[13579BDFbdf]),\s*["“]?([A-Za-z][^,()"”]{1,63}?)["”]?\s*,\s*([0-9A-Fa-f]{2})\)')
NORM2 = re.compile(r'\(([0-9A-Fa-f]{3}[13579BDFbdf])\s?[xX]{2}([0-9A-Fa-f]{2})\)')
NORM3 = re.compile(r'\(\s?([0-9A-Fa-f]{3}[13579BDFbdf])\s?,\s?(?:yy|YY|zz|ZZ|nn|NN)([0-9A-Fa-f]{2})\s?\)')
NORM4 = re.compile(r'\(\s?([0-9A-Fa-f]{3}[13579BDFbdf])\s?,\s?([0-9A-Fa-f]{2})(?:yy|YY|zz|ZZ|nn|NN)\s?\)')
NORM5 = re.compile(r'\(\s?([0-9A-Fa-f]{3}[13579BDFbdf])\s?,\s?ee([0-9A-Fa-f]{2})\s?\)')
def nearest_col(hdr, s):
    best = None; bd = 10**6
    for hs, he, k in hdr:
        d = abs(s - hs)
        if d < bd: bd = d; best = k
    return best if bd <= 6 else None
def raw(idv):
    t = open(os.path.join(W, "text", idv + ".txt"), encoding="utf-8", errors="replace").read()
    pages = t.split("\f"); out = []; cand = []; hdr = None
    for pi, pg in enumerate(pages, 1):
        lines = pg.split("\n")
        for li, line in enumerate(lines):
            mi = NORM1.search(line)
            inl = mi.group(2).strip() if mi else None
            line = NORM1.sub(lambda m: "(%s,xx%s)   %s   " % (m.group(1), m.group(3), m.group(2).strip()), line)
            line = NORM5.sub(lambda m: "(%s,xx%s)" % (m.group(1), m.group(2)), line)
            line = NORM3.sub(lambda m: "(%s,xx%s)" % (m.group(1), m.group(2)), line)
            line = NORM4.sub(lambda m: "(%s,%sxx)" % (m.group(1), m.group(2)), line)
            line = NORM2.sub(lambda m: "(%s,xx%s)" % (m.group(1), m.group(2)), line)
            lines[li] = line
            h = header(line)
            if h: hdr = h; continue
            if not TAG.search(line) and line.strip() and (re.search(r'(?i)private|creator|owner|group|identification', line) or re.match(r'^\s*[A-Z]?\.?\d+(\.\d+)+\.?\s+\S', line)):
                out.append(dict(h=1, p=pi, line=line)); continue
            if "," not in line: continue
            ms = [m for m in TAG.finditer(line) if m.group(1).upper() not in ("0001","0003","0005","0007") and not m.group(1).upper().startswith("FFF")]
            if not ms: continue
            m = ms[0]; g = m.group(1).upper(); el = m.group(2).upper()
            cs = [c for c in cells(line[m.end():], m.end())]
            bef = [c for c in cells(line[:m.start()])]
            # neighbours (non-tag lines) for wrap handling
            prev = lines[li-1] if li > 0 and not TAG.search(lines[li-1]) else ""
            nxt = []
            for nl in lines[li+1:li+3]:
                if TAG.search(nl) or not nl.strip(): break
                nxt.append(nl)
            rec = dict(g=g, el=el, p=pi, line=line, prev=prev, next=nxt, tagpos=[m.start(), m.end()])
            if inl: rec["inline"] = inl
            if hdr:
                rec["hdr"] = [[s, k] for s, e, k in hdr]
                for s, e, c in bef + cs:
                    if nearest_col(hdr, s) == "creator":
                        cl = creator_like(c)
                        if cl: cand.append([cl, "hdrcol"])
            slot = re.match(r'^00([1-9A-F][0-9A-F]|XX)$', el) or re.match(r'^[1-9A-F][0-9A-F]XX$', el)
            for s, e, c in bef + cs:
                if re.match(r'^\s*["“\'‘].+["”\'’]\s*$', c) and creator_like(c):
                    cand.append([creator_like(c), "quoted_slot" if slot else "quoted"])
            if slot:
                for s, e, c in cs:
                    if re.search(r'private|creator|reservation|identification|owner|code', c, re.I): continue
                    cl = creator_like(c)
                    if cl: cand.append([cl, "slot"]); break
            out.append(rec)
    return {"lines": out, "cand": cand, "pages": len(pages)}
if __name__ == "__main__":
    os.makedirs(os.path.join(W, "raw"), exist_ok=True)
    ids = [l.split("\t") for l in open(os.path.join(W, "idmap.tsv")).read().splitlines()]
    pref = sys.argv[1]; budget = float(sys.argv[2]); T0 = time.time(); n = 0; left = 0
    for idv, path, _ in ids:
        if not re.match(pref, path): continue
        of = os.path.join(W, "raw", idv + ".json")
        if os.path.exists(of) and "-f" not in sys.argv: continue
        if time.time() - T0 > budget: left += 1; continue
        d = raw(idv); d["path"] = path; json.dump(d, open(of, "w")); n += 1
    print("raw", n, "left", left)
