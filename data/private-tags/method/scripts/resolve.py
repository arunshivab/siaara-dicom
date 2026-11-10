import os, re, sys, json, glob, collections
sys.path.insert(0, os.path.dirname(__file__))
from parse import TAG, VRS, VMRE, SKIPCELL, vrcell, cells, unq, creator_like
W = os.path.expanduser("~/work")
STOP = set("HEADER OOG MEDCOM REPORT CSA NON-IMAGE ENVELOPE PRIVATE PORT WORKFLOW NA GENERATE COPIED ADVANCED CONDITIONA CONDITI ECA LOPE PROJECTION COREGISTRATION SRC_COP".split())
def clean(c):
    c = re.sub(r'^(Value:\s*)', '', c).strip()
    c = re.sub(r'^(%s)\s+' % "|".join(VRS), '', c)
    c = re.sub(r'(\s+(1C|2C|1|2|3|0|A|ALWAYS|AUTO))+$', '', c).strip()
    return c
def build_vocab(docs):
    cnt = collections.Counter(); srcs = collections.defaultdict(set); ndoc = collections.defaultdict(set)
    for path, d in docs:
        for c, s in d["cand"]:
            c = clean(c)
            if not c or c.upper() in STOP or c.endswith("-") or len(c) < 3: continue
            if not creator_like(c): continue
            cnt[c] += 1; srcs[c].add(s); ndoc[c].add(path)
    strong = {c for c in cnt if srcs[c] & {"slot", "quoted_slot"}}
    vocab = {}
    for c in cnt:
        weak = not (srcs[c] & {"slot", "quoted_slot"})
        if weak:
            # fragment of a longer candidate?
            if any(c != o and re.search(r'(^|\s)' + re.escape(c) + r'($|\s)', o) for o in cnt if cnt[o] >= 2): continue
            # glued: strong creator + extra words
            if any(c.startswith(o + " ") for o in strong): continue
            if cnt[c] < 2: continue
        vocab[c] = dict(n=cnt[c], docs=len(ndoc[c]), src=sorted(srcs[c]))
    return vocab
def spans(line, sub):
    out = []; i = line.find(sub)
    while i >= 0:
        out.append(i); i = line.find(sub, i + 1)
    return out
def col_text(l, x0, x1):
    # cells of l that start within [x0-2, x1]
    return [c for s, e, c in cells(l) if x0 - 3 <= s <= x1]
FAMS = []
def family(rec):
    lines = ([rec["prev"]] if rec["prev"] else []) + [rec["line"]] + rec["next"]
    for fam in FAMS:
        for k, l in enumerate(lines):
            i = l.find(fam)
            if i < 0: continue
            if l is rec["line"] and i < rec["tagpos"][1] and rec["tagpos"][0] > i: continue
            acc = None
            for l2 in lines[k:]:
                ct = [c for s, e, c in cells(l2) if i - 2 <= s <= i + 2]
                if not ct:
                    if l2 is rec["line"]: continue
                    break
                c = ct[0]
                if acc is None: acc = c
                elif creator_like(c) and not vrcell(c): acc = (acc + c) if acc.endswith("-") else (acc + " " + c)
                else: break
            if acc: return acc.strip()
    return None
