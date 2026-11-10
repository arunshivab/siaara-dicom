#!/usr/bin/env python3
"""The toolkit version, from its single source (standard library only).

The version is written in ONE place: the project(siaara VERSION x.y.z ...) line of the top-level
CMakeLists.txt. Everything else is derived from it (docs/RELEASING.md lists what).

    python3 tools/release/version.py                      prints x.y.z
    python3 tools/release/version.py --implementation-version-name     prints SIAARA_xyz
    python3 tools/release/version.py --check-tag v1.2.3   exit 1 unless the tag is v<version>
    python3 tools/release/version.py --check              checks the derived places (ctest: version_single_source)
    python3 tools/release/version.py --release-notes x.y.z [--commit SHA] [--repository owner/name]
                                                          the CHANGELOG.md section of x.y.z as release notes
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def version():
    m = re.search(r"^project\(siaara VERSION ([0-9]+)\.([0-9]+)\.([0-9]+)\b", read("CMakeLists.txt"), re.M)
    if not m:
        sys.exit("version.py: no project(siaara VERSION x.y.z ...) line in CMakeLists.txt")
    return m.group(1), m.group(2), m.group(3)


def implementation_version_name(v):
    return "SIAARA_" + "".join(v)


def check():
    """The places that must NOT carry a version of their own, and the limits the version must keep."""
    v = version()
    dotted = ".".join(v)
    problems = []
    ivn = implementation_version_name(v)
    if len(ivn) > 16:
        problems.append("Implementation Version Name %s is longer than 16 characters (VR SH)" % ivn)
    if "SIAARA_VERSION_STRING" not in read("src/capi/capi.cpp") or re.search(
            r'siaara_version_string\(void\)[^{]*\{\s*return\s*"', read("src/capi/capi.cpp")):
        problems.append("src/capi/capi.cpp: siaara_version_string() must return SIAARA_VERSION_STRING")
    if re.search(r'kImplementationVersionName\s*=\s*"', read("src/core/writer.hpp")):
        problems.append("src/core/writer.hpp: kImplementationVersionName must be SIAARA_IMPLEMENTATION_VERSION_NAME")
    for rel in ("bindings/csharp/Directory.Build.props", "bindings/csharp/src/Siaara/Siaara.csproj",
                "bindings/csharp/src/Siaara.Interop/Siaara.Interop.csproj"):
        if re.search(r"<(Version|PackageVersion|AssemblyVersion|FileVersion|VersionPrefix)>\s*[0-9]", read(rel)):
            problems.append("%s: carries a literal version (it must come from CMakeLists.txt)" % rel)
    if re.search(r"find_package\(siaara\s+[0-9]", read("tests/package/CMakeLists.txt")):
        problems.append("tests/package/CMakeLists.txt: find_package(siaara) must not name a version")
    changelog = read("CHANGELOG.md") if os.path.exists(os.path.join(ROOT, "CHANGELOG.md")) else ""
    top = re.search(r"^## (.+)$", changelog, re.M)
    if not top or (top.group(1).strip() not in (dotted, "[%s]" % dotted, "First release")):
        problems.append("CHANGELOG.md: the topmost section must be '## %s' (or '## First release'), found %r"
                        % (dotted, top.group(1) if top else None))
    for p in problems:
        print("version.py: " + p, file=sys.stderr)
    if not problems:
        print("version %s, Implementation Version Name %s: one source, derived places consistent" % (dotted, ivn))
    return 1 if problems else 0


def release_notes(want, commit, repository):
    text = read("CHANGELOG.md")
    m = re.search(r"^## (?:\[?%s\]?|First release)[ \t]*\n(.*?)(?=^## |\Z)" % re.escape(want), text, re.M | re.S)
    if not m or m.start() != re.search(r"^## ", text, re.M).start():
        sys.exit("version.py: the topmost section of CHANGELOG.md is not '## %s' (or '## First release')" % want)
    out = ["Draft release of SIAARA DICOM Toolkit %s%s." % (want, (", built from commit %s" % commit) if commit else ""), "",
           "Assets: one archive per platform (`siaara-%s-<platform>.tar.gz` / `.zip`) with its SPDX 2.3 SBOM, the NuGet "
           "package `Siaara.%s.nupkg` (managed assemblies and the native library of every runtime), and `SHA256SUMS`." % (want, want),
           "The binaries and the package are not code signed. Licence: proprietary, see `LICENSE` in every archive and in the package.",
           ""]
    if repository:
        out += ["Verify: `sha256sum -c SHA256SUMS`; provenance: `gh attestation verify <file> --repo %s`." % repository, ""]
    out.append(m.group(1).strip())
    print("\n".join(out))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--implementation-version-name", action="store_true")
    ap.add_argument("--check-tag")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--release-notes")
    ap.add_argument("--commit", default="")
    ap.add_argument("--repository", default="")
    args = ap.parse_args()
    v = version()
    dotted = ".".join(v)
    if args.check:
        return check()
    if args.release_notes:
        return release_notes(args.release_notes, args.commit, args.repository)
    if args.check_tag:
        if args.check_tag != "v" + dotted:
            print("Tag %s does not name the version of CMakeLists.txt (%s): expected v%s" % (args.check_tag, dotted, dotted),
                  file=sys.stderr)
            return 1
        return 0
    print(implementation_version_name(v) if args.implementation_version_name else dotted)
    return 0


if __name__ == "__main__":
    sys.exit(main())
