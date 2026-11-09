# Fuzz targets (test tooling only; nothing here is installed or linked into the library).
#
# Always (with SIAARA_BUILD_TESTS): siaara_fuzz_replay, every target in one executable, which the
# test suite runs over the committed seed corpus (fuzz/corpus/<target>/) and the regression inputs
# (tests/regress/<target>/).
# With SIAARA_FUZZ=ON, per target:
#   fuzz_<target>         libFuzzer binary (Clang with -fsanitize=fuzzer available), otherwise a
#                         replay executable;
#   fuzz_<target>_replay  replay executable (any compiler).
set(SIAARA_FUZZ_TARGETS
  part10 codec_rle codec_jpeg codec_jpegls codec_j2k codec_inflate frames_render charset
  pdu assoc dimse http multipart json xml dicom_json dicom_xml dicomdir store_journal deid
  clinical values)
set(SIAARA_FUZZ_TARGETS ${SIAARA_FUZZ_TARGETS} PARENT_SCOPE)

set(SIAARA_FUZZ_WARN ${SIAARA_WARN})
if(MSVC)
  list(APPEND SIAARA_FUZZ_WARN /wd4244 /wd4267)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

set(_fuzz_sources "")
foreach(t ${SIAARA_FUZZ_TARGETS})
  list(APPEND _fuzz_sources ${CMAKE_CURRENT_SOURCE_DIR}/targets/${t}.cpp)
endforeach()

if(SIAARA_BUILD_TESTS)
  add_executable(siaara_fuzz_replay replay_main.cpp ${_fuzz_sources})
  target_compile_definitions(siaara_fuzz_replay PRIVATE SIAARA_FUZZ_COMBINED=1)
  target_include_directories(siaara_fuzz_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(siaara_fuzz_replay PRIVATE siaara_static)
  target_compile_options(siaara_fuzz_replay PRIVATE ${SIAARA_FUZZ_WARN})
  # Seeds and regression inputs; an input slower than 10 s (sanitizer debug builds) is a failure.
  add_test(NAME fuzz_regressions
           COMMAND siaara_fuzz_replay --max-ms 10000 --all ${CMAKE_CURRENT_SOURCE_DIR}/corpus
                   ${PROJECT_SOURCE_DIR}/tests/regress)
endif()

if(SIAARA_FUZZ)
  foreach(t ${SIAARA_FUZZ_TARGETS})
    add_executable(fuzz_${t}_replay replay_main.cpp targets/${t}.cpp)
    target_include_directories(fuzz_${t}_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(fuzz_${t}_replay PRIVATE siaara_static)
    target_compile_options(fuzz_${t}_replay PRIVATE ${SIAARA_FUZZ_WARN})
    if(SIAARA_LIBFUZZER)
      add_executable(fuzz_${t} targets/${t}.cpp)
      target_compile_options(fuzz_${t} PRIVATE -fsanitize=fuzzer)
      target_link_options(fuzz_${t} PRIVATE -fsanitize=fuzzer)
    else()
      add_executable(fuzz_${t} replay_main.cpp targets/${t}.cpp)
    endif()
    target_include_directories(fuzz_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(fuzz_${t} PRIVATE siaara_static)
    target_compile_options(fuzz_${t} PRIVATE ${SIAARA_FUZZ_WARN})
  endforeach()
endif()

# Seed generator (fuzz/make_seeds.py runs it): small seeds made with the toolkit's own writers.
add_executable(siaara_make_seeds make_seeds.cpp)
target_link_libraries(siaara_make_seeds PRIVATE siaara_static)
target_compile_options(siaara_make_seeds PRIVATE ${SIAARA_FUZZ_WARN})
