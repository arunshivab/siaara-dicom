# Releasing SIAARA

Releases are built by `.github/workflows/release.yml` when a tag `v<version>` is pushed. The
workflow produces a **draft** GitHub release; a maintainer reviews it and publishes it by hand.
The gates and the exact order of steps are in [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md); this
document explains how the pieces work.

## Version: one source

The version is written in exactly one place:

    project(siaara VERSION x.y.z LANGUAGES C CXX)        # top-level CMakeLists.txt, line 5

**Bump procedure: edit that line. Nothing else.** Then build, run the tests, commit, tag.
(`ctest -R version_single_source`, i.e. `python3 tools/release/version.py --check`, fails if a
second literal version has crept in anywhere it knows of.)

Everything that changes with that one edit:

| What | Where it comes from | Value for 0.1.0 |
|---|---|---|
| `siaara_version_string()` (C API; C# `SiaaraLibrary.Version`) | `SIAARA_VERSION_STRING` in the generated `<build>/generated/siaara_version.hpp` (`cmake/siaara_version.hpp.in`) | `0.1.0` |
| **Implementation Version Name** (0002,0013) written into every file the toolkit saves, and sent in every A-ASSOCIATE request and response | `SIAARA_IMPLEMENTATION_VERSION_NAME` in the same header: `SIAARA_` + major, minor, patch digits. VR SH: **at most 16 characters**; CMake stops the configure step when it would be longer | `SIAARA_010` |
| CMake package version (`siaaraConfigVersion.cmake`) and its compatibility rule | `PROJECT_VERSION`; 0.x: same minor only, from 1.0.0: same major | `0.1.0`, SameMinorVersion |
| `Version:` of `lib/pkgconfig/siaara.pc` | `PROJECT_VERSION` (`cmake/siaara.pc.in`) | `0.1.0` |
| C# assembly version, file version, informational version and the **NuGet package version** of `Siaara` | `bindings/csharp/Directory.Build.props` reads the `project(...)` line of `CMakeLists.txt` | `0.1.0` |
| The tag the release workflow accepts | `tools/release/version.py --check-tag` | `v0.1.0` |
| Names of the archives, SBOMs and the top folder inside each archive; `versionInfo` and document name in the SBOM | the workflow passes `tools/release/version.py` output to `package.py` / `make_sbom.py` | `siaara-0.1.0-<platform>` |
| Release title and the first line of the release notes | the tag; `tools/release/version.py --release-notes` | |
| "Toolkit version" and "Implementation Version Name" rows of `docs/CONFORMANCE.md` | `tools/release/gen_conformance.py` (see below) | |

Two things do not change by themselves and are checked:

- **`docs/CONFORMANCE.md`** quotes the version and the Implementation Version Name in a
  generated table. After a bump, `ctest -R conformance_statement_up_to_date` fails until you run
  `python3 tools/release/gen_conformance.py --dump build/tools/siaara_conformance_dump`.
  This is intended: a conformance statement is a dated document and should be regenerated
  knowingly.
- **`CHANGELOG.md`**: the topmost section must be the one for the release (`## x.y.z`; the first
  release's section is headed `## First release` so that it needs no edit when the number is
  chosen).

Not tied to the version: **`SIAARA_ABI_VERSION`** in `include/siaara/siaara.h` (1; it changes
only when the C ABI changes incompatibly, together with `tools/ci/abi/exported_symbols.txt`:
`python3 tools/ci/check_binaries.py symbols --update build/libsiaara.so`), and the
Implementation Class UID (never changes).

Note on the Implementation Version Name: the digits are concatenated without separators, so
1.0.0 gives `SIAARA_100` and 1.10.2 gives `SIAARA_1102`. It identifies the release in files and
associations; it is not parsed by anything. Files written by an earlier version keep the name
they were written with.

## What the workflow does, per platform

Platforms: linux-x64, linux-x86, linux-arm64, windows-x64, windows-x86, windows-arm64,
macos-universal (`CMAKE_OSX_ARCHITECTURES="x86_64;arm64"`, deployment target 12.0).

1. Checks that the tag is `v` + the version of `CMakeLists.txt`.
2. Sets `SOURCE_DATE_EPOCH` to the commit time.
3. Builds Release (static C/C++ runtime, hardening on) and runs the full test suite (`ctest`),
   which includes the examples, the conformance statement check and the version check. On macOS
   it also runs the tests under Rosetta for the x86_64 slice when the runner has Rosetta
   (otherwise a warning says that only arm64 was tested) and checks that the library contains
   both slices.
4. Installs into `stage/siaara-<version>-<platform>/` (`bin/`, `lib/` with the shared and static
   libraries, `lib/cmake/siaara/`, `lib/pkgconfig/siaara.pc`, `include/siaara/siaara.h`).
5. Builds `tests/package/` against the installed tree with `find_package(siaara)` and runs the
   shared and static consumers; they check the ABI version and that the library reports the
   tagged version, and write and read back a file. On Linux the pkg-config file is tested the
   same way.
6. Checks the runtime dependencies of every installed binary against
   `tools/ci/deps_allowlist.json`, their hardening (same arguments as the `dependency-allowlist`
   job of `ci.yml`; on linux-arm64 the BTI/PAC property note is not checked, see the known
   limitations in `CHANGELOG.md`), and the exported symbols against the ABI baseline.
7. Packages with `tools/release/package.py`: adds `LICENSE`, `README.md`, `SECURITY.md`,
   `CHANGELOG.md`, the consumer documents in `docs/`, the example sources in `examples/` and the
   SBOM `siaara.spdx.json`; writes `siaara-<version>-<platform>.tar.gz` (`.zip` on Windows), the
   SPDX 2.3 SBOM next to it, and a `.sha256` file. `tools/release/check_archive.py` then fails
   the job when something a consumer needs is missing from the archive or something that must
   not ship is in it.
8. Attests build provenance for the archive and SBOM with `actions/attest-build-provenance`.
9. Uploads the installed shared library as `native-<platform>` for the NuGet job.

### What is in an archive, and what is not

In: the header, shared and static library, CMake and pkg-config files, the ten tools, `LICENSE`,
`README.md`, `SECURITY.md`, `CHANGELOG.md`, `docs/` (README, GETTING_STARTED, CONFORMANCE,
NETWORKING, DATA_ACCESS, VALIDATION, PRIVATE_DICTIONARY, capi-notes), `examples/`, the SBOM.

Not in: tests, fuzz targets and corpora, the viewer application, the bindings' sources, internal
documents (BUILDING, ARCHITECTURE, FUZZING, RELEASING, RELEASE_CHECKLIST).

**`data/`**: nothing from `data/` is packaged. `data/private-tags/` (about 51 MB: the rows
extracted from vendor DICOM Conformance Statements, with document names and page numbers, from
which `src/dict/private_dictionary_data.inc` is generated) stays in the repository. The
*dictionary* generated from it is compiled into the library and described by
`docs/PRIVATE_DICTIONARY.md`, which is packaged. One switch changes this:
`package.py --include-private-tag-sources` (and `check_archive.py --allow-private-tag-sources`).
Whether that data may be distributed is an owner decision (RELEASE_CHECKLIST.md).

## The NuGet package

.NET consumers get **one** package, `Siaara.<version>.nupkg`, for all platforms:

    lib/net10.0/Siaara.dll, Siaara.Interop.dll, Siaara.xml        managed, built once
    runtimes/win-x64/native/siaara.dll          (also win-x86, win-arm64)
    runtimes/linux-x64/native/libsiaara.so      (also linux-arm64)
    runtimes/osx-x64/native/libsiaara.dylib     (also osx-arm64: the same universal file)
    LICENSE, README.md, Siaara.nuspec

How the native libraries of seven separate build jobs get into one package:

1. Each platform job, after its tests and checks, copies its *installed* shared library to
   `native/<rid>/` and uploads it as the artifact `native-<platform>` (the matrix column `rid`;
   macOS uploads the universal library under both `osx-x64` and `osx-arm64` - the tested file,
   not a thinned copy; linux-x86 has no RID because .NET has no 32-bit x86 Linux runtime).
2. The `nuget` job (`needs: build`) downloads all `native-*` artifacts into one folder, builds
   the managed assemblies once with the SDK pinned by `global.json` (`setup-dotnet` with
   `global-json-file`), and runs
   `dotnet pack bindings/csharp/src/Siaara -p:SiaaraNativeArtifactsDir=<folder>
   -p:ContinuousIntegrationBuild=true`. The project packs `<folder>/<rid>/<library>` under
   `runtimes/<rid>/native/`.
3. `tools/release/check_nupkg.py --require-all-rids` fails the job unless all seven runtimes,
   the licence, the readme and the XML documentation are in the package, the nuspec has the
   right id / version / authors / licence file and declares **no dependency**.
4. `check_nupkg.py --consumer-test` copies `examples/csharp/QuickStart` to a temporary folder
   with a `nuget.config` whose only source is a folder holding the new package, and restores,
   builds and runs it with a fresh package cache and a dead proxy. It fails when anything other
   than the Siaara package lands in the package cache. (linux-x64 only: the job runs there. The
   other runtimes' libraries are the files their own platform jobs tested; that the package
   loads them on Windows and macOS is a manual check of RELEASE_CHECKLIST.md.)

Properties of the package: id `Siaara`; authors and company Sthraas Intelligence Pvt Ltd; licence
= the repository's `LICENSE` file inside the package (`PackageLicenseFile`; there is no SPDX
expression for a proprietary licence), acceptance required; `PACKAGE_README.md` as the package
readme; XML documentation; deterministic build with build paths mapped out
(`ContinuousIntegrationBuild`); debug symbols **embedded** in the assemblies (no `.snupkg`, no
symbol server needed); `Siaara.Interop.dll` is inside the package, not a package of its own.

