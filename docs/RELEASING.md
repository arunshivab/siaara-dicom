# Releasing SIAARA

Releases are built by `.github/workflows/release.yml` when a tag `v<version>` is pushed. The
workflow produces a **draft** GitHub release; a maintainer reviews it and publishes it by hand.

## Steps

1. Make sure `main` is green (every CI job, including fuzz-smoke, TSan, dependency allow-list and
   exported symbols) and the nightly fuzz run has no open findings.
2. Set the version in both places (the workflow refuses a tag that does not match them):
   - `project(siaara VERSION x.y.z ...)` in `CMakeLists.txt`
   - `siaara_version_string()` in `src/capi/capi.cpp`
   If the C API changed, bump `SIAARA_ABI_VERSION` as needed and update
   `tools/ci/abi/exported_symbols.txt` with `python3 tools/ci/check_binaries.py symbols --update build/libsiaara.so`.
3. Commit, then tag and push the tag:

       git tag -a vX.Y.Z -m "SIAARA DICOM Toolkit X.Y.Z"
       git push origin vX.Y.Z

4. Wait for the workflow, then review the draft release: the archives, `SHA256SUMS`, the SBOMs and
   the release notes. Publish it.

## What the workflow does, per platform

Platforms: linux-x64, linux-x86, linux-arm64, windows-x64, windows-x86, windows-arm64,
macos-universal (`CMAKE_OSX_ARCHITECTURES="x86_64;arm64"`, deployment target 12.0).

1. Checks that the tag matches the CMake project version and `siaara_version_string()`.
2. Sets `SOURCE_DATE_EPOCH` to the commit time.
3. Builds Release and runs the full test suite (`ctest`). On macOS it also runs the tests under
   Rosetta for the x86_64 slice when the runner has Rosetta (otherwise a warning says that only
   arm64 was tested) and checks that the library contains both slices.
4. Installs into `stage/siaara-<version>-<platform>/` (`bin/`, `lib/` with the shared and static
   libraries and `lib/cmake/siaara/`, `include/siaara/siaara.h`).
5. Builds `tests/package/` against the installed tree with `find_package(siaara)` and runs the
   shared and static consumers.
6. Checks the runtime dependencies of every installed binary against `tools/ci/deps_allowlist.json`
   and the exported symbols against the ABI baseline.
7. Packages with `tools/release/package.py`: adds `NOTICE` (the license is to be decided - no
   license file is invented; when a `LICENSE` file exists in the repository it is shipped instead),
   `README.md`, `SECURITY.md` and the SBOM `siaara.spdx.json`; writes
   `siaara-<version>-<platform>.tar.gz` (`.zip` on Windows), the SPDX 2.3 SBOM next to it, and a
   `.sha256` file.
8. Attests build provenance for the archive and SBOM with `actions/attest-build-provenance`.

The `draft-release` job then merges the checksum files into `SHA256SUMS`, verifies them, and runs
`gh release create --draft --verify-tag` with every file attached.

## Verifying a release

    sha256sum -c SHA256SUMS
    gh attestation verify siaara-X.Y.Z-linux-x64.tar.gz --repo arunshivab/siaara-dicom

## Reproducibility

- Compile flags map build paths out of the binaries (`-ffile-prefix-map`, MSVC `/pathmap`) and
  MSVC uses `/Brepro`; static archives are written in GNU ar deterministic mode; no source uses
  `__DATE__` / `__TIME__`; the shared library gets a SHA-1 build ID derived from its contents.
- Archives have sorted entries, owner 0/0, normalised permissions, and every timestamp set to
  `SOURCE_DATE_EPOCH`; the gzip header carries no name and the same timestamp.
- The SBOM's creation time is `SOURCE_DATE_EPOCH` and its namespace is derived from version,
  platform and commit.

Two builds of the same commit with the same compiler and runner image should produce identical
archives. This has been checked locally on Linux (see the commit that added these scripts); it
has not yet been verified on Windows or macOS.

## SBOM contents

`tools/release/make_sbom.py` (standard library only) lists: the package and every file in it
(SHA-1 and SHA-256); the toolkit's source components (`core`, `dict`, `codec`, `image`, `charset`,
`net`, `web`, `store`, `deid`, `clinical`, `capi`); the data generated from published standards and
mapping tables (PS3.6/PS3.7 dictionary, PS3.15 Table E.1-1, PS3.16 codes, PS3.6 Annex B palettes,
Unicode/WHATWG mappings); the OS libraries recorded in the binaries (`readelf -d` / `otool -L` /
`dumpbin /dependents`); and the libraries loaded on demand (system OpenSSL 3 on Linux, `ncrypt.dll`
on Windows) as optional dependencies. Licenses are `NOASSERTION` until the project chooses one.
