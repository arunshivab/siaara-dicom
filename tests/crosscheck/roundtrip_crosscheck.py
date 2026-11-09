#!/usr/bin/env python3
"""Round-trip fidelity check (test-only tool, never shipped).

For every file under a folder: open it with SIAARA, save it again with the default options in the
same transfer syntax (no edits), and compare the bytes. Reports how many files round-trip byte for
byte, and why the others differ. Uses the C API through ctypes. Usage:
    python3 roundtrip_crosscheck.py <folder> <path to libsiaara.so / siaara.dll> [-v]

Files that cannot round-trip by design are counted separately:
  raw      no preamble / "DICM" (the writer always writes Part 10 with file meta)
  deflate  deflated transfer syntax (the data set is re-compressed)
  repaired the reader reported lenient-mode repairs (warnings)
"""
import ctypes
import os
import sys
import tempfile

if len(sys.argv) < 3:
    print(__doc__)
    sys.exit(2)
folder, libpath = sys.argv[1], sys.argv[2]
verbose = "-v" in sys.argv[3:]

lib = ctypes.CDLL(os.path.abspath(libpath))
lib.siaara_file_open.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
lib.siaara_file_open.restype = ctypes.c_int32
if hasattr(lib, "siaara_file_save_ex"):
    lib.siaara_file_save_ex.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p]
    lib.siaara_file_save_ex.restype = ctypes.c_int32
    save = lambda f, p: lib.siaara_file_save_ex(f, p, len(p), None, None)
else:  # older library: siaara_file_save (same defaults)
    lib.siaara_file_save.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p]
    save = lambda f, p: lib.siaara_file_save(f, p, len(p), None)
lib.siaara_file_free.argtypes = [ctypes.c_void_p]
lib.siaara_file_warning_count.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
lib.siaara_file_transfer_syntax.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
lib.siaara_last_error_message.restype = ctypes.c_char_p

paths = []
for root, _, names in os.walk(folder):
    for n in sorted(names):
        p = os.path.join(root, n)
        if os.path.isfile(p) and not os.path.islink(p):
            paths.append(p)
paths.sort()

out_path = os.path.join(tempfile.mkdtemp(prefix="siaara_rt_"), "out.dcm").encode()
counts = {"identical": 0, "differ": 0, "raw": 0, "deflate": 0, "repaired": 0, "unopened": 0, "save_failed": 0}
differ = []
for p in paths:
    with open(p, "rb") as fh:
        data = fh.read()
    f = ctypes.c_void_p()
    bp = p.encode()
    if lib.siaara_file_open(bp, len(bp), None, ctypes.byref(f)) != 0:
        counts["unopened"] += 1
        continue
    warn = ctypes.c_uint32()
    lib.siaara_file_warning_count(f, ctypes.byref(warn))
    tsbuf = ctypes.create_string_buffer(128)
    need = ctypes.c_size_t()
    lib.siaara_file_transfer_syntax(f, tsbuf, 128, ctypes.byref(need))
    ts = tsbuf.value.decode()
    rc = save(f, out_path)
    msg = lib.siaara_last_error_message().decode(errors="replace") if rc else ""
    lib.siaara_file_free(f)
    if rc != 0:
        counts["save_failed"] += 1
        differ.append((p, "save failed: " + msg))
        continue
    with open(out_path, "rb") as fh:
        back = fh.read()
    if back == data:
        counts["identical"] += 1
        continue
    if len(data) < 132 or data[128:132] != b"DICM":
        counts["raw"] += 1
        kind = "raw"
    elif ts == "1.2.840.10008.1.2.1.99":
        counts["deflate"] += 1
        kind = "deflate"
    elif warn.value:
        counts["repaired"] += 1
        kind = f"repaired ({warn.value} warnings)"
    else:
        counts["differ"] += 1
        kind = "differ"
    first = next((i for i in range(min(len(data), len(back))) if data[i] != back[i]), min(len(data), len(back)))
    differ.append((p, f"{kind}: first difference at byte {first}, size {len(data)} -> {len(back)}"))

opened = len(paths) - counts["unopened"]
print(f"{len(paths)} files, {opened} opened, {counts['identical']} round-trip byte for byte")
print("not identical: " + ", ".join(f"{k} {v}" for k, v in counts.items() if k not in ("identical", "unopened")))
print(f"could not open: {counts['unopened']}")
if verbose or counts["differ"] or counts["save_failed"]:
    for p, why in differ:
        if verbose or why.startswith(("differ", "save failed")):
            print(f"  {os.path.relpath(p, folder)}: {why}")
try:
    os.remove(out_path)
    os.rmdir(os.path.dirname(out_path))
except OSError:
    pass
sys.exit(1 if counts["differ"] or counts["save_failed"] else 0)
