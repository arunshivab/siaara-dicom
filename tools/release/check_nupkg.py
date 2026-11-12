#!/usr/bin/env python3
"""Checks the Siaara NuGet package, and tests it as a consumer would (standard library only).

    python3 tools/release/check_nupkg.py Siaara.0.1.0.nupkg --version 0.1.0 [--require-all-rids] [--list]
    python3 tools/release/check_nupkg.py Siaara.0.1.0.nupkg --version 0.1.0 --consumer-test <empty work folder>

Content check: the managed assemblies and their XML documentation in lib/net10.0, LICENSE and
README.md, the nuspec (id, version, authors, licence file, NO dependency), and the native library
under runtimes/<rid>/native for the runtimes present (--require-all-rids: all seven).
Allow-list (R-218): the package may hold ONLY those files, an icon.png, and the OPC metadata that
NuGet writes ([Content_Types].xml, _rels/.rels, package/services/metadata/core-properties/
<32 hex digits>.psmdcp). Any other entry fails the check and is named.
Consumer test: copies examples/csharp/QuickStart into the work folder with a nuget.config whose
only source is a local folder holding the package, then restores, builds and runs it with a fresh
package cache. The caller makes the network unusable (dead proxy) to prove nothing is fetched; the
test also fails when anything but the Siaara package lands in the package cache.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MANAGED = ("lib/net10.0/Siaara.dll", "lib/net10.0/Siaara.Interop.dll", "lib/net10.0/Siaara.xml")
REQUIRED = MANAGED + ("LICENSE", "README.md", "Siaara.nuspec")
OPTIONAL = ("icon.png", "[Content_Types].xml", "_rels/.rels")
PSMDCP = re.compile(r"package/services/metadata/core-properties/[0-9a-f]{32}\.psmdcp")
RIDS = {"linux-x64": "libsiaara.so", "linux-arm64": "libsiaara.so", "win-x64": "siaara.dll", "win-x86": "siaara.dll",
        "win-arm64": "siaara.dll", "osx-x64": "libsiaara.dylib", "osx-arm64": "libsiaara.dylib"}


def allowed_names():
    """Every file name the package may hold, apart from the .psmdcp metadata file (PSMDCP)."""
    return set(REQUIRED) | set(OPTIONAL) | {"runtimes/%s/native/%s" % (r, l) for r, l in RIDS.items()}


def unexpected_entries(names):
    """The entries of the package that are not on the allow-list, sorted. A directory entry (name
    ending in "/") is allowed only when it is a folder of an allowed file."""
    allowed = allowed_names()
    folders = set()
    for n in allowed | {"package/services/metadata/core-properties/x"}:
        parts = n.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            folders.add("/".join(parts[:i]) + "/")
    bad = []
    for n in names:
        if n in allowed or PSMDCP.fullmatch(n) or n in folders:
            continue
        bad.append(n)
    return sorted(bad)


def check_content(path, version, require_all, listing):
    problems = []
    with zipfile.ZipFile(path) as z:
        entries = z.infolist()
        names = {i.filename: i.file_size for i in entries}
        nuspec = z.read("Siaara.nuspec").decode("utf-8") if "Siaara.nuspec" in names else ""
    if listing:
        for n in sorted(names):
            print("%10d  %s" % (names[n], n))
    for n in REQUIRED:
        if n not in names:
            problems.append("missing: " + n)
    present = []
    for rid, lib in sorted(RIDS.items()):
        n = "runtimes/%s/native/%s" % (rid, lib)
        if n in names:
            present.append(rid)
        elif require_all:
            problems.append("missing native library: " + n)
    if not present:
        problems.append("no native library under runtimes/")
    if len(entries) != len(names):
        problems.append("the package has duplicate entries")
    for n in unexpected_entries(names):
        problems.append("file not on the package allow-list: " + n)
    if sum(1 for n in names if PSMDCP.fullmatch(n)) > 1:
        problems.append("more than one package/services/metadata/core-properties/*.psmdcp")

    def tag(t):
        m = re.search(r"<%s[^>]*>([^<]*)</%s>" % (t, t), nuspec)
        return m.group(1) if m else None
    if tag("id") != "Siaara":
        problems.append("nuspec id is %r" % tag("id"))
    if tag("version") != version:
        problems.append("nuspec version is %r, expected %s" % (tag("version"), version))
    if tag("authors") != "Sthraas Intelligence Pvt Ltd":
        problems.append("nuspec authors is %r" % tag("authors"))
    if not re.search(r'<license type="file">LICENSE</license>', nuspec):
        problems.append("nuspec does not name LICENSE as the licence file")
    if tag("readme") != "README.md":
        problems.append("nuspec readme is %r" % tag("readme"))
    if re.search(r"<dependency\s", nuspec):
        problems.append("nuspec declares a package dependency")
    print("%s: %d entries; runtimes: %s" % (path, len(names), ", ".join(present) or "none"))
    return problems


def consumer_test(path, version, work):
    if os.path.exists(work):
        shutil.rmtree(work)
    feed = os.path.join(work, "packages")
    cache = os.path.join(work, "nuget-cache")
    app = os.path.join(work, "QuickStart")
    os.makedirs(feed)
    shutil.copy2(path, feed)
    shutil.copytree(os.path.join(ROOT, "examples", "csharp", "QuickStart"), app,
                    ignore=shutil.ignore_patterns("bin", "obj"))
    with open(os.path.join(work, "nuget.config"), "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n<configuration>\n  <packageSources>\n    <clear />\n'
                '    <add key="siaara-local" value="%s" />\n  </packageSources>\n</configuration>\n' % feed)
    env = dict(os.environ, NUGET_PACKAGES=cache, NUGET_HTTP_CACHE_PATH=os.path.join(work, "nuget-http"),
               DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_NOLOGO="1")
    props = ["-p:SiaaraPackageVersion=" + version]

    def run(cmd):
        print("+ " + " ".join(cmd), flush=True)
        subprocess.run(cmd, cwd=app, env=env, check=True)
    run(["dotnet", "restore", "--configfile", os.path.join(work, "nuget.config")] + props)
    run(["dotnet", "build", "-c", "Release", "--no-restore"] + props)
    run(["dotnet", "run", "-c", "Release", "--no-build"] + props + ["--", os.path.join(work, "run")])
    fetched = sorted(os.listdir(cache))
    print("package cache after the test: " + ", ".join(fetched))
    if fetched != ["siaara"]:
        return ["the consumer restore put more than the Siaara package into the package cache: %s" % fetched]
    return []


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("nupkg")
    ap.add_argument("--version", required=True)
    ap.add_argument("--require-all-rids", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--consumer-test", metavar="WORK")
    args = ap.parse_args()
    nupkg = os.path.abspath(args.nupkg)
    problems = check_content(nupkg, args.version, args.require_all_rids, args.list)
    if not problems and args.consumer_test:
        problems = consumer_test(nupkg, args.version, os.path.abspath(args.consumer_test))
    for p in problems:
        print("check_nupkg.py: " + p, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
