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

What to expect (response times):

- **Acknowledgement** within 5 working days of the report.
- **Triage and assessment** within 15 working days of the report: whether the report is accepted,
  its severity, and the affected versions.
- **Fix target** within 90 days of the report, sooner for issues that are being exploited or are
  easy to trigger remotely. The fix is developed in a private fork and released as a new version
  with a GitHub security advisory that credits the reporter unless they prefer otherwise.

Disclosure is coordinated: please keep the report private until the fix is released or the 90
days have passed, whichever comes first. If a fix needs longer, we will tell you why before the
deadline and agree a new date with you.

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

Every parser has a fuzz target (`fuzz/`): 60 s per target with libFuzzer + ASan + UBSan on every
change, 30 minutes per target nightly, and ClusterFuzzLite on pull requests and daily. CI also
runs ThreadSanitizer over the threaded code, MSVC AddressSanitizer, clang-tidy (new findings fail)
and CodeQL. Release binaries are built with compiler and linker hardening (stack protector,
FORTIFY_SOURCE, full RELRO, CFG/CET where the platform has them; checked on the binaries), link
the C/C++ runtime statically and otherwise only OS libraries (checked), export only the C API
(checked against a baseline), and come with SHA-256 checksums, an SPDX SBOM and GitHub build
provenance attestations.

## Optional OS libraries loaded at run time

On Linux the system OpenSSL 3 (TLS) and, on Linux and macOS, the Vulkan loader (GPU rendering)
are opened with `dlopen` only by absolute path from a fixed list of system library folders
(`src/core/system_library.hpp`): `LD_LIBRARY_PATH`, `DYLD_LIBRARY_PATH`, RUNPATH/RPATH and the
current directory are never searched. Before opening, the file (after resolving symlinks) and its
folder must be owned by root or by the effective user and must not be writable by group or
others; a library that fails a later check is closed again. Windows loads both from System32 only.

The Vulkan loader then chooses GPU drivers and layers from its own environment variables
(`VK_ICD_FILENAMES` / `VK_DRIVER_FILES`, `VK_LAYER_PATH`, `VK_ADD_LAYER_PATH`,
`VK_INSTANCE_LAYERS`). Whoever controls the process environment therefore chooses the driver code
that runs in the process: services that use GPU rendering should start with a clean environment.
