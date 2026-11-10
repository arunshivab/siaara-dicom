import os, sys, json, re, subprocess
sys.path.insert(0, os.path.dirname(__file__))
import write
W = os.path.expanduser("~/work")
VEND = {
 "siemens": (r'^Siemens/(?!Radiation Therapy \(Varian\))', "Siemens"),
 "ge": (r'^GE/', "GE"),
 "philips": (r'^Philips/', "Philips"),
 "canon_toshiba": (r'^Canon/', "Canon/Toshiba"),
 "varian": (r'^(Siemens/Radiation Therapy \(Varian\)|Varian/|Varian Old/)', "Varian"),
 "elekta": (r'^Elekta/', "Elekta"),
}
key = sys.argv[1]; rx, vname = VEND[key]
r = subprocess.run(["python3", "-I", os.path.join(W, "scripts/resolve.py"), rx, key], capture_output=True, text=True)
print(r.stdout.strip(), r.stderr[-2000:])
idmap = {l.split("\t")[1]: l.split("\t")[0] for l in open(os.path.join(W, "idmap.tsv")).read().splitlines()}
rows, docstat = write.build(key + "_rows.json", vname, idmap)
json.dump({"rows": rows, "docstat": docstat}, open(os.path.join(W, key + "_final.json"), "w"))
print(key, "rows", len(rows), "docs", len(docstat))
