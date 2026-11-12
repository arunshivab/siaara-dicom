#!/usr/bin/env python3
"""Unit tests of the content check of tools/release/check_nupkg.py (standard library only).

    python3 -I tools/release/test_check_nupkg.py        (ctest: release_check_nupkg)

Builds small synthetic .nupkg files (zip archives with a minimal nuspec) in a temporary folder:
the expected layout passes; a file that is not on the allow-list (R-218), a missing file, a
wrong nuspec or a duplicate entry fails, with the offending name in the message.
"""
import contextlib
import io
import os
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_nupkg  # noqa: E402

VERSION = "0.1.0-rc.1"
NUSPEC = ('<?xml version="1.0" encoding="utf-8"?>\n<package><metadata><id>Siaara</id><version>%s</version>'
          '<authors>Sthraas Intelligence Pvt Ltd</authors><license type="file">LICENSE</license>'
          '<readme>README.md</readme></metadata></package>\n')


def good_entries(version=VERSION):
    e = {n: b"x" for n in check_nupkg.MANAGED + ("LICENSE", "README.md")}
    e["Siaara.nuspec"] = (NUSPEC % version).encode()
    e["[Content_Types].xml"] = b"<Types/>"
    e["_rels/.rels"] = b"<Relationships/>"
    e["package/services/metadata/core-properties/0123456789abcdef0123456789abcdef.psmdcp"] = b"<x/>"
    for rid, lib in check_nupkg.RIDS.items():
        e["runtimes/%s/native/%s" % (rid, lib)] = b"lib"
    return e


class CheckContent(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def run_check(self, entries, require_all=True, extra=()):
        path = os.path.join(self.tmp.name, "Siaara.%s.nupkg" % VERSION)
        with zipfile.ZipFile(path, "w") as z:
            for n, data in entries.items():
                z.writestr(n, data)
            for n, data in extra:  # duplicate names
                z.writestr(n, data)
        with contextlib.redirect_stdout(io.StringIO()):
            import warnings
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")  # zipfile warns on a duplicate name
                return check_nupkg.check_content(path, VERSION, require_all, False)

    def assertProblem(self, problems, text):
        self.assertTrue(any(text in p for p in problems), "%r not in %r" % (text, problems))

    def test_expected_layout_passes(self):
        self.assertEqual(self.run_check(good_entries()), [])

    def test_icon_and_folder_entries_are_allowed(self):
        e = good_entries()
        e["icon.png"] = b"png"
        e["lib/"] = b""
        e["runtimes/win-x64/native/"] = b""
        self.assertEqual(self.run_check(e), [])

    def test_unexpected_files_fail_and_are_named(self):
        for name in ("lib/net10.0/Other.dll", "lib/net10.0/Siaara.Interop.xml", "lib/net10.0/Siaara.pdb",
                     "tools/install.ps1", "build/Siaara.targets", "content/readme.txt", "contentFiles/any/x.cs",
                     "runtimes/win-x64/native/extra.dll", "runtimes/linux-x86/native/libsiaara.so",
                     "runtimes/win-x64/lib/net10.0/Siaara.dll", "../evil", "/abs", "license", "Readme.md",
                     "package/services/metadata/core-properties/zz.psmdcp", "_rels/other.rels", "other/"):
            with self.subTest(name=name):
                e = good_entries()
                e[name] = b"x"
                self.assertProblem(self.run_check(e), "allow-list: " + name)

    def test_missing_runtime_fails_only_with_require_all(self):
        e = good_entries()
        del e["runtimes/osx-arm64/native/libsiaara.dylib"]
        self.assertProblem(self.run_check(e), "missing native library: runtimes/osx-arm64/native/libsiaara.dylib")
        self.assertEqual(self.run_check(e, require_all=False), [])

    def test_missing_managed_file_fails(self):
        e = good_entries()
        del e["lib/net10.0/Siaara.xml"]
        self.assertProblem(self.run_check(e), "missing: lib/net10.0/Siaara.xml")

    def test_wrong_version_fails(self):
        self.assertProblem(self.run_check(good_entries("0.1.0")), "nuspec version is '0.1.0'")

    def test_dependency_fails(self):
        e = good_entries()
        e["Siaara.nuspec"] = e["Siaara.nuspec"].replace(b"</metadata>", b'<dependencies><dependency id="X" /></dependencies></metadata>')
        self.assertProblem(self.run_check(e), "declares a package dependency")

    def test_duplicate_entry_fails(self):
        self.assertProblem(self.run_check(good_entries(), extra=[("LICENSE", b"other")]), "duplicate entries")

    def test_two_psmdcp_fail(self):
        e = good_entries()
        e["package/services/metadata/core-properties/ffffffffffffffffffffffffffffffff.psmdcp"] = b"<x/>"
        self.assertProblem(self.run_check(e), "more than one")


if __name__ == "__main__":
    unittest.main(verbosity=1)
