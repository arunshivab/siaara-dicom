#!/bin/bash -eu
# ClusterFuzzLite / OSS-Fuzz build script. The environment provides CC, CXX, CFLAGS, CXXFLAGS
# (sanitizer + coverage instrumentation for the selected sanitizer) and LIB_FUZZING_ENGINE.
# Builds every target in fuzz/targets/ as $OUT/fuzz_<target>, with its seed corpus
# ($OUT/fuzz_<target>_seed_corpus.zip from fuzz/corpus/<target> and tests/regress/<target>).
cd "$SRC/siaara-dicom"
cmake -S . -B build-oss -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
  -DSIAARA_LIB_FUZZING_ENGINE="$LIB_FUZZING_ENGINE" \
  -DSIAARA_BUILD_TESTS=OFF -DSIAARA_BUILD_TOOLS=OFF -DSIAARA_WERROR=OFF
# The lines of set(SIAARA_FUZZ_TARGETS ...) without the first one; names are never edited
# (removing the word "set" everywhere once turned the target charset into char).
targets=$(sed -n '/set(SIAARA_FUZZ_TARGETS$/,/)/p' fuzz/CMakeLists.txt | sed '1d' | tr -d '()')
for t in $targets; do
  cmake --build build-oss --target "fuzz_$t"
  cp "build-oss/fuzz/fuzz_$t" "$OUT/"
  seeds=()
  for d in "fuzz/corpus/$t" "tests/regress/$t"; do [ -d "$d" ] && seeds+=("$d"); done
  if [ ${#seeds[@]} -gt 0 ]; then
    (find "${seeds[@]}" -type f ! -name 'README*' -print0 | xargs -0 zip -q -j "$OUT/fuzz_${t}_seed_corpus.zip")
  fi
done
