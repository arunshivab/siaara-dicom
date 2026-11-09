#!/usr/bin/env python3
"""Packages an installed SIAARA tree as a reproducible release archive (standard library only).

    SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) python3 tools/release/package.py \
        --stage stage/siaara-0.1.0-linux-x64 --version 0.1.0 --platform linux-x64 \
        --commit $(git rev-parse HEAD) --out dist

Adds to the staged tree: NOTICE (the license is to be decided; no LICENSE file is invented), the
README, and the SBOM (siaara.spdx.json, describing every other file). Then writes to --out:
  siaara-<version>-<platform>.tar.gz   (.zip for windows-*): sorted entries, owner 0/0, fixed
                                        permissions, every timestamp = SOURCE_DATE_EPOCH
  siaara-<version>-<platform>.spdx.json
  siaara-<version>-<platform>.sha256    SHA-256 of the archive and the SBOM (sha256sum format)
"""
import argparse
import gzip
import hashlib
import io
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

NOTICE = """SIAARA DICOM Toolkit {version}

License: to be decided. The project has not chosen a license yet; until a LICENSE file is
published in the source repository, no license is granted by this package. Contact the
copyright holder before redistributing.

Source: https://github.com/arunshivab/siaara-dicom (commit {commit})
SBOM: siaara.spdx.json (SPDX 2.3) lists the files of this package, the toolkit's components,
the data generated from published standards, and the operating-system libraries used at run time.
"""


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def entries(stage):
    """(relative path, absolute path) of every file and folder, sorted."""
    out = []
    for dirpath, dirnames, filenames in os.walk(stage):
        dirnames.sort()
        for d in dirnames:
            p = os.path.join(dirpath, d)
            out.append((os.path.relpath(p, stage).replace(os.sep, "/"), p))
        for n in sorted(filenames):
            p = os.path.join(dirpath, n)
            out.append((os.path.relpath(p, stage).replace(os.sep, "/"), p))
    return sorted(out)


def mode_of(path):
    st = os.lstat(path)
    if stat.S_ISDIR(st.st_mode):
        return 0o755
    if stat.S_ISLNK(st.st_mode):
        return 0o777
    return 0o755 if st.st_mode & 0o111 else 0o644


def write_tar(stage, top, out, epoch):
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for rel, path in [("", stage)] + entries(stage):
            info = tar.gettarinfo(path, arcname=top + ("/" + rel if rel else ""))
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mtime = epoch
            info.mode = mode_of(path)
            info.pax_headers = {}
            if info.isreg():
                with open(path, "rb") as f:
                    tar.addfile(info, f)
            else:
                tar.addfile(info)
    with open(out, "wb") as f:
        # gzip header: no file name, fixed mtime.
        with gzip.GzipFile(filename="", mode="wb", fileobj=f, mtime=epoch, compresslevel=9) as gz:
            gz.write(raw.getvalue())


def write_zip(stage, top, out, epoch):
    import time
    t = time.gmtime(max(epoch, 315532800))  # zip timestamps start in 1980
    stamp = (t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec - t.tm_sec % 2)
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel, path in entries(stage):
            name = top + "/" + rel
            if os.path.isdir(path):
                info = zipfile.ZipInfo(name + "/", stamp)
                info.external_attr = (0o40755 << 16) | 0x10
                z.writestr(info, b"")
                continue
            info = zipfile.ZipInfo(name, stamp)
            info.external_attr = (0o100000 | mode_of(path)) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3  # fixed, independent of the packaging host
            with open(path, "rb") as f:
                z.writestr(info, f.read())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stage", required=True, help="cmake --install prefix to package")
    ap.add_argument("--version", required=True)
    ap.add_argument("--platform", required=True)
    ap.add_argument("--commit", default="NOASSERTION")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    name = "siaara-%s-%s" % (args.version, args.platform)
    os.makedirs(args.out, exist_ok=True)

    license_files = [f for f in ("LICENSE", "LICENSE.txt", "LICENSE.md", "COPYING") if os.path.exists(os.path.join(ROOT, f))]
    for f in license_files:
        shutil.copy2(os.path.join(ROOT, f), os.path.join(args.stage, f))
    if not license_files:
        with open(os.path.join(args.stage, "NOTICE"), "w", encoding="utf-8", newline="\n") as f:
            f.write(NOTICE.format(version=args.version, commit=args.commit))
    for doc in ("README.md", "SECURITY.md"):
        if os.path.exists(os.path.join(ROOT, doc)):
            shutil.copyfile(os.path.join(ROOT, doc), os.path.join(args.stage, doc))

    sbom_tmp = os.path.join(args.out, name + ".spdx.json")
    stage_sbom = os.path.join(args.stage, "siaara.spdx.json")
    if os.path.exists(stage_sbom):
        os.remove(stage_sbom)
    subprocess.run([sys.executable, os.path.join(HERE, "make_sbom.py"), "--stage", args.stage, "--version", args.version,
                    "--platform", args.platform, "--commit", args.commit, "--out", sbom_tmp], check=True)
    shutil.copyfile(sbom_tmp, stage_sbom)

    if args.platform.startswith("windows"):
        archive = os.path.join(args.out, name + ".zip")
        write_zip(args.stage, name, archive, epoch)
    else:
        archive = os.path.join(args.out, name + ".tar.gz")
        write_tar(args.stage, name, archive, epoch)
    sums = os.path.join(args.out, name + ".sha256")
    with open(sums, "w", encoding="utf-8", newline="\n") as f:
        for p in (archive, sbom_tmp):
            f.write("%s  %s\n" % (sha256(p), os.path.basename(p)))
    print("packaged %s" % archive)
    with open(sums, encoding="utf-8") as f:
        sys.stdout.write(f.read())
    return 0


if __name__ == "__main__":
    sys.exit(main())
