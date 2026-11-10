import os, re, sys, json, time
W = os.path.expanduser("~/work")
VRS = set("AE AS AT CS DA DS DT FL FD IS LO LT OB OD OF OL OV OW PN SH SL SQ SS ST SV TM UC UI UL UN UR US UT UV".split())
TAG = re.compile(r'(?<![0-9A-Za-z])\(?\s?([0-9A-Fa-f]{3}[13579BDFbdf])\s?,\s?([0-9A-Fa-fxX]{4})\s?\)?(?![0-9A-Za-z])')
VMRE = re.compile(r'^(\d+|n|\d+\s?-\s?\d*n|\d+\s?-\s?\d+)$')
SKIPCELL = re.compile(r'^(1C?|2C?|3|P|O|M|U|C|R|MC|UC|R\+|K|-|—|Yes|No|N/A|ALWAYS|VNAP|ANAP|EMPTY|AUTO|USER|CONFIG|CONDITIONAL|Zero [Ll]ength|IMAGE|SERIES|STUDY|PATIENT|GENERATED|SAFE|UNSAFE|REQUIRED|OPTIONAL|IGNORED|FIXED|NEVER|COPY|IMPORT|MWL|MPPS|CONDITION|SRC_COPY|SRC_INS|DEFAULT|NS|S)$', re.I)
def vrcell(c):
    c2 = c.strip().replace(" ", "")
    parts = [p for p in re.split(r'or|/|\||,', c2) if p]
    if parts and all(p in VRS for p in parts): return "/".join(parts)
    if c2.lower() in ("xs", "ox"): return c2
    return None
def cells(s, off=0):
    return [(m.start() + off, m.end() + off, m.group(0)) for m in re.finditer(r'\S+(?: \S+)*', s)]
def unq(c): return c.strip().strip('"“”\'‘’').strip()
def creator_like(c):
    c = unq(c)
    if not c or vrcell(c) or VMRE.match(c) or SKIPCELL.match(c): return None
    if not re.match(r'^[A-Za-z0-9][A-Za-z0-9_ .:/&+\-]{1,63}$', c): return None
    letters = [ch for ch in c if ch.isalpha()]
    if len(letters) < 2: return None
    if sum(ch.isupper() for ch in letters) / len(letters) >= 0.8 or "_" in c: return c
    return None
HKEYS = [("creator", r'(private\s+)?(owner(\s+code)?|creator(\s+(id|code|uid|string))?|implementor|private\s+group\s+id)'),
         ("name", r'(attribute\s+name|element\s+name|attribute|name|description|tag\s+name)'),
         ("vr", r'VR'), ("vm", r'VM'), ("tag", r'(tag(\s+id)?|element|group,\s*element|\(gggg,eeee\))')]
def header(line):
    l = line
    if not re.search(r'\bTag\b|\bElement\b', l, re.I): return None
    cols = []
    for (s, e, c) in cells(l):
        cc = c.strip()
        for k, rx in HKEYS:
            if re.fullmatch(rx, cc, re.I) or (k == "vr" and cc in ("VR", "VR VM")) :
                cols.append((s, e, k)); break
    ks = {k for _, _, k in cols}
    if "tag" in ks and (len(ks) >= 3 or "creator" in ks or "vr" in ks): return sorted(cols)
    return None
def colof(hdr, s, e):
    best = None; bo = -1
    for i, (hs, he, k) in enumerate(hdr):
        ns = hdr[i + 1][0] if i + 1 < len(hdr) else 10 ** 6
        lo, hi = hs - 3, ns - 1
        ov = min(e, hi) - max(s, lo)
        if ov > bo: bo = ov; best = (i, k)
    return best if bo > 0 else None
