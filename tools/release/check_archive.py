#!/usr/bin/env python3
"""Checks the content of a release archive (standard library only).

    python3 tools/release/check_archive.py dist/siaara-0.1.0-linux-x64.tar.gz --platform linux-x64 [--list]

Fails when something a consumer needs is missing (header, shared and static library, CMake package
files, pkg-config file on Unix, the command-line tools, LICENSE, the consumer documents, the
examples, the SBOM) or when something that must not ship is inside (tests, fuzz corpora, internal
documents, build trees, anything under data/ unless --allow-private-tag-sources). Other files
given on the command line (.spdx.json, .sha256) are ignored, so a shell glob may be passed.
--list prints every entry with its size.
"""
import argparse
import fnmatch
import sys
import tarfile
import zipfile

TOOLS = ["siaara-dump", "siaara-echoscu", "siaara-storescp", "siaara-webcli", "siaara-deid", "siaara-findscu",
         "siaara-movescu", "siaara-getscu", "siaara-storescu", "siaara-repair"]
DOCS = ["README.md", "GETTING_STARTED.md", "CONFORMANCE.md", "NETWORKING.md", "DATA_ACCESS.md", "VALIDATION.md",
        "PRIVATE_DICTIONARY.md", "capi-notes.md"]
FORBIDDEN = ["tests/*", "fuzz/*", "app/*", "bindings/*", "src/*", "tools/*", "build*/*", ".github/*", "*.o", "*.obj",
             "*.pdb", "*.ilk", "docs/FUZZING.md", "docs/RELEASING.md", "docs/RELEASE_CHECKLIST.md",
             "docs/ARCHITECTURE.md", "docs/BUILDING.md", "*/bin/Debug/*", "*/obj/*", "*.nupkg"]


def entries(path):
    if path.endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            return [(i.filename, i.file_size) for i in z.infolist() if not i.filename.endswith("/")]
    with tarfile.open(path) as t:
        return [(m.name, m.size) for m in t.getmembers() if m.isreg()]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--platform", required=True)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--allow-private-tag-sources", action="store_true")
    args = ap.parse_args()
    archives = [f for f in args.files if f.endswith((".tar.gz", ".zip"))]
    if len(archives) != 1:
        sys.exit("check_archive.py: expected exactly one archive, got %r" % archives)
    items = entries(archives[0])
    top = items[0][0].split("/")[0]
    rel = {}
    for name, size in items:
        if not name.startswith(top + "/"):
            sys.exit("check_archive.py: entry outside the top folder: " + name)
        rel[name[len(top) + 1:]] = size
    windows, macos = args.platform.startswith("windows"), args.platform.startswith("macos")
    exe = ".exe" if windows else ""
    need = ["include/siaara/siaara.h", "LICENSE", "README.md", "SECURITY.md", "CHANGELOG.md", "siaara.spdx.json",
            "lib/cmake/siaara/siaaraConfig.cmake", "lib/cmake/siaara/siaaraConfigVersion.cmake",
            "lib/cmake/siaara/siaaraTargets.cmake", "examples/CMakeLists.txt", "examples/c/read_file.c",
            "examples/c/network.c", "examples/cpp/read_file.cpp", "examples/csharp/QuickStart/Program.cs"]
    need += ["docs/" + d for d in DOCS]
    need += ["bin/%s%s" % (t, exe) for t in TOOLS]
    if windows:
        need += ["bin/siaara.dll", "lib/siaara.lib", "lib/siaara_static.lib"]
    elif macos:
        need += ["lib/libsiaara.dylib", "lib/libsiaara_static.a", "lib/pkgconfig/siaara.pc"]
    else:
        need += ["lib/libsiaara.so", "lib/libsiaara_static.a", "lib/pkgconfig/siaara.pc"]
    problems = ["missing: " + n for n in need if n not in rel]
    for name in sorted(rel):
        if any(fnmatch.fnmatch(name, pat) for pat in FORBIDDEN):
            problems.append("must not ship: " + name)
        if name.startswith("data/") and not args.allow_private_tag_sources:
            problems.append("data/ content in the package (owner decision; --allow-private-tag-sources): " + name)
    if args.list:
        for name in sorted(rel):
            print("%10d  %s" % (rel[name], name))
    data = sorted(n for n in rel if n.startswith("data/"))
    print("%s: %d files, %d bytes unpacked; from data/: %s" % (archives[0], len(rel), sum(rel.values()),
                                                               ("%d files" % len(data)) if data else "nothing"))
    for p in problems:
        print("check_archive.py: " + p, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
