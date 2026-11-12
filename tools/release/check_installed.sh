#!/usr/bin/env bash
# Checks an installed SIAARA package the way a consumer uses it (release.yml: every platform leg,
# and the macos-intel-test job on the unpacked macOS universal archive).
#
#     bash tools/release/check_installed.sh consumer <prefix> <version> [cmake configure args...]
#     bash tools/release/check_installed.sh examples <prefix> [cmake configure args...]
#
# consumer: builds tests/package against <prefix> with find_package(siaara), checks that the
#           package and siaara_version_string() are <version>, and runs the shared and the static
#           consumer (build-consumer/).
# examples: builds the shipped examples (examples/) against <prefix> and runs them with ctest
#           (build-examples/).
# Run from the repository root. <prefix> must be an absolute path. The shared library is found
# through PATH (Windows), DYLD_LIBRARY_PATH (macOS) or LD_LIBRARY_PATH (Linux).
set -euo pipefail

usage() { echo "usage: $0 consumer <prefix> <version> [cmake args...] | examples <prefix> [cmake args...]" >&2; exit 2; }
[ $# -ge 2 ] || usage
what=$1 prefix=$2
shift 2

os=${RUNNER_OS:-}
if [ -z "$os" ]; then
  case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) os=Windows ;;
    Darwin) os=macOS ;;
    *) os=Linux ;;
  esac
fi

# Runs "$@" with the shared library of <prefix> on the loader's search path.
with_lib() {
  case "$os" in
    Windows) PATH="$prefix/bin:$PATH" "$@" ;;
    macOS) DYLD_LIBRARY_PATH="$prefix/lib" "$@" ;;
    *) LD_LIBRARY_PATH="$prefix/lib" "$@" ;;
  esac
}

case "$what" in
  consumer)
    [ $# -ge 1 ] || usage
    version=$1
    shift
    cmake -S tests/package -B build-consumer "$@" -DCMAKE_PREFIX_PATH="$prefix" \
      -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DSIAARA_EXPECT_VERSION="$version"
    cmake --build build-consumer --config Release
    if [ "$os" = "Windows" ]; then
      with_lib build-consumer/Release/consumer_shared.exe
      build-consumer/Release/consumer_static.exe
    else
      with_lib build-consumer/consumer_shared
      build-consumer/consumer_static
    fi
    ;;
  examples)
    cmake -S examples -B build-examples "$@" -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
    cmake --build build-examples --config Release
    with_lib ctest --test-dir build-examples -C Release --output-on-failure --no-tests=error
    ;;
  *)
    usage
    ;;
esac