**SDK packs: who needs what.** (Owner's rule: .NET SDK packs such as ILLink and ILCompiler are
fetched only on the release machine / CI, never vendored.)

- *Consuming* the package needs none of them. A framework-dependent application that references
  `Siaara` restores from a folder holding the one `.nupkg`; the consumer test proves it with a
  fresh package cache and no network: afterwards the cache holds `siaara` and nothing else.
- *Building* the package needs one: the managed projects are marked `IsTrimmable` and
  `IsAotCompatible`, which turns on the SDK's trim / AOT analyzers, and the SDK takes those from
  its pack `Microsoft.NET.ILLink.Tasks`, fetched from nuget.org at restore. So the `Pack` step
  of the release job is online and the job fails when anything else is fetched. The pack is not
  a dependency of the package (the nuspec lists none) and not vendored.
- A consumer that publishes self-contained, trimmed or NativeAOT fetches the runtime packs /
  ILCompiler with its own SDK; that is the consumer's build, not this package.

Local pack with only this machine's native library (what CI's `csharp` job does):

    cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel --target siaara
    dotnet pack bindings/csharp/src/Siaara -c Release        # -> bindings/csharp/artifacts/packages
    python3 tools/release/check_nupkg.py bindings/csharp/artifacts/packages/Siaara.<version>.nupkg \
            --version <version> --consumer-test /tmp/nupkg-consumer

