# writes all vendor TSVs, inventory, unreadable, conflicts, creators, summary from *_final.json
import os, sys, json, glob, collections, re
sys.path.insert(0, os.path.dirname(__file__))
import write
W = os.path.expanduser("~/work"); OUT = write.OUT
os.makedirs(OUT, exist_ok=True)
allrows = []; docstat = {}; done = []
for f in sorted(glob.glob(os.path.join(W, "*_final.json"))):
    key = os.path.basename(f)[:-11]; d = json.load(open(f)); done.append(key)
    for r in d["rows"]: r["_key"] = key
    allrows += d["rows"]; docstat.update({p: dict(s, key=key) for p, s in d["docstat"].items()})
conf = write.conflicts(allrows)
for key in done:
    rs = sorted([r for r in allrows if r["_key"] == key], key=lambda r: (r["creator"].upper(), r["group"], r["element_xx"], r["document"], r["page"]))
    write.tsv(os.path.join(OUT, key + ".tsv"), write.COLS, rs)
write.tsv(os.path.join(OUT, "conflicts.tsv"), ["creator","group","element_xx","conflict","vr","vm","description","vendor","product","software_version","document","page"],
          sorted(conf, key=lambda r: (r["creator"].upper(), r["group"], r["element_xx"], r["document"])))
# creators
cr = collections.defaultdict(lambda: dict(n=0, docs=set(), groups=set(), vendor=set()))
for r in allrows:
    if r["creator"]:
        c = cr[r["creator"]]; c["n"] += 1; c["docs"].add(r["document"]); c["groups"].add(r["group"]); c["vendor"].add(r["vendor"])
write.tsv(os.path.join(OUT, "creators.tsv"), ["creator","vendor","groups","tag_rows","documents"],
          [dict(creator=k, vendor=",".join(sorted(v["vendor"])), groups=",".join(sorted(v["groups"])), tag_rows=v["n"], documents=len(v["docs"])) for k, v in sorted(cr.items(), key=lambda x: x[0].upper())])
# inventory + unreadable
recs = {}
for l in open(os.path.join(W, "extract_log.jsonl")):
    x = json.loads(l); recs[x["path"]] = x
inv = []; unr = []
VR = {"siemens":"Siemens","ge":"GE","philips":"Philips","canon_toshiba":"Canon/Toshiba","varian":"Varian","elekta":"Elekta"}
for p, x in sorted(recs.items()):
    if x.get("nonpdf"): continue
    pages = int(x.get("pages") or 0)
    tl = "yes" if (x.get("chars") or 0) >= 200 * max(pages, 1) * 0 + 50 * max(pages, 1) else "no"
    if (x.get("chars") or 0) == 0: tl = "no"
    ds = docstat.get(p)
    m = ds["meta"] if ds else {}
    vendor = VR.get(ds["key"], "") if ds else ("Varian" if re.match(r'^(Siemens/Radiation Therapy \(Varian\)|Varian)', p) else p.split("/")[0])
    inv.append(dict(path=p, size=x.get("size"), vendor=vendor, product=m.get("product",""), version=m.get("version",""), date=m.get("date",""),
                    pages=pages, text_layer=tl, private_tags_found=(ds["count"] if ds else ("" if not ds else 0)),
                    notes=("; ".join(filter(None, ["product from " + m["product_src"] if m.get("product") else "",
                           "permissions-encrypted (text extraction allowed)" if str(x.get("encrypted","")).startswith("yes") else "",
                           "inside zip" if "!" in p else "", "not yet processed" if not ds else ""])))))
    if tl == "no" or x.get("error") or x.get("text_rc"):
        why = "no text layer (scanned image or vector-only text); not OCR'd, nothing guessed" if tl == "no" else (x.get("error") or x.get("text_err") or "")
        unr.append(dict(path=p, size=x.get("size"), pages=pages, reason=why, encrypted=str(x.get("encrypted","")).replace("\x00",""), producer=str(x.get("producer","")).replace("\x00","")))
write.tsv(os.path.join(OUT, "inventory.tsv"), ["path","size","vendor","product","version","date","pages","text_layer","private_tags_found","notes"], inv)
write.tsv(os.path.join(OUT, "unreadable.tsv"), ["path","size","pages","reason","encrypted","producer"], unr)
json.dump(dict(done=done, rows=len(allrows), conf=len(conf), unreadable=len(unr), creators=len(cr),
               per=[(k, sum(1 for r in allrows if r["_key"]==k), len({r["creator"] for r in allrows if r["_key"]==k and r["creator"]}),
                     sum(1 for r in allrows if r["_key"]==k and not r["creator"]), sum(1 for p,s in docstat.items() if s["key"]==k), sum(1 for p,s in docstat.items() if s["key"]==k and s["count"]>0)) for k in done]),
          open(os.path.join(W, "stats.json"), "w"))
print(open(os.path.join(W, "stats.json")).read())