def find_creator(rec, vocab, vl):
    line = rec["line"]; te = rec["tagpos"][1]
    if rec.get("inline"): return rec["inline"], "inline", None
    fv = family(rec)
    if fv: return fv, "assembled", None
    for v in vl:
        for i in spans(line, v):
            before_ok = i == 0 or not line[i-1].isalnum()
            after_ok = i + len(v) >= len(line) or not (line[i+len(v)].isalnum())
            if before_ok and after_ok:
                longer = [w for w in vl if w.startswith(v + " ")]
                if longer:
                    for nl in rec["next"][:2] + ([rec["prev"]] if rec["prev"] else []):
                        ct = [c for s2, e2, c in cells(nl) if i - 3 <= s2 <= i + 3]
                        if ct:
                            for w in longer:
                                if (v + " " + ct[0]).startswith(w) or w == v + " " + ct[0]:
                                    return w, "wrapped", i
                return v, "exact", i
    # wrapped: fragment on line + neighbours
    for (s, e, c) in cells(line):
        if s < te: continue
        c0 = c.strip()
        if len(c0) < 3: continue
        prevs = col_text(rec["prev"], s, e) if rec["prev"] else []
        nexts = []
        for nl in rec["next"]:
            ct = col_text(nl, s, e)
            if ct: nexts.append(ct[0])
            else: break
        combos = []
        def j(a, b): return (a + b) if a.endswith("-") else (a + " " + b)
        acc = c0
        for n in nexts:
            acc = j(acc, n); combos.append(acc)
        if prevs:
            pc = prevs[0]; combos.append(j(pc, c0)); acc = j(pc, c0)
            for n in nexts:
                acc = j(acc, n); combos.append(acc)
        combos = combos + [x.replace(" ", "") for x in combos]
        for cmb in combos:
            for v in vl:
                if cmb == v or cmb.startswith(v + " ") or cmb.endswith(" " + v) or (" " + v + " ") in (" " + cmb + " "):
                    return v, "wrapped", s
    return None, None, None
