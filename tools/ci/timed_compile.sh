#!/bin/sh
# Compiler launcher for CI (CMAKE_<LANG>_COMPILER_LAUNCHER): runs the compile unchanged and
# prints the source file of any compile that takes longer than SIAARA_SLOW_COMPILE_S seconds
# (default 60), so a slow translation unit is named in the job log.
limit=${SIAARA_SLOW_COMPILE_S:-60}
start=$(date +%s)
"$@"
rc=$?
secs=$(( $(date +%s) - start ))
if [ "$secs" -ge "$limit" ]; then
  src=""
  for a in "$@"; do case "$a" in *.c|*.cc|*.cpp|*.cxx|*.mm|*.m) src="$a";; esac; done
  echo "SLOW-COMPILE ${secs}s ${src}" >&2
fi
exit $rc
