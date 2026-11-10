import os, sys, subprocess, hashlib, json, zipfile, shutil, time
from concurrent.futures import ThreadPoolExecutor, wait, FIRST_COMPLETED
ROOT = os.path.expanduser("~/mnt/DICOM Conformance Statement")
W = os.path.expanduser("~/work")
BUDGET = float(sys.argv[1]) if len(sys.argv) > 1 else 160
T0 = time.time()
LOG = os.path.join(W, "extract_log.jsonl")
done = set()
if os.path.exists(LOG):
    for l in open(LOG):
        try: done.add(json.loads(l)["path"])
        except Exception: pass
def info(p):
    r = subprocess.run(["pdfinfo", p], capture_output=True, text=True, errors="replace", timeout=60)
    d = {}
    for line in r.stdout.splitlines():
        if ":" in line:
            k, v = line.split(":", 1); d[k.strip()] = v.strip()
    return d, r.returncode, r.stderr[:300]
def handle(p, rel, size):
    h = hashlib.sha1(rel.encode()).hexdigest()[:16]
    rec = {"path": rel, "size": size, "id": h}
    try:
        d, rc, err = info(p)
        rec.update({"pages": d.get("Pages"), "encrypted": d.get("Encrypted"), "title": d.get("Title"),
                    "created": d.get("CreationDate"), "producer": d.get("Producer"), "info_rc": rc, "info_err": err})
        txt = os.path.join(W, "text", h + ".txt")
        r = subprocess.run(["pdftotext", "-layout", p, txt], capture_output=True, text=True, errors="replace", timeout=150)
        rec["text_rc"] = r.returncode; rec["text_err"] = r.stderr[:300]
        if os.path.exists(txt):
            t = open(txt, encoding="utf-8", errors="replace").read()
            rec["chars"] = len(t.replace(" ", "").replace("\n", "").replace("\f", ""))
    except subprocess.TimeoutExpired:
        rec["error"] = "timeout"
    except Exception as e:
        rec["error"] = repr(e)[:300]
    return rec
ORDER = ["Siemens","GE","Philips","Canon","Varian","Varian Old","Elekta"]
tops = [x for x in ORDER if os.path.isdir(os.path.join(ROOT,x))] + sorted(x for x in os.listdir(ROOT) if os.path.isdir(os.path.join(ROOT,x)) and x not in ORDER and not x.startswith("_siaara"))
jobs = []
for f in sorted(os.listdir(ROOT)):
    if os.path.isfile(os.path.join(ROOT,f)): jobs.append(os.path.join(ROOT,f))
for t in tops:
    for dp, dn, fn in os.walk(os.path.join(ROOT,t)):
        dn.sort()
        for f in sorted(fn): jobs.append(os.path.join(dp,f))
tasks = []
out = open(LOG, "a")
for p in jobs:
    rel = os.path.relpath(p, ROOT); f = os.path.basename(p)
    if f.lower().endswith(".pdf"):
        if rel not in done: tasks.append((p, rel, os.path.getsize(p)))
    elif f.lower().endswith(".zip"):
        zd = os.path.join(W, "zips", hashlib.sha1(rel.encode()).hexdigest()[:12])
        if not os.path.isdir(zd):
            os.makedirs(zd)
            with zipfile.ZipFile(p) as z:
                for m in z.infolist():
                    if m.filename.lower().endswith(".pdf") and ".." not in m.filename:
                        with z.open(m) as s, open(os.path.join(zd, os.path.basename(m.filename)), "wb") as d: shutil.copyfileobj(s, d)
        for f2 in sorted(os.listdir(zd)):
            r2 = rel + "!" + f2
            if r2 not in done: tasks.append((os.path.join(zd, f2), r2, os.path.getsize(os.path.join(zd, f2))))
    elif rel not in done:
        out.write(json.dumps({"path": rel, "size": os.path.getsize(p), "nonpdf": True}) + "\n")
out.flush()
n = 0
with ThreadPoolExecutor(2) as ex:
    pending = set(); it = iter(tasks)
    while True:
        while len(pending) < 2 and time.time() - T0 < BUDGET:
            try: pending.add(ex.submit(handle, *next(it)))
            except StopIteration: break
        if not pending: break
        dn, pending = wait(pending, return_when=FIRST_COMPLETED)
        for fu in dn:
            out.write(json.dumps(fu.result()) + "\n"); out.flush(); n += 1
remaining = len(tasks) - n
print("did", n, "remaining", remaining)
if remaining == 0: open(os.path.join(W, "EXTRACT_DONE"), "w").write("ok")
