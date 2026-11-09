# Security policy

SIAARA parses untrusted input from files, the network (DICOM upper layer, DICOMweb) and removable
media. Reports of vulnerabilities are welcome and are handled privately.

## Reporting a vulnerability

Please use GitHub's private vulnerability reporting: open the repository's **Security** tab and
choose **Report a vulnerability** (https://github.com/arunshivab/siaara-dicom/security/advisories/new).
Do not open a public issue, pull request or discussion for a suspected vulnerability.

A useful report contains:

- the affected version or commit, platform and compiler;
- the API, tool or service involved (for example `read_part10`, the JPEG 2000 decoder, the
  association acceptor, the DICOMweb server);
- a minimal input that reproduces the problem (a fuzz target input is ideal: see
  [docs/FUZZING.md](docs/FUZZING.md) for `fuzz_<target> <file>`), and the sanitizer report if any;
- the impact you see (memory corruption, crash, hang, excessive memory, information disclosure,
  de-identification failure).

Do not include real patient data. If a reproduction needs an image, de-identify it or build a
synthetic one.

What to expect:

- an acknowledgement within 5 working days;
- an assessment (accepted or not, severity) within 15 working days;
- a fix developed in a private fork, released as a new version with a GitHub security advisory
  that credits the reporter unless they prefer otherwise. The target is 90 days from the report
  for a fix, sooner for issues that are being exploited or are easy to trigger remotely.

## Supported versions

SIAARA is pre-1.0. Security fixes are made on `main` and published in the next release; there are
no long-term support branches yet.

| Version | Supported |
|---|---|
| latest 0.x release | yes |
| `main` | yes (fixes land here first) |
| older 0.x releases | no - upgrade to the latest release |

This table will be extended when 1.0 is released.

## Scope

In scope: memory-safety bugs, crashes, hangs and unbounded resource use on any input; protocol
state-machine errors; TLS configuration flaws in the toolkit's TLS adapter; de-identification
that leaves identifying data that the selected profile requires to be removed; path traversal in
the store or DICOMDIR handling.

Out of scope: vulnerabilities in the operating system's TLS stack or C/C++ runtime (report those
upstream), and issues that need an attacker who already controls the process.

## Hardening in place

Every parser has a fuzz target (`fuzz/`), run for 60 s per target on every change and 30 minutes
per target nightly with ASan and UBSan; ThreadSanitizer covers the threaded code; release binaries
link only OS libraries (checked), export only the C API (checked against a baseline), and come
with SHA-256 checksums, an SPDX SBOM and GitHub build provenance attestations.
