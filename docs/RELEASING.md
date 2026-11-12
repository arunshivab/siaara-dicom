# Releasing SIAARA

Releases are built by `.github/workflows/release.yml` when a tag `v<version>` is pushed. The
workflow produces a **draft** GitHub release; a maintainer reviews it and publishes it by hand.
Nothing is built unless the gate passes (below): the tagged commit is on `main` and `ci.yml`
passed on exactly that commit.
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

## The gate (RL-02, R-217)

The first job, `gate`, runs before any build (every other job needs it):

1. **The tagged commit is on `main`**: it fetches `main` and checks
   `git merge-base --is-ancestor <tagged commit> origin/main`. A tag on a branch, or on a commit
   that was never merged, stops the release with an error.
2. **`ci.yml` passed on that exact commit**:
   `gh run list --workflow ci.yml --commit <tagged commit>` must show at least one run that is
   completed with conclusion `success`. A run still in progress, cancelled (a later push to
   `main` cancels the running CI of the earlier commit) or failed does not count.

When the gate fails: wait for CI on that commit to finish (or re-run it from the Actions page),
then re-run the release workflow (Actions, the failed `release` run, "Re-run all jobs"). Tag only
a commit whose CI is green and that is on `main`; never move a tag.

## What the workflow does, per platform

Platforms: linux-x64, linux-x86, linux-arm64, windows-x64, windows-x86, windows-arm64,
macos-universal (`CMAKE_OSX_ARCHITECTURES="x86_64;arm64"`, deployment target 14.0).
The legs run with `fail-fast: false`: a failing platform does not cancel the others, so one run
shows every platform that fails (RL-01). The release is still blocked, because `nuget` and
`draft-release` need every leg.

1. Checks that the tag is `v` + the version of `CMakeLists.txt`.
2. Sets `SOURCE_DATE_EPOCH` to the commit time.
3. Builds Release (static C/C++ runtime, hardening on) and runs the full test suite (`ctest`),
   which includes the examples, the conformance statement check and the version check. On macOS
   it also runs the tests under Rosetta for the x86_64 slice and checks that the library
   contains both slices. A runner without Rosetta fails the leg (RL-03): the x86_64 slice is
   never shipped untested.
4. Installs into `stage/siaara-<version>-<platform>/` (`bin/`, `lib/` with the shared and static
   libraries, `lib/cmake/siaara/`, `lib/pkgconfig/siaara.pc`, `include/siaara/siaara.h`).
5. Builds `tests/package/` against the installed tree with `find_package(siaara)` and runs the
   shared and static consumers; they check the ABI version and that the library reports the
   tagged version, and write and read back a file. Then builds the shipped `examples/` against
   the installed tree and runs them with `ctest`. Both are `tools/release/check_installed.sh`
   (`consumer` and `examples`), which the Intel Mac job below runs too. On Linux the pkg-config
   file is tested the same way.
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
8. Attests build provenance for the archive and SBOM with `actions/attest-build-provenance`, and
   copies the attestation's Sigstore bundle next to the archive as
   `siaara-<version>-<platform>.sigstore.json` (one bundle covers the archive and its SBOM), so
   it is attached to the release and can be verified without GitHub's attestation store - also
   after the repository goes private (RL-05, R-209). When no attestation could be made (private
   repository without GitHub Enterprise Cloud: the step has
   `continue-on-error: ${{ github.event.repository.private }}`), the leg prints a warning and
   the release notes say which files have none (RL-06).
9. Uploads the installed shared library as `native-<platform>` for the NuGet job.

### The Intel Mac test (`macos-intel-test`, RL-03)