def resolve_doc(path, d, vocab, vl):
    rows = []; ctx = {}; grp = {}; dropped = 0; hctx = None
    alld = {}
    for rec in d["lines"]:
        if rec.get("h"): continue
        if re.match(r'^00([1-9A-F][0-9A-F]|XX)$|^[1-9A-F][0-9A-F]XX$', rec["el"]):
            v0, how0, _ = find_creator(rec, vocab, vl)
            if v0: alld.setdefault(rec["g"], {}).setdefault(v0, rec["p"])
    for rec in d["lines"]:
        if rec.get("h"):
            hv = None
            for v in vl:
                i = rec["line"].find(v)
                if i >= 0 and (i == 0 or not rec["line"][i-1].isalnum()) and (i + len(v) >= len(rec["line"]) or not rec["line"][i+len(v)].isalnum()):
                    hv = v; break
            if hv: hctx = {"c": hv, "p": rec["p"], "g": None}
            elif re.match(r'^\s*[A-Z]?\.?\d+(\.\d+)+\.?\s+\S', rec["line"]): hctx = None
            continue
        g, el, line = rec["g"], rec["el"], rec["line"]
        slot = re.match(r'^00([1-9A-F][0-9A-F]|XX)$', el); blk = re.match(r'^([1-9A-F][0-9A-F])XX$', el)
        if slot or blk:
            v, how, _ = find_creator(rec, vocab, vl)
            if v:
                b = el[2:] if slot else el[:2]
                if b != "XX": ctx[(g, b)] = (v, rec["p"])
                grp[g] = (v, rec["p"])
            continue
        if re.match(r'^XX[0-9A-F]{2}$', el): low, b = el[2:], None
        elif re.match(r'^[1-9A-F][0-9A-F][0-9A-F]{2}$', el): low, b = el[2:], el[:2]
        else: dropped += 1; continue
        note = []
        v, how, cpos = find_creator(rec, vocab, vl)
        if how == "wrapped": note.append("creator text wraps over lines in source")
        if how == "assembled": note.append("creator assembled from wrapped lines; verify")
        if not v and b and (g, b) in ctx:
            v, p0 = ctx[(g, b)]; how = "decl"; note.append("creator from (%s,00%s) declaration p%d" % (g, b, p0))
        if not v and hctx and (hctx["g"] is None or hctx["g"] == g):
            hctx["g"] = g; v = hctx["c"]; how = "heading"; note.append("creator from section heading p%d" % hctx["p"])
        if not v and b is None and g in grp:
            v, p0 = grp[g]; how = "ctx"; note.append("creator assumed from preceding group %s declaration p%d" % (g, p0))
        if not v and g in alld and len(alld[g]) == 1:
            v, p0 = list(alld[g].items())[0]; how = "docdecl"; note.append("creator from the document's only declaration for group %s (p%d)" % (g, p0))
        ts, te = rec["tagpos"]
        cs = cells(line)
        aft = [(s, e, c) for s, e, c in cs if s >= te]
        bef = [(s, e, c) for s, e, c in cs if e <= ts]
        vr = None; vm = None
        for s, e, c in aft + bef:
            if cpos is not None and how == "exact" and s <= cpos < e and not vrcell(c): continue
            x = vrcell(c)
            if x: vr = x; break
            # VR glued with VM "LO 1"
            mm = re.match(r'^([A-Z]{2}(?:\s?(?:or|/)\s?[A-Z]{2})?)\s+(\d+(?:-\d*n|-\d+)?|n)$', c.strip())
            if mm and vrcell(mm.group(1)): vr = vrcell(mm.group(1)); vm = mm.group(2); break
        if vr and vm is None:
            toks = [c.strip() for s, e, c in aft]
            for i, c in enumerate(toks):
                if vrcell(c) and i + 1 < len(toks) and VMRE.match(toks[i+1]): vm = toks[i+1]; break
                for part in c.split():
                    pass
        def isname(c):
            c = c.strip()
            if not re.search(r'[A-Za-z]{2}', c): return False
            if vrcell(c) or VMRE.match(c) or SKIPCELL.match(c): return False
            if v and (c == v or v in c and len(c) - len(v) < 3): return False
            return True
        nm = None
        if bef and isname(bef[-1][2]): nm = bef[-1][2]
        if nm is None:
            for s, e, c in aft:
                c2 = c
                if v and v in c2: c2 = c2.replace(v, " ").strip()
                if vr: c2 = re.sub(r'\s+%s(\s+\S+)?$' % re.escape(vr.split("/")[0]), '', c2).strip()
                if isname(c2): nm = c2; break
        if nm: nm = re.sub(r'^[>\s]+', '', nm).strip()
        if nm and v and v.startswith(nm) and nm != v:
            nm = None; note.append("name column held a wrapped piece of the creator; name left blank")
        if not vr and not v: dropped += 1; continue
        if not vr and how not in ("exact", "wrapped", "inline", "assembled", "heading"): dropped += 1; continue
        retired = "yes" if re.search(r'\bretired\b', line + " " + " ".join(rec["next"][:1]), re.I) else ""
        rows.append(dict(group=g, element_xx=low, block=b or "", creator=v or "", how=how or "", name=nm or "",
                         vr=vr or "", vm=vm or "", retired=retired, page=rec["p"], notes="; ".join(note)))
    return rows, dropped
def load(prefix_re):
    docs = []
    for f in sorted(glob.glob(os.path.join(W, "raw", "*.json"))):
        d = json.load(open(f))
        if re.match(prefix_re, d["path"]): docs.append((d["path"], d))
    return docs
if __name__ == "__main__":
    docs = load(sys.argv[1])
    vj = json.load(open(os.path.join(W, "vocab", sys.argv[2] + ".json")))
    vocab = {c: 1 for c in vj["creators"]}; FAMS[:] = vj.get("families", [])
    for f2 in glob.glob(os.path.join(W, "vocab", "*.json")):
        for c in json.load(open(f2))["creators"]: vocab.setdefault(c, 1)
    vl = sorted(vocab, key=lambda x: -len(x))
    json.dump(vocab, open(os.path.join(W, sys.argv[2] + "_vocab.json"), "w"), indent=0)
    res = {}
    tot = 0; dr = 0
    for path, d in docs:
        rows, dropped = resolve_doc(path, d, vocab, vl); res[path] = {"rows": rows, "dropped": dropped}; tot += len(rows); dr += dropped
    json.dump(res, open(os.path.join(W, sys.argv[2] + "_rows.json"), "w"))
    print("vocab", len(vocab), "rows", tot, "dropped", dr)
