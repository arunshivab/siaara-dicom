# Fuzzing SIAARA

Every parser of untrusted input has a fuzz target in `fuzz/targets/`. Each target is one source
file built three ways:

| Build | What it is | Compilers |
|---|---|---|
| `fuzz_<target>` (with `-DSIAARA_FUZZ=ON`) | libFuzzer binary, ASan + UBSan, coverage-guided | Clang with libFuzzer |
| `fuzz_<target>_replay` (with `-DSIAARA_FUZZ=ON`) | runs files or folders through the target, no libFuzzer | any (GCC, Clang, MSVC) |
| `siaara_fuzz_replay` (always, with the tests) | all targets in one executable; ctest's `fuzz_regressions` runs it over `fuzz/corpus/` and `tests/regress/` | any |

Without libFuzzer (GCC, MSVC, or Clang without the runtime) `fuzz_<target>` is a replay executable too.

## Targets

| Target | What it feeds | Properties checked besides "no crash, no sanitizer report, no hang" |
|---|---|---|
| `part10` | `read_part10` lenient / strict / header-only; `read_dataset` in a syntax picked by the first byte | a parsed file is written back and must parse again |
| `codec_rle` | RLE decoder (4-byte frame description + RLE frame) | decoded frames re-encode and round-trip |
| `codec_jpeg` | JPEG decoder (frame description from the codestream header, then varied) | |
| `codec_jpegls` | JPEG-LS decoder | lossless decodes re-encode and round-trip |
| `codec_j2k` | JPEG 2000 / HTJ2K probe, component decoder, frame decoder | |
| `codec_inflate` | raw DEFLATE with output caps | deflate(input) inflates back to the input |
| `frames_render` | frame reader (native, every codec) and the 2D render pipeline; last byte picks render options | |
| `charset` | Specific Character Set parsing, decoding and re-encoding (`<0008,0005 value>\0<text>`) | output is valid UTF-8; UTF-8 delimiters preserved |
| `pdu` | `parse_pdu`, `read_pdu` from a chunked peer | encode(parse(x)) is stable |
| `assoc` | association state machine fed from a peer: acceptor (negotiate, accept, serve DIMSE, release) or requestor; first byte picks role and chunk size | |
| `dimse` | DIMSE command decoder | encode(decode(x)) is stable |
| `http` | HTTP/1.1 requests (pipelined, chunked) or a response; media type / Accept / target / URL helpers | |
| `multipart` | multipart/related parser (`<boundary>\n<body>`) | part bodies lie inside the input |
| `multipart_stream` | incremental multipart parser used by the streaming retrieve (`<split seed byte><boundary>\n<body>`) | any split of the body gives the same parts as one piece; whatever the whole-body parser accepts is accepted with the same parts |
| `json`, `xml` | JSON and XML parsers | JSON: write(parse(x)) is stable and valid UTF-8 |
| `dicom_json`, `dicom_xml` | DICOM JSON and Native XML model readers | accepted documents written back read again |
| `dicomdir` | DICOMDIR reader (written to a scratch file) | record depth within the limit |
| `dicomdir_memory` | DICOMDIR reader from bytes (`siaara_dicomdir_read_memory`), default and small record limits | depth within the limit; a File ID reported well formed is 1 to 8 components of A-Z 0-9 _ |
| `display_text` | display-safe text cleaner (`siaara_text_display_safe`) | output is UTF-8 without controls, separators or format characters, at most 3 x the input, cleaning again changes nothing, a cut copy ends on a character |
| `store_journal` | store snapshot load and journal replay at `Store::open` (records framed with valid CRCs by the harness) | after compaction, reopening gives the same counts |
| `deid` | de-identifier on a parsed file with PHI planted at the top level and in sequence items | the planted marker never appears in the output |
| `clinical` | SR, SEG (and SEG frame masks), RT Structure Set and presentation state readers | |
| `values` | DS / IS / DA / TM / DT parsers, date ranges, date shifting | `format_ds` output fits 16 characters and parses back |