## The draft release

The `draft-release` job merges the checksum files into `SHA256SUMS` (archives, SBOMs and the
NuGet package), verifies them, takes the release notes from the topmost section of `CHANGELOG.md`
and runs `gh release create --draft --verify-tag` with every file attached.

## Release candidates (a rehearsal of the workflow)

A tag `vX.Y.Z-rc.N` (X.Y.Z being the version of `CMakeLists.txt`, N from 1) runs the same
`release` workflow. Nothing is edited for it: the binaries are the X.Y.Z binaries and report
X.Y.Z; the archives, the SBOMs and the NuGet package carry the label `X.Y.Z-rc.N`
(`Siaara.X.Y.Z-rc.N.nupkg`; assembly and file versions stay X.Y.Z); the draft is marked as a
**pre-release** and its notes say it is a rehearsal and not for production use. Publishing that
draft pushes the pre-release package to the feed like any other. NuGet orders `X.Y.Z-rc.N` before
`X.Y.Z`, and a consumer gets a pre-release only by naming it. Use the next N for another
rehearsal; never reuse a number. `tools/release/version.py --check-tag` accepts exactly
`vX.Y.Z` and `vX.Y.Z-rc.N`.

## The package feed (GitHub Packages)

`.github/workflows/publish-package.yml` runs when a maintainer **publishes** the draft release.
It downloads `Siaara.<version>.nupkg` and `SHA256SUMS` from that release, checks the package
against the checksum and with `check_nupkg.py --require-all-rids`, and pushes it unchanged to
`https://nuget.pkg.github.com/<owner>/index.json`. Nothing reaches the feed before the draft was
reviewed and published. It can also be run by hand for an existing release (`workflow_dispatch`,
input: the tag).