The macOS universal leg runs on an Apple silicon runner, where the x86_64 slice runs only under
Rosetta. The job `macos-intel-test` (`macos-15-intel`, needs `build`) downloads the artifact
`dist-macos-universal`, checks the archive against its `.sha256`, unpacks
`siaara-<version>-macos-universal.tar.gz` as a consumer would, checks that the runner is x86_64
and the library has the x86_64 slice, and runs `tools/release/check_installed.sh consumer` and
`examples` against the unpacked tree, built for x86_64 only. `nuget` and `draft-release` need
this job.

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
    [Content_Types].xml, _rels/.rels, package/services/metadata/core-properties/<id>.psmdcp   (NuGet's metadata)

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
   A guard right after `dotnet pack` fails the job when `dist/Siaara.<label>.nupkg` was not
   produced under exactly that name. (The version is kept in the environment as
   `SIAARA_VERSION`, never `VERSION`: MSBuild reads environment variables as properties,
   case-insensitively, and `VERSION=0.1.0` set `$(Version)` and dropped the `rc.N` suffix, so
   the v0.1.0-rc.1 run packed `Siaara.0.1.0.nupkg`.)
3. `tools/release/check_nupkg.py --require-all-rids` fails the job unless all seven runtimes,
   the licence, the readme and the XML documentation are in the package, the nuspec has the
   right id / version / authors / licence file and declares **no dependency**. It also holds the
   package to an **allow-list** (R-218): only the files listed above (plus an `icon.png`, if one
   is ever added) may be in it; any other entry fails the check and is named. Its unit tests:
   `tools/release/test_check_nupkg.py` (`ctest -R release_check_nupkg`).
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

After the package checks, the `nuget` job signs the package with the SSH release key
(`Siaara.<label>.nupkg.sig`, see "The SSH release signature"), attests its provenance and copies
the Sigstore bundle to `Siaara.<label>.nupkg.sigstore.json`.

## The draft release

The `draft-release` job (needs `build`, `nuget` and `macos-intel-test`) merges the checksum files
into `SHA256SUMS` (archives, SBOMs and the NuGet package), verifies them, signs every archive and
`SHA256SUMS` with the SSH release key (`<file>.sig`), and takes the release notes from the
topmost section of `CHANGELOG.md`. Near the top, the notes get a "Provenance and signatures"
paragraph; it names every file that has **no** attestation bundle or **no** SSH signature
(RL-06), and the job prints the same as warnings.

Files attached to a release (X.Y.Z, or X.Y.Z-rc.N):

| File | What |
|---|---|
| `siaara-X.Y.Z-<platform>.tar.gz` / `.zip` (7) | the archive |
| `siaara-X.Y.Z-<platform>.spdx.json` (7) | its SBOM |
| `siaara-X.Y.Z-<platform>.sigstore.json` (7) | Sigstore bundle of the provenance attestation of the archive and its SBOM (absent when no attestation could be made) |
| `siaara-X.Y.Z-<platform>.tar.gz.sig` / `.zip.sig` (7) | SSH release signature of the archive (absent without the signing key) |
| `Siaara.X.Y.Z.nupkg` | the NuGet package |
| `Siaara.X.Y.Z.nupkg.sigstore.json` | Sigstore bundle of its attestation |
| `Siaara.X.Y.Z.nupkg.sig` | its SSH release signature |
| `SHA256SUMS` | SHA-256 of the archives, SBOMs and the package (15 lines) |
| `SHA256SUMS.sig` | its SSH release signature |

**Re-running the workflow (RL-04).** The last step looks for a release of the tag first:

- none: it creates the draft (`gh release create --draft --verify-tag`, `--prerelease` for an
  `-rc.N` tag, title "SIAARA DICOM Toolkit X.Y.Z", the notes above, every file attached);
- a **draft** (an earlier run of the same tag): it re-uploads every file, replacing those of the
  same name (`gh release upload --clobber dist/*`), and replaces the notes
  (`gh release edit --notes-file`). Files of the earlier run that this run did not produce (for
  example a `.sig` when the key has since been removed) stay on the draft: delete them by hand;
- a **published** release: it fails with an error and changes nothing. A published release is
  never replaced; release a new version (or the next `-rc.N`).

So after a failure of one platform (a flaky runner, for example) use "Re-run failed jobs" on the
same run, or "Re-run all jobs"; the draft is refreshed, not duplicated.

## Release candidates (a rehearsal of the workflow)

A tag `vX.Y.Z-rc.N` (X.Y.Z being the version of `CMakeLists.txt`, N from 1) runs the same
`release` workflow. Nothing is edited for it: the binaries are the X.Y.Z binaries and report
X.Y.Z; the archives, the SBOMs and the NuGet package carry the label `X.Y.Z-rc.N`
(`Siaara.X.Y.Z-rc.N.nupkg`; assembly and file versions stay X.Y.Z); the draft is marked as a
**pre-release** and its notes say it is a rehearsal and not for production use. Publishing that
draft does **not** push the package to the feed (RL-13): a release candidate reaches the feed
only by a deliberate manual run of `publish-package`, after the rc draft was published:

    gh workflow run publish-package.yml -R arunshivab/siaara-dicom -f tag=v0.1.0-rc.1

NuGet orders `X.Y.Z-rc.N` before `X.Y.Z`, and a consumer gets a pre-release only by naming it. Use the next N for another
rehearsal; never reuse a number. `tools/release/version.py --check-tag` accepts exactly
`vX.Y.Z` and `vX.Y.Z-rc.N`.

## The package feed (GitHub Packages)

`.github/workflows/publish-package.yml` runs when a maintainer **publishes** the draft release
(not for a pre-release: see "Release candidates"). It:

1. checks that the tag is exactly `vX.Y.Z` or `vX.Y.Z-rc.N`, **before** anything is checked out,
   then checks out that tag (`refs/tags/<tag>`, depth 1), so the checks below are the tagged
   commit's;
2. downloads `Siaara.<version>.nupkg` and `SHA256SUMS` (required), and
   `Siaara.<version>.nupkg.sigstore.json` and `Siaara.<version>.nupkg.sig` (each optional);
3. checks the package against `SHA256SUMS` and with `check_nupkg.py --require-all-rids`
   (allow-list included);
4. **verifies its provenance** - at least one must succeed, otherwise nothing is pushed:
   - the attestation, from the attached bundle when there is one, or else (public repository
     only) from GitHub's attestation store: `gh attestation verify` with `--repo`,
     `--signer-workflow <owner>/<repo>/.github/workflows/release.yml`,
     `--source-ref refs/tags/<tag>` and `--deny-self-hosted-runners`. A bundle that is attached
     but does not verify fails the job;
   - the SSH release signature, when `.sig` is attached and `.github/release-signers` is in the
     tagged commit. A signature that does not verify fails the job.

   So on a **private** repository without an attestation bundle, the package can be pushed only
   when it carries an SSH signature that verifies.
5. pushes it unchanged to `https://nuget.pkg.github.com/<owner>/index.json`. When the version is
   already on the feed (HTTP 409), the job fails with the error "already on the feed, a version
   can't be replaced; delete it by hand first" (RL-11; below).

Nothing reaches the feed before the draft was reviewed and published. It can also be run by hand
for an existing, published release (`workflow_dispatch`, input `tag`), for example
`gh workflow run publish-package.yml -R arunshivab/siaara-dicom -f tag=v0.1.0` (a release
candidate goes to the feed only this way).

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

Download the assets of the release into one folder, together with the `release-signers` line
(the committed `.github/release-signers`, or the owner's copy kept outside GitHub). In Bash
(Linux, macOS, Git Bash on Windows):

    sha256sum -c SHA256SUMS                    # macOS: shasum -a 256 -c SHA256SUMS
    # SSH release signature (any file with a .sig; SHA256SUMS covers the archives, SBOMs, package)
    ssh-keygen -Y verify -f release-signers -I release@siaara -n siaara-release -s SHA256SUMS.sig < SHA256SUMS
    ssh-keygen -Y verify -f release-signers -I release@siaara -n siaara-release \
        -s Siaara.X.Y.Z.nupkg.sig < Siaara.X.Y.Z.nupkg
    # provenance from the attached bundle (no attestation store needed; gh must be signed in)
    gh attestation verify siaara-X.Y.Z-linux-x64.tar.gz --bundle siaara-X.Y.Z-linux-x64.sigstore.json \
        --repo arunshivab/siaara-dicom \
        --signer-workflow arunshivab/siaara-dicom/.github/workflows/release.yml --source-ref refs/tags/vX.Y.Z
    gh attestation verify Siaara.X.Y.Z.nupkg --bundle Siaara.X.Y.Z.nupkg.sigstore.json \
        --repo arunshivab/siaara-dicom \
        --signer-workflow arunshivab/siaara-dicom/.github/workflows/release.yml --source-ref refs/tags/vX.Y.Z
    # while the repository is public, also without the bundle:
    gh attestation verify siaara-X.Y.Z-linux-x64.tar.gz --repo arunshivab/siaara-dicom

"Good "siaara-release" signature for release@siaara" is the success line of `ssh-keygen`. In
Windows PowerShell, `<` does not exist; run the same `ssh-keygen` line through `cmd`, for example
`cmd /c "ssh-keygen -Y verify -f release-signers -I release@siaara -n siaara-release -s SHA256SUMS.sig < SHA256SUMS"`,
and compare checksums with `Get-FileHash -Algorithm SHA256 <file>`.

Attestations need a public repository or GitHub Enterprise Cloud; on a private repository
without it the attestation step reports its failure, the release goes on without one, and the
notes say so. The SSH signature does not depend on GitHub at all.

## The SSH release signature (RL-05, R-209)

A detached signature made with `ssh-keygen -Y sign` (namespace `siaara-release`) by a key that
only the owner holds; the workflows read it from the secret `SIAARA_RELEASE_SIGNING_KEY`. The
matching public key is committed as `.github/release-signers`, one line in the OpenSSH
"allowed signers" format (the template is `.github/release-signers.example`):

    release@siaara namespaces="siaara-release" ssh-ed25519 AAAA... release@siaara

- No secret: the signing steps print a warning (`SIAARA_RELEASE_SIGNING_KEY is not set: no SSH
  signature`) and the release goes on without `.sig` files; the notes say so.
- Secret, but no `.github/release-signers` in the tagged commit: the files are signed, the
  workflow's own check of the signatures is skipped with a warning, and `publish-package` cannot
  use the signature.
- Both: every signature is verified in the job that made it; a mismatch (wrong key in the
  secret) fails the job.

This is a publisher signature of the files, not code signing (see below): Windows and macOS do
not look at it.

### Creating the key (owner, once; Windows PowerShell)

The workflows cannot type a passphrase, so the key has none: it lives only in the GitHub secret
and in the owner's own backup. Steps (PowerShell, the GitHub CLI signed in):

1. Make the key pair. At both passphrase prompts press Enter (empty passphrase):

       New-Item -ItemType Directory -Force C:\Users\aruns\.ssh
       ssh-keygen -t ed25519 -C release@siaara -f C:\Users\aruns\.ssh\siaara-release

   This writes `C:\Users\aruns\.ssh\siaara-release` (private) and
   `C:\Users\aruns\.ssh\siaara-release.pub` (public).
2. Put the private key into the repository secret, and check that it is listed:

       Get-Content -Raw C:\Users\aruns\.ssh\siaara-release | gh secret set SIAARA_RELEASE_SIGNING_KEY -R arunshivab/siaara-dicom
       gh secret list -R arunshivab/siaara-dicom

3. Commit the public line as `.github/release-signers` on `main` (a small sparse clone, so the
   51 MB of `data/` is not downloaded):

       git clone --depth 1 --filter=blob:none --sparse https://github.com/arunshivab/siaara-dicom C:\Users\aruns\siaara-signers
       cd C:\Users\aruns\siaara-signers
       git sparse-checkout set .github
       $pub = (Get-Content -Raw C:\Users\aruns\.ssh\siaara-release.pub).Trim()
       [IO.File]::WriteAllText("C:\Users\aruns\siaara-signers\.github\release-signers", "release@siaara namespaces=`"siaara-release`" $pub`n")
       Get-Content C:\Users\aruns\siaara-signers\.github\release-signers
       git add .github/release-signers
       git -c user.name="Arun Shiva B" -c user.email="arunshiva@gmail.com" commit -m "release: SSH release signer (public key)"
       git push origin main

   (`WriteAllText` writes the line with a Unix line ending and no byte-order mark, which is
   what `ssh-keygen` expects.) The printed line must start with
   `release@siaara namespaces="siaara-release" ssh-ed25519 AAAA` and end with `release@siaara`.
4. Optional local check that the key and the line match:

       Set-Content -Path C:\Users\aruns\siaara-sign-test.txt -Value "test"
       ssh-keygen -Y sign -f C:\Users\aruns\.ssh\siaara-release -n siaara-release C:\Users\aruns\siaara-sign-test.txt
       cmd /c "ssh-keygen -Y verify -f C:\Users\aruns\siaara-signers\.github\release-signers -I release@siaara -n siaara-release -s C:\Users\aruns\siaara-sign-test.txt.sig < C:\Users\aruns\siaara-sign-test.txt"

   It must print `Good "siaara-release" signature for release@siaara`.
5. Back up `C:\Users\aruns\.ssh\siaara-release` offline (a GitHub secret cannot be read
   back), and keep a copy of the `release-signers` line outside GitHub: after the repository
   goes private, that copy is what outsiders verify against.

The commit of step 3 must be **in the tagged commit** (tag after it, once CI is green on it): the
release and `publish-package` workflows read `.github/release-signers` from the tag.

If the key is lost or leaks: make a new pair (steps 1-3, replacing the line), and say in the
next release notes from which version on the new key signs.

## Code signing

Not done in this release: the binaries (`.dll`, `.exe`, `.dylib`, `.so`) and the `.nupkg` are
not code signed (the SSH release signature above is a detached signature of the released files,
not an Authenticode, Apple or NuGet signature). Consequences and what changes when certificates exist: RELEASE_CHECKLIST.md.

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
