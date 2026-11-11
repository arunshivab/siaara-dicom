# Release checklist

The exact steps and gates for cutting a release of the SIAARA DICOM Toolkit. How the machinery
works is in [RELEASING.md](RELEASING.md). Tick every box; a box that cannot be ticked stops the
release until the owner decides otherwise, in writing, in the release notes.

## A. Decisions the owner takes before tagging

None of these is decided by the tooling. Each has a default that applies if nothing is said.

| # | Decision | Options | If nothing is said |
|---|---|---|---|
| 1 | **Version number** of the first release | `1.0.0`, or stay on `0.x` (`0.1.0` today). From 1.0.0 the CMake package promises compatibility within the major version; on 0.x only within the minor version. `SIAARA_ABI_VERSION` stays 1 either way (decided). The Implementation Version Name written into files becomes `SIAARA_100` or stays `SIAARA_010` | `0.1.0` |
| 2 | **LICENSE wording against a public repository.** `LICENSE` says the software and its source code are "proprietary and confidential" and grants no licence. The repository is currently public (attestations rely on that), which contradicts "confidential" for as long as it stays public | (a) make the repository private before or right after the release (planned, step F); (b) keep it public and change the wording (for example drop "and confidential" or add evaluation terms) - wording is the owner's and counsel's, not the tooling's | LICENSE unchanged; repository made private in step F |
| 3 | **Vendor conformance-statement data** under `data/private-tags/` (about 51 MB: rows extracted from 2,914 vendor PDF documents, with document names and page numbers). Two separate questions: (i) does it stay in the repository while that is public; (ii) does it go into the release packages. The dictionary *compiled from it* (names, VRs, VMs per private creator) is in the library in any case | (i) keep / remove from the public history; (ii) `package.py --include-private-tag-sources` to ship it, default not shipped | (i) stays in the repository; (ii) **not** in any package |
| 4 | **Code signing** | sign now (needs certificates, see G), or ship unsigned | **unsigned** (first release; the consumers are the owner's own products) |
| 5 | Whether the release may be published while a gate of section B is open | name the gate and the reason in the release notes | no |

## B. Gates on the commit to be tagged

- [ ] The commit is on `main` and is the commit CI ran on (compare the SHA).
- [ ] **CI is green on that commit**: every job of `ci.yml` (all platforms and compilers,
      ASan + UBSan, TSan, fuzz smoke, dependency allow-list and hardening, exported symbols /
      ABI baseline, clang-tidy, the `csharp` job on three operating systems), `codeql.yml`, and
      the ClusterFuzzLite PR job. The nightly fuzz run has no open finding.
- [ ] **Security review verification**: every finding of the latest security review rounds is
      closed or explicitly accepted by the owner, and the fixes were verified by someone other
      than their author (the review documents in the project record the status per finding).
      Open items that ship are listed in `CHANGELOG.md` under "Known limitations" (today: BTI/PAC
      marking on linux-arm64).
- [ ] **Independent test re-run**: the project's independent testing harness
      (its interoperability crosschecks and public data set runs included) was run again on this commit by the tester, not the developer, and its results
      document has no open P1 / P2 item.
- [ ] `ctest` passes locally in a Release build, including `example_*`,
      `conformance_statement_up_to_date`, `version_single_source`, `csharp_interop_up_to_date`,
      `private_dict_up_to_date`.
- [ ] Local dry run of the release steps on one platform (the steps of `release.yml`, described in
      RELEASING.md): install, `tests/package` consumer, dependency and hardening
      checks, `package.py`, `check_archive.py`, `dotnet pack`, `check_nupkg.py --consumer-test`.
- [ ] `docs/CONFORMANCE.md` was read by a person, not only checked by the test: the hand-written
      text still matches the behaviour (`docs/NETWORKING.md` is the reference for it).
- [ ] `CHANGELOG.md`: the topmost section describes this release; "Known limitations" is
      complete and honest.
- [ ] `SECURITY.md` names the right reporting address for where the repository will live.

## C. Version bump

- [ ] Edit the one line: `project(siaara VERSION x.y.z ...)` in `CMakeLists.txt` (decision A1).
- [ ] Rebuild; `python3 tools/release/gen_conformance.py --dump build/tools/siaara_conformance_dump`
      (the conformance statement quotes the version and the Implementation Version Name).
- [ ] If the C ABI changed since the last release: `SIAARA_ABI_VERSION` and
      `tools/ci/abi/exported_symbols.txt` (not for the first release: ABI version 1).
- [ ] `ctest` again (`version_single_source` and `conformance_statement_up_to_date` pass).
- [ ] Commit ("release x.y.z"), push, wait for CI to be green on that commit (section B again
      for the new SHA - the bump commit is the one that gets tagged).

## D. Tag and build

- [ ] `git tag -a vX.Y.Z -m "SIAARA DICOM Toolkit X.Y.Z"` on the green commit; `git push origin vX.Y.Z`.
- [ ] The `release` workflow runs: seven platform jobs, `nuget`, `draft-release`. All green.
      A red platform job stops everything (`fail-fast`): fix on `main`, delete the tag and the
      draft, start again from B with a new commit. Never move a tag that a published release used.

## E. Check the draft release

- [ ] Assets present: 7 archives (`linux-x64`, `linux-x86`, `linux-arm64`, `windows-x64`,
      `windows-x86`, `windows-arm64`, `macos-universal`), 7 `.spdx.json`, `Siaara.X.Y.Z.nupkg`,
      `SHA256SUMS` (15 lines).
- [ ] `sha256sum -c SHA256SUMS` on downloaded copies.
- [ ] One archive unpacked and looked at: `LICENSE`, `README.md`, `CHANGELOG.md`, `docs/`,
      `examples/`, header, libraries, tools; nothing from `data/` (unless decision A3 says so).
      `tools/release/check_archive.py --list` prints the content.
- [ ] `bin/siaara-dump --help` runs on a clean machine of at least one Windows and one Linux
      platform (no Visual C++ redistributable, no extra libraries).
- [ ] **SBOM**: one `.spdx.json` opened: package version, commit in the comment, licence
      `LicenseRef-Sthraas-Proprietary`, the files, the OS libraries; nothing unexpected under
      dependencies.
- [ ] **NuGet package**: `python3 tools/release/check_nupkg.py Siaara.X.Y.Z.nupkg --version X.Y.Z
      --require-all-rids --list`; then restored and run from a local folder feed in one of the
      owner's products on Windows (the workflow tested linux-x64 only).
- [ ] **Attestation**: `gh attestation verify <archive> --repo <owner>/<repository>` succeeds
      (public repository), or the workflow log shows the documented "attestation not available"
      note (private repository without Enterprise Cloud). Record which.
- [ ] **Code signing status** recorded in the release notes: *not signed* (decision A4). The
      generated notes say so; do not remove the sentence.
- [ ] Release notes read through; publish the draft.

## F. After publishing

- [ ] The `publish-package` workflow ran (it starts when the release is published) and is green;
      `Siaara` X.Y.Z is listed under the owner's Packages. Visibility and access of the package
      checked (docs/RELEASING.md, "The package feed"). One of the owner's products restores it
      from `https://nuget.pkg.github.com/<owner>/index.json`.
- [ ] Download the published assets once more from the release page and keep a copy with
      `SHA256SUMS` outside GitHub (the owner's archive).
- [ ] **Make the repository private** (decision A2), after saving: the attestation
      verification output for each asset (it cannot be re-verified by outsiders afterwards), the
      workflow run logs, and the CodeQL / fuzzing results. Check afterwards that Actions still
      run (minutes are billed on private repositories; the ARM64 runners in particular), that
      `SECURITY.md`'s private reporting link still fits, and that forks, if any, are dealt with.
      Attestations of later releases will need GitHub Enterprise Cloud or will be skipped.
- [ ] Tell the consuming products: version, `SIAARA_ABI_VERSION`, where the package is, the
      known limitations.
- [ ] Open the next cycle: the second release's configurability programme (options audit, 257
      items) and the open items carried in "Known limitations".

## G. What changes when signing certificates are bought

Today nothing is signed. With certificates, add (in `release.yml`, after the tests and before
packaging, so the signed files are the packaged and attested ones):

| Platform | What gets signed | With | Notes |
|---|---|---|---|
| Windows | `siaara.dll`, every `siaara-*.exe` | Authenticode (`signtool`, SHA-256, RFC 3161 timestamp), an OV or EV code-signing certificate held in an HSM or a cloud signing service (private keys of new certificates are not exportable) | Removes SmartScreen / "unknown publisher" warnings; signing changes the files, so it must come before `package.py`, the SBOM and the checksums |
| macOS | `libsiaara.dylib`, the tools | `codesign` with a Developer ID Application certificate, hardened runtime; **notarization** of the archive (`notarytool`) | Without it Gatekeeper quarantines downloaded binaries; today's binaries carry only the linker's ad-hoc signature |
| NuGet | `Siaara.X.Y.Z.nupkg` | `dotnet nuget sign` with a code-signing certificate and timestamp | Consumers can then require signed packages (`signatureValidationMode=require`) |
| Linux | archives | a detached signature of `SHA256SUMS` (for example `cosign sign-blob` or GPG) | The attestation already covers provenance; this adds a publisher signature |

Also: store the certificates' secrets as GitHub environment secrets with required reviewers on
the `release` environment; re-check reproducibility (signatures carry timestamps: the *unsigned*
build stays reproducible, the signed files are not); update `CHANGELOG.md` and the release-notes
sentence generated by `tools/release/version.py`; the dependency / hardening / symbol checks run
on the signed files.