- A package version on the feed cannot be overwritten. A wrong one is deleted on the package's
  page (owner only) before the corrected one is pushed - and a version that a product already
  restored should get a new number instead.
- The package belongs to the owner's account and is linked to this repository by the repository
  URL inside it. What GitHub's documentation says (read 2026-11-11; "Working with the NuGet
  registry", "Configuring a package's access control and visibility", "About permissions for
  GitHub Packages", "Deleting and restoring a package"):
  - The NuGet registry has granular permissions: the package is scoped to the account, not to
    the repository.
  - **A newly published package is private**, also when the linked repository is public. A
    package linked to a repository inherits the repository's *access permissions*, but **not its
    visibility**. So the repository going private does not change the package, and the package
    does not become public because the repository is.
  - Reading a private package needs an account with read access to it and a personal access
    token (classic) with `read:packages`. Installing needs a token even for public packages.
  - Visibility is changed only by hand (Package settings, "Danger Zone", "Change visibility").
    Leave it private. Older versions of the documentation warn that a package made public
    cannot be made private again; treat making it public as irreversible.
  - A version can be deleted by an admin of the package (the package's page, the version's
    "Delete"), and restored within 30 days as long as the same version was not published again.
  Still to be confirmed on the package's own page after the first push: that it shows
  "Private", and who is listed under "Manage access".
- Consumers need a personal access token with `read:packages` (docs/GETTING_STARTED.md,
  section 6). Storage and data transfer of private packages count against the account's
  GitHub Packages allowance.

## Verifying a release

    sha256sum -c SHA256SUMS
    gh attestation verify siaara-X.Y.Z-linux-x64.tar.gz --repo <owner>/<repository>

Attestations need a public repository or GitHub Enterprise Cloud; on a private repository
without it the attestation step reports its failure and the release goes on without one.

## Code signing

Not done in this release: the binaries (`.dll`, `.exe`, `.dylib`, `.so`) and the `.nupkg` are
unsigned. Consequences and what changes when certificates exist: RELEASE_CHECKLIST.md.

## Reproducibility

- Compile flags map build paths out of the binaries (`-ffile-prefix-map`, MSVC `/pathmap`) and
  MSVC uses `/Brepro`; static archives are written in GNU ar deterministic mode; no source uses
  `__DATE__` / `__TIME__`; the shared library gets a SHA-1 build ID derived from its contents.
- Archives have sorted entries, owner 0/0, normalised permissions, and every timestamp set to
  `SOURCE_DATE_EPOCH`; the gzip header carries no name and the same timestamp.
- The SBOM's creation time is `SOURCE_DATE_EPOCH` and its namespace is derived from version,
  platform and commit.

Two builds of the same commit with the same compiler and runner image should produce identical
archives. This has been checked locally on Linux; it has not been verified on Windows or macOS.
The NuGet package is built deterministically but its byte-for-byte reproducibility has not been
verified.

## SBOM contents

`tools/release/make_sbom.py` (standard library only) lists: the package and every file in it
(SHA-1 and SHA-256); the toolkit's source components (`core`, `dict`, `codec`, `image`, `charset`,
`net`, `web`, `store`, `deid`, `clinical`, `capi`); the data generated from published standards and
mapping tables (PS3.6/PS3.7 dictionary, PS3.15 Table E.1-1, PS3.16 codes, PS3.6 Annex B palettes,
Unicode/WHATWG mappings) and the private-tag dictionary generated from vendor conformance
statements; the OS libraries recorded in the binaries (`readelf -d` / `otool -L` /
`dumpbin /dependents`); and the libraries loaded on demand (system OpenSSL 3 on Linux,
`ncrypt.dll` on Windows) as optional dependencies. The toolkit and its components are declared
under `LicenseRef-Sthraas-Proprietary` with the text of `LICENSE`; the licences of the standards
data and OS libraries are `NOASSERTION`.
