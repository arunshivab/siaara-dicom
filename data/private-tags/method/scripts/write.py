import os, re, sys, json, glob, collections, csv
W = os.path.expanduser("~/work")
OUT = os.path.expanduser("~/mnt/DICOM Conformance Statement/_siaara_private_tags_output")
def meta(path, idv):
    fn = os.path.basename(path.split("!")[-1])
    base = re.sub(r'\.pdf$', '', fn, flags=re.I)
    t = open(os.path.join(W, "text", idv + ".txt"), encoding="utf-8", errors="replace").read().split("\f")
    head = "\n".join(t[:3])
    prod = re.sub(r'^.* - ', '', base) if " - " in base else base
    prod = re.sub(r'(?i)[_ -]*(dicom)?[_ -]*conformance[_ -]*statement.*$', '', prod)
    prod = re.sub(r'[-_](0\d{7}|1800\d+|\d{8,}).*$', '', prod)
    prod = prod.replace("_", " ").strip(" -")
    if re.fullmatch(r'(?i)[0-9a-f]{20,}( \(\d+\))?|[0-9A-Z]{2}\d{3}-\d{3}[A-Z]*|\d{6,}|mii[a-z]{2}\d{4}[a-z*]*|2g\d{3}-\d{3}[a-z]*', prod) or len(prod) < 3:
        p1 = [re.sub(r'\s{2,}', ' ', l.strip()) for l in t[0].split("\n") if l.strip()]
        prod = ""
        SKIP = re.compile(r'(?i)^(for$|no\. |\(mii|dicom|conformance|statement|©|copyright|all rights|document|doc |doc id|doc status|page |philips$|medical$|systems$|healthcare|koninklijke|icap-|snip-|corresponds|rev(ision)?\b|date|\d{1,2} [a-z]+ \d{4})')
        for i, l in enumerate(p1):
            m = re.search(r'(?i)dicom conformance statement\s*(?:application annex|annex)?\s*:?\s*(.*)$', l)
            if m:
                rest = re.sub(r'(?i)^for\b\s*', '', m.group(1).strip(" :-")).strip()
                if rest and not SKIP.match(rest): prod = rest; break
                for l2 in p1[i+1:i+4]:
                    l2 = re.sub(r'(?i)^(application annex|annex)\s*:?\s*', '', l2).strip()
                    if l2 and not SKIP.match(l2) and not re.search(r'(?i)conformance statement', l2):
                        prod = l2; j = p1.index(l2) if l2 in p1 else -1
                        if j >= 0 and j + 1 < len(p1) and re.search(r'(?i)model|series|^[A-Z][A-Za-z0-9\-]+ ', p1[j+1]) and not SKIP.match(p1[j+1]): prod += " " + p1[j+1]
                        break
                if prod: break
        psrc = "first page"
    else: psrc = "file name"
    prod = re.sub(r'\s+Page \d+ of \d+.*$', '', prod).strip()
    ver = ""
    for src in (prod, base.replace("_", " "), head[:4000]):
        m = re.search(r'\b(V[A-Z]\d{2}[A-Z]?|VE\d{2}[A-Z]|XA\d{2}[A-Z]?|syngo [A-Z]{2}\d{2}[A-Z]?|[Vv](?:ersion)?\s?\d+(?:\.\d+)+[A-Za-z]?|R\d+(?:\.\d+)*|\d+\.\d+(?:\.\d+)*|[A-Z]\.\d{2}|SW\s?\d+(?:\.\d+)*)\b', src)
        if m: ver = m.group(1); break
    date = ""
    m = re.search(r'(\b\d{1,2}[-. ](?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)[a-z]*[-. ,]+\d{4}\b|\b(?:January|February|March|April|May|June|July|August|September|October|November|December)\s+\d{1,2}?,?\s*\d{4}\b|\b\d{4}-\d{2}-\d{2}\b|\b\d{2}\.\d{2}\.\d{4}\b|©\s?\d{2}\.\d{4})', head[:6000])
    if m: date = re.sub(r'\s+', ' ', m.group(1)).strip()
    sp = lambda x: re.sub(r'\s+', ' ', x or '').strip()
    return dict(product=sp(prod), product_src=psrc, version=sp(ver), date=sp(date))