## Building and running

    sudo apt-get install clang-18 libclang-rt-18-dev       # Ubuntu: libFuzzer + sanitizer runtimes
    cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
          -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSIAARA_FUZZ=ON -DSIAARA_BUILD_TESTS=OFF -DSIAARA_BUILD_TOOLS=OFF
    cmake --build build-fuzz --parallel

All targets, 60 s each, four at a time (what CI runs on every change):

    python3 fuzz/run_fuzzers.py --bin build-fuzz/fuzz --seconds 60 --jobs 4

One target, longer, continuing after findings (what the nightly workflow runs, 30 min each):

    python3 fuzz/run_fuzzers.py --bin build-fuzz/fuzz --targets part10 --seconds 1800 --keep-going

Each target reads the committed seeds (`fuzz/corpus/<target>/`), the regression inputs
(`tests/regress/<target>/`) and any `--extra` roots, and writes new inputs to
`<work>/<target>/corpus/` (default work folder `build-fuzz/fuzz-work/`). Findings land in
`<work>/<target>/artifacts/` as `crash-*`, `timeout-*`, `oom-*` or
`leak-*` (hang limit `--hang-seconds`, default 10 s; memory limit `--rss-mb`, default 2048 MB, as the security review R-012 asks), with the log in `<work>/<target>/fuzz.log`.

The libFuzzer binaries accept the usual flags directly, for example

    build-fuzz/fuzz/fuzz_pdu -max_total_time=600 -timeout=2 build-fuzz/fuzz-work/pdu/corpus fuzz/corpus/pdu

The store and DICOMDIR targets write to a scratch folder under the system temp folder; point
`TMPDIR` at a RAM disk (`/dev/shm`) to make them faster.

## Seeds

    python3 fuzz/make_seeds.py --tool build/fuzz/siaara_make_seeds --out fuzz/corpus --clean
    python3 fuzz/make_seeds.py --tool build/fuzz/siaara_make_seeds --out /tmp/corpus-full --pydicom

`siaara_make_seeds` (`fuzz/make_seeds.cpp`) writes small valid inputs for every target with the
toolkit's own writers and encoders, in each harness's input layout; that output is the committed
`fuzz/corpus/` (well under 5 MB). `--pydicom` adds pydicom's test files (whole files for the
Part 10 based targets, extracted encapsulated frames for the codec targets, its JSON files and
DICOMDIR set) at seed-generation time; those are not committed. Pass the folder with `--extra`.

## When a target finds something

1. Reproduce: `build-fuzz/fuzz/fuzz_<target> <artifact>` (or `fuzz_<target>_replay <artifact>`
   with any compiler).
2. Minimise: `build-fuzz/fuzz/fuzz_<target> -minimize_crash=1 -runs=10000 <artifact>` (for a
   property failure or hang, `-exact_artifact_path=min.bin` keeps the smallest one).
3. Add the minimised input as `tests/regress/<target>/<short-description>.<ext>`; ctest's
   `fuzz_regressions` replays it from then on, on every platform, with a 10 s per-input limit.
4. Fix the library, with a unit test next to the code's other tests where the bug is reachable
   through the public API.

`fuzz/corpus/**` and `tests/regress/**` are marked binary in `.gitattributes` so line endings are
never converted.

## ClusterFuzzLite and OSS-Fuzz

`.clusterfuzzlite/` (Dockerfile on the pinned OSS-Fuzz `base-builder` image, `build.sh`,
`project.yaml`) builds the same targets the OSS-Fuzz way: the environment supplies `CC`, `CXX`,
`CFLAGS`, `CXXFLAGS` and `LIB_FUZZING_ENGINE`, and `build.sh` passes the engine as
`-DSIAARA_LIB_FUZZING_ENGINE=...` (then CMake adds no sanitizer or fuzzer flags of its own), copies
`fuzz_<target>` to `$OUT` with `fuzz_<target>_seed_corpus.zip` (from `fuzz/corpus/` and
`tests/regress/`). Workflows: `cflite_pr.yml` (code-change fuzzing of pull requests, ASan and
UBSan, 10 min), `cflite_batch.yml` (daily, 1 h), `cflite_cron.yml` (weekly corpus pruning and a
coverage report). The same folder can be used for an OSS-Fuzz project once the repository has a
license.
