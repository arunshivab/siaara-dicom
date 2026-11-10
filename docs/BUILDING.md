# Building SIAARA

SIAARA needs only a C++20 compiler and CMake 3.20 or newer. There are no third-party
dependencies: the library links nothing but operating-system libraries and the platform C/C++
runtime (checked in CI, see "Release gates" below).

| Platform | Compilers checked in CI | Architectures |
|---|---|---|
| Linux (Ubuntu 24.04) | GCC 13, Clang 18 | x64, x86 (`-m32`), arm64 |
| Windows | MSVC 2022 | x64, x86 (`-A Win32`), arm64 (`-A ARM64`) |
| macOS 15 | Apple Clang | arm64, x64, universal (`CMAKE_OSX_ARCHITECTURES="x86_64;arm64"`) |

## Quick start

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

On Windows (multi-config generator) pass the configuration at build and test time:

    cmake -S . -B build -A x64
    cmake --build build --config Release --parallel
    ctest --test-dir build -C Release --output-on-failure

## Options

| Option | Default | Meaning |
|---|---|---|
| `SIAARA_BUILD_TESTS` | ON | Unit tests (`tests/`), the C API smoke test, the fuzz regression replay (`fuzz_regressions`) and the test-only crosscheck helpers. |
| `SIAARA_BUILD_TOOLS` | ON | Command-line tools: `siaara-dump`, `siaara-echoscu`, `siaara-storescp`, `siaara-webcli`, `siaara-deid`. |
| `SIAARA_WERROR` | ON | Warnings are errors (`-Werror` / `/WX`). The code builds warning-free with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion` and MSVC `/W4`. |
| `SIAARA_SANITIZE` | OFF | AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang; UBSan findings are fatal). With MSVC: `/fsanitize=address` (put the folder of `clang_rt.asan_dynamic-*.dll` on `PATH` to run). |
| `SIAARA_HARDENING` | ON | `cmake/SiaaraHardening.cmake`: stack protector, stack clash protection, `_FORTIFY_SOURCE=3` (optimised builds), CET (`-fcf-protection=full`) on x86, BTI/PAC (`-mbranch-protection=standard`) on ARM64 except Apple platforms (Apple's unwinder cannot unwind through A-key-signed return addresses, so C++ exceptions would terminate), full RELRO and non-executable stack on Linux; MSVC `/guard:cf /Qspectre /sdl`, `/DYNAMICBASE /NXCOMPAT`, x64 `/HIGHENTROPYVA /CETCOMPAT /guard:ehcont`. Debug builds of every target define `_GLIBCXX_ASSERTIONS` and `_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE`. |
| `SIAARA_STATIC_RUNTIME` | OFF | Static C/C++ runtime (release builds turn it on): MSVC `/MT` (applications linking the static library must use `/MT` too), `-static-libstdc++ -static-libgcc` on Linux; macOS uses the system libc++, part of the OS. |
| `SIAARA_LIB_FUZZING_ENGINE` | empty | OSS-Fuzz / ClusterFuzzLite: link flags of the fuzzing engine (`$LIB_FUZZING_ENGINE`); see [FUZZING.md](FUZZING.md). |
| `SIAARA_TSAN` | OFF | ThreadSanitizer (GCC/Clang). Cannot be combined with `SIAARA_SANITIZE`. |
| `SIAARA_FUZZ` | OFF | Fuzz targets in `fuzz/` (see [FUZZING.md](FUZZING.md)). Turns on `SIAARA_SANITIZE`; with Clang and libFuzzer available, everything is also built with `-fsanitize=fuzzer-no-link`. |
| `SIAARA_REPRODUCIBLE` | ON | Build paths are mapped out of the binaries (`-ffile-prefix-map` on GCC/Clang, `/pathmap` and `/Brepro` on MSVC) and static archives are written in GNU ar's deterministic mode. |

Standard CMake variables apply: `CMAKE_BUILD_TYPE`, `CMAKE_INSTALL_PREFIX`,
`CMAKE_OSX_ARCHITECTURES`, `CMAKE_OSX_DEPLOYMENT_TARGET`, `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER`.

## Typical configurations

ASan + UBSan debug build (what the standards require before every change):

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSIAARA_SANITIZE=ON
    cmake --build build -j1 && (cd build && ctest --output-on-failure)

Clang:

    cmake -S . -B build-clang -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
    cmake --build build-clang --parallel && ctest --test-dir build-clang --output-on-failure

32-bit Linux (needs `gcc-multilib g++-multilib`):

    cmake -S . -B build-32 -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32
    cmake --build build-32 -j1 && build-32/tests/siaara_tests

The TLS tests use the system OpenSSL 3 of the same architecture at run time (`libssl3t64:i386`
for the 32-bit build); without it they report TLS as unavailable and skip.

ThreadSanitizer (on kernels with 32 bits of mmap ASLR entropy, such as Ubuntu 24.04 runners,
first run `sudo sysctl vm.mmap_rnd_bits=28`):

    cmake -S . -B build-tsan -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
          -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSIAARA_TSAN=ON -DSIAARA_BUILD_TOOLS=OFF
    cmake --build build-tsan --target siaara_tests
    build-tsan/tests/siaara_tests "net/"      # the test runner takes one name filter (substring)

macOS universal binary:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=x86_64;arm64"

## Installing and using the package

    cmake --install build --prefix /opt/siaara          # add --config Release on Windows

installs:

    bin/        siaara-* tools (and siaara.dll on Windows)
    lib/        libsiaara.so / libsiaara.dylib / siaara.lib, the static library siaara_static,
                lib/cmake/siaara/ (siaaraConfig.cmake, siaaraConfigVersion.cmake, siaaraTargets*.cmake)
    include/    siaara/siaara.h (the C API)

An application then uses:

    find_package(siaara 0.1 REQUIRED CONFIG)                    # CMAKE_PREFIX_PATH=/opt/siaara
    target_link_libraries(app PRIVATE siaara::siaara)            # shared library
    target_link_libraries(app PRIVATE siaara::siaara_static)     # or the static library

`siaara::siaara_static` defines `SIAARA_STATIC` for its users (no `dllimport` on Windows) and brings
the OS libraries it needs (threads, `dl`; `ws2_32 secur32 crypt32` on Windows; the Network,
Security and CoreFoundation frameworks on macOS). `tests/package/` is a minimal consumer project
used by the release workflow.

The shared library exports only the C API (`siaara_*`): a linker version script on Linux
(`src/capi/siaara.map`), an exported-symbols list on macOS (`src/capi/siaara_macos.exports`), and
`__declspec(dllexport)` on Windows.

## Release gates run in CI

| Check | How |
|---|---|
| Dependency allow-list | `python3 tools/ci/check_binaries.py deps <binaries>`: `readelf -d` / `otool -L` / `dumpbin /dependents` against `tools/ci/deps_allowlist.json`; on Linux `libssl`/`libcrypto` must never be linked. |
| Exported-symbol baseline | `python3 tools/ci/check_binaries.py symbols --header include/siaara/siaara.h <shared library>`: only `siaara_*`, equal to `tools/ci/abi/exported_symbols.txt`. After a planned C API change run it with `--update` and commit the baseline diff. |
| Sanitizers | ASan+UBSan jobs (Linux GCC, macOS Apple clang, Windows MSVC ASan), TSan job (`net/`, `net2:`, `capi/`, `web/`, `tls/`, store, scan, parallel decoding, volumes), fuzz smoke test (60 s per target with libFuzzer) and the same targets again with assertions on (`fuzz-asserts`, no `NDEBUG`) |
| Hardening | `python3 tools/ci/check_binaries.py hardening [--fortify] <binaries>`: `readelf` (RELRO + BIND_NOW, NX stack, PIE, stack protector, FORTIFY wrappers), `dumpbin /headers /loadconfig` (DYNAMICBASE, NX, CFG, x64 high-entropy VA and CET), `otool -hv`. On Linux it also reads the `.note.gnu.property` feature bits the linker recorded for the whole binary: IBT and SHSTK (CET) on x86-64, BTI and PAC on ARM64 (`--no-cfi` skips this for builds without `SIAARA_HARDENING`; Apple platforms and 32-bit x86 have no such marking and are not checked). |
| Release runtime | `deps --profile release`: no `libstdc++`/`libgcc_s` (Linux) and no MSVC runtime DLLs (Windows) in release binaries. |
| Generated tables | `tools/ci/fetch_standard.py` downloads PS3.3/4/6/7/15/16 XML pinned by SHA-256 (`tools/ci/dicom_standard.json`); the dictionary, the de-identification table, the IOD requirement tables (`src/repair/iod_tables.inc`) and the named data layer (`src/modules/module_tables.inc`, `Modules.g.cs`) must equal the generators' output. The ctest drift tests are registered only when `SIAARA_NEMA_PART03/04/06/15/16` point at the XML; CI sets all of them and asks for each test by name with `--no-tests=error`. |
| Static analysis | `.clang-tidy` (run with `run-clang-tidy -p <build with compile_commands.json> "src/|tools/"`), gated by `tools/ci/clang_tidy_gate.py --compile-commands <...> --files "src/|tools/" tidy.log`: no finding outside the baseline, and the log must be a complete run (every translation unit analysed, no crash or error line; an empty log fails). CodeQL (`codeql.yml`). |
| .NET SDK | `global.json` pins the exact SDK (no roll-forward); the `csharp` and `app` jobs install it with `setup-dotnet` `global-json-file`. To move to a new SDK change `global.json` only. |
| Action pinning | every `uses:` in `.github/workflows/` is a full commit SHA |

On a private repository: set the repository variables `LINUX_ARM64_RUNNER` / `WINDOWS_ARM64_RUNNER`
to available ARM64 runner labels (GitHub's free ARM64 runners are for public repositories),
`CODEQL_ENABLED=true` only with GitHub Advanced Security; build provenance attestations need
GitHub Enterprise Cloud for private repositories (the release step then reports the failure and
continues).