def keyword(name):
    if not name: return ""
    w = re.findall(r'[A-Za-z0-9]+', name)
    k = "".join(x if (x.isupper() and len(x) > 1) or x[0].isupper() else x[:1].upper() + x[1:] for x in w)
    return k if k and k[0].isalpha() else ("X" + k if k else "")
def norm(n): return re.sub(r'[^a-z0-9]', '', n.lower())
COLS = ["creator","group","element_xx","keyword_suggestion","description","vr","vm","retired","conflict_flag","vendor","product","software_version","document","page","notes"]
def tsv(path, header, rows):
    with open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f, delimiter="\t", lineterminator="\n", quoting=csv.QUOTE_MINIMAL, escapechar="\\")
        w.writerow(header)
        for r in rows: w.writerow([r.get(c, "") for c in header])
def build(rowsfile, vendor, idmap):
    res = json.load(open(os.path.join(W, rowsfile)))
    out = []; docstat = {}
    for path, d in res.items():
        idv = idmap[path]; m = meta(path, idv)
        seen = {}
        withvr = {(r["creator"], r["group"], r["element_xx"]) for r in d["rows"] if r["vr"]}
        n = 0
        for r in d["rows"]:
            k = (r["creator"], r["group"], r["element_xx"], r["vr"], r["vm"], norm(r["name"]))
            if not r["vr"] and (r["creator"], r["group"], r["element_xx"]) in withvr: continue
            if k in seen:
                o = seen[k]
                if str(r["page"]) not in o["_pages"]: o["_pages"].append(str(r["page"]))
                continue
            notes = [x for x in [r["notes"]] if x]
            if r["block"]: notes.insert(0, "printed with block %s, i.e. (%s,%s%s)" % (r["block"], r["group"], r["block"], r["element_xx"]))
            if not r["creator"]: notes.append("private creator not stated for this row in the document")
            if not r["vr"]: notes.append("VR not stated in this table")
            if r["name"] == "" : notes.append("no attribute name on the tag line")
            o = dict(creator=r["creator"], group=r["group"], element_xx=r["element_xx"], keyword_suggestion=keyword(r["name"]),
                     description=r["name"], vr=r["vr"], vm=r["vm"], retired=r["retired"], vendor=vendor, product=m["product"],
                     software_version=m["version"], document=path, page=r["page"], _pages=[], _notes=notes)
            seen[k] = o; out.append(o); n += 1
        docstat[path] = dict(meta=m, count=n, dropped=d["dropped"])
    for o in out:
        if o["_pages"]: o["_notes"].append("also on page(s) " + ",".join(o["_pages"][:15]) + ("…" if len(o["_pages"]) > 15 else ""))
        o["notes"] = "; ".join(o["_notes"])
    return out, docstat
def conflicts(rows):
    g = collections.defaultdict(list)
    for r in rows:
        if r["creator"]: g[(r["creator"], r["group"], r["element_xx"])].append(r)
    conf = []
    for k, rs in g.items():
        vrs = {r["vr"] for r in rs if r["vr"]}; vms = {r["vm"] for r in rs if r["vm"]}
        names = {norm(r["description"]) for r in rs if r["description"]}
        names2 = {n for n in names if not any(n != o and (o.startswith(n) or n.startswith(o)) for o in names)}
        flags = []
        if len(vrs) > 1: flags.append("VR")
        if len(vms) > 1: flags.append("VM")
        if len(names2) > 1: flags.append("NAME")
        if flags:
            f = "+".join(flags)
            for r in rs: r["conflict_flag"] = f
            for r in rs:
                conf.append(dict(creator=k[0], group=k[1], element_xx=k[2], conflict=f, vr=r["vr"], vm=r["vm"], description=r["description"],
                                 vendor=r["vendor"], product=r["product"], software_version=r["software_version"], document=r["document"], page=r["page"]))
    return conf