def parse(idv):
    t = open(os.path.join(W, "text", idv + ".txt"), encoding="utf-8", errors="replace").read()
    pages = t.split("\f")
    rows = []; ctx = {}; grp_ctx = {}; dropped = 0; hdr = None; decl = set()
    for pi, pg in enumerate(pages, 1):
        lines = pg.split("\n")
        for li, line in enumerate(lines):
            h = header(line)
            if h: hdr = h; continue
            if re.match(r'^\s*\d+(\.\d+)+\s+[A-Z]', line): hdr = None  # new numbered section
            if "," not in line: continue
            ms = [m for m in TAG.finditer(line) if m.group(1).upper() not in ("0001","0003","0005","0007") and not m.group(1).upper().startswith("FFF")]
            if not ms: continue
            m = ms[0]; g = m.group(1).upper(); el = m.group(2).upper()
            cs = [c for c in cells(line) if not (c[0] <= m.start() + 1 and c[1] >= m.end() - 1) and not any(c[0] <= x.start() + 1 and c[1] >= x.end() - 1 for x in ms[1:])]
            # split cells that contain the tag text
            cs2 = []
            for (s, e, c) in cells(line):
                if s <= m.start() < e or s < m.end() <= e:
                    pre = line[s:m.start()].strip(); post = line[m.end():e].strip()
                    if pre: cs2.append((s, s + len(pre), pre))
                    if post: cs2.append((e - len(post), e, post))
                elif any(s <= x.start() < e for x in ms[1:]): continue
                else: cs2.append((s, e, c))
            cs = cs2
            colmap = {}
            if hdr:
                for (s, e, c) in cs:
                    r = colof(hdr, s, e)
                    k = r[1] if r else None
                    # glued creator+name: cell spanning creator col into name col
                    if k == "creator":
                        i = r[0]
                        if i + 1 < len(hdr) and e > hdr[i + 1][0] + 1 and hdr[i + 1][2] == "name":
                            cut = hdr[i + 1][0] - s
                            sp = line[s:e].rfind(" ", 0, cut + 2)
                            if sp > 0:
                                colmap.setdefault("creator", []).append(line[s:s + sp].strip())
                                colmap.setdefault("name", []).append(line[s + sp:e].strip()); continue
                    colmap.setdefault(k, []).append(c)
            allc = [c for _, _, c in cs]
            vr = None; vm = None; cre = None; nm = None; note = []
            for c in colmap.get("vr", []) + allc:
                if vrcell(c): vr = vrcell(c); break
            for c in colmap.get("vm", []):
                if VMRE.match(c.strip()): vm = c.strip(); break
            if vm is None and vr:
                aft = [c for (s, e, c) in cs if s > m.end()]
                for i, c in enumerate(aft):
                    if vrcell(c) and i + 1 < len(aft) and VMRE.match(aft[i + 1].strip()): vm = aft[i + 1].strip(); break
            is_slot = re.match(r'^00([1-9A-F][0-9A-F]|XX)$', el) is not None
            is_block = re.match(r'^[1-9A-F][0-9A-F]XX$', el) is not None
            if is_slot or is_block:
                cand = None
                for c in sorted(allc, key=lambda c: 0 if re.search(r'^["“\']', c.strip()) else 1):
                    if re.search(r'private|creator|reservation|identification|owner|code', c, re.I) and not re.search(r'["“]', c): continue
                    cl = creator_like(c)
                    if cl: cand = cl; break
                if cand:
                    blk = el[2:] if is_slot else el[:2]
                    if blk != "XX": ctx[(g, blk)] = (cand, pi)
                    grp_ctx[g] = (cand, pi); decl.add(cand)
                continue
            if re.match(r'^XX[0-9A-F]{2}$', el): low = el[2:]; blk = None
            elif re.match(r'^[1-9A-F][0-9A-F][0-9A-F]{2}$', el): low = el[2:]; blk = el[:2]
            else: dropped += 1; continue
            src = ""
            if colmap.get("creator"):
                c0 = " ".join(colmap["creator"])
                # wrapped creator: continuation on next lines in creator column
                if hdr:
                    ci = [i for i, x in enumerate(hdr) if x[2] == "creator"][0]
                    for nl in lines[li + 1: li + 3]:
                        if TAG.search(nl) or not nl.strip(): break
                        frag = [c for (s, e, c) in cells(nl) if (colof(hdr, s, e) or (0, ""))[0] == ci]
                        if frag and len(cells(nl)) <= 3:
                            c0 = (c0 + frag[0]) if c0.endswith("-") else (c0 + " " + frag[0]); note.append("creator wrapped over lines in source")
                        else: break
                cl = creator_like(c0)
                if cl: cre = cl; src = "column"
            if not cre and not hdr:
                for c in allc:
                    if re.match(r'^["“].+["”]$', c.strip()) and creator_like(c): cre = creator_like(c); src = "quoted"; break
                    if creator_like(c) and creator_like(c) in decl: cre = creator_like(c); src = "declared"; break
            if not cre and blk and (g, blk) in ctx:
                cre, p0 = ctx[(g, blk)]; src = "ctx"; note.append("creator from (%s,00%s) declaration on p%d" % (g, blk, p0))
            if not cre and blk is None and g in grp_ctx:
                cre, p0 = grp_ctx[g]; src = "ctx"; note.append("creator taken from nearest preceding declaration for group %s on p%d" % (g, p0))
            def isname(c):
                c = c.strip()
                if not re.search(r'[A-Za-z]{2}', c): return False
                if vrcell(c) or VMRE.match(c) or SKIPCELL.match(c): return False
                if cre and unq(c) == cre: return False
                return True
            if colmap.get("name"):
                nm = " ".join(c for c in colmap["name"] if isname(c)) or None
            if nm is None:
                bef = [c for (s, e, c) in cs if e <= m.start()]
                aft = [c for (s, e, c) in cs if s >= m.end()]
                if bef and isname(bef[-1]): nm = bef[-1]
                elif not hdr:
                    for c in aft:
                        if isname(c): nm = c; break
            if nm: nm = nm.lstrip(">").strip()
            if not vr and not cre: dropped += 1; continue
            retired = "yes" if re.search(r'\bretired\b', line, re.I) else ""
            rows.append(dict(group=g, element_xx=low, block=blk or "", creator=cre or "", creator_src=src,
                             name=nm or "", vr=vr or "", vm=vm or "", retired=retired, page=pi, hdr=1 if hdr else 0, notes="; ".join(note)))
    return rows, dropped, len(pages), sorted(decl)
if __name__ == "__main__":
    ids = [l.split("\t") for l in open(os.path.join(W, "idmap.tsv")).read().splitlines()]
    pref = sys.argv[1]; budget = float(sys.argv[2]) if len(sys.argv) > 2 else 150
    T0 = time.time(); n = 0; left = 0
    for idv, path, _ in ids:
        if not re.match(pref, path): continue
        of = os.path.join(W, "parsed", idv + ".json")
        if os.path.exists(of) and "-f" not in sys.argv: continue
        if time.time() - T0 > budget: left += 1; continue
        rows, dropped, np, decl = parse(idv)
        json.dump({"path": path, "rows": rows, "dropped": dropped, "pages": np, "declared": decl}, open(of, "w")); n += 1
    print("parsed", n, "left", left)
