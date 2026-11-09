# Test-only code may use the plain C stdio API; the library itself never does.
# Integer-narrowing warnings inside std templates (C4244/C4267) are also relaxed for test code only;
# the library itself keeps them as errors.
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()
set(SIAARA_TEST_WARN ${SIAARA_WARN})
if(MSVC)
  list(APPEND SIAARA_TEST_WARN /wd4244 /wd4267)
endif()

file(GLOB SIAARA_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_executable(siaara_tests ${SIAARA_TEST_SOURCES})
target_link_libraries(siaara_tests PRIVATE siaara_static)
target_compile_options(siaara_tests PRIVATE ${SIAARA_TEST_WARN})
add_test(NAME siaara_tests COMMAND siaara_tests --data ${CMAKE_CURRENT_SOURCE_DIR}/data)

# C API smoke test: plain C11 against the shared library.
add_executable(siaara_capi_smoke capi_smoke.c)
set_target_properties(siaara_capi_smoke PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_link_libraries(siaara_capi_smoke PRIVATE siaara)
target_compile_options(siaara_capi_smoke PRIVATE ${SIAARA_WARN_C})
add_test(NAME siaara_capi_smoke COMMAND siaara_capi_smoke ${CMAKE_CURRENT_BINARY_DIR}/capi_smoke.dcm)
if(WIN32)
  set_tests_properties(siaara_capi_smoke PROPERTIES ENVIRONMENT "PATH=$<TARGET_FILE_DIR:siaara>;$ENV{PATH}")
endif()
# Test-only JPEG-LS conversion tool for tests/crosscheck/jpegls_crosscheck.py (not installed).
add_executable(jpegls_tool crosscheck/jpegls_tool.cpp)
target_link_libraries(jpegls_tool PRIVATE siaara_static)
target_compile_options(jpegls_tool PRIVATE ${SIAARA_TEST_WARN})
# Test-only helper for tests/crosscheck/*.py and the codec benchmarks (not installed).
add_executable(siaara_codec_tool crosscheck/codec_tool.cpp)
target_link_libraries(siaara_codec_tool PRIVATE siaara_static)
target_compile_options(siaara_codec_tool PRIVATE ${SIAARA_TEST_WARN})
add_executable(jpeg_tool crosscheck/jpeg_tool.cpp)  # test-only helper for crosscheck/jpeg_crosscheck.py
target_link_libraries(jpeg_tool PRIVATE siaara_static)
target_compile_options(jpeg_tool PRIVATE ${SIAARA_TEST_WARN})
# Test-only helper for tests/crosscheck/pixel_crosscheck.py and the render benchmark (not installed).
add_executable(pixel_tool crosscheck/pixel_tool.cpp)
target_link_libraries(pixel_tool PRIVATE siaara_static)
target_compile_options(pixel_tool PRIVATE ${SIAARA_TEST_WARN})
# Test-only helper for tests/crosscheck/charset_crosscheck.py (not installed).
add_executable(charset_tool crosscheck/charset_tool.cpp)
target_link_libraries(charset_tool PRIVATE siaara_static)
target_compile_options(charset_tool PRIVATE ${SIAARA_TEST_WARN})
target_compile_options(siaara_codec_tool PRIVATE ${SIAARA_WARN})
# Test-only JPEG 2000 tool for tests/crosscheck/j2k_crosscheck.py (not installed).
add_executable(j2k_tool crosscheck/j2k_tool.cpp)
target_link_libraries(j2k_tool PRIVATE siaara_static)
target_compile_options(j2k_tool PRIVATE ${SIAARA_WARN})
target_compile_options(pixel_tool PRIVATE ${SIAARA_WARN})
# Test-only helper for tests/crosscheck/web_crosscheck.py (not installed).
add_executable(web_tool crosscheck/web_tool.cpp)
target_link_libraries(web_tool PRIVATE siaara_static)
target_compile_options(web_tool PRIVATE ${SIAARA_WARN})
target_compile_options(charset_tool PRIVATE ${SIAARA_WARN})
# Test-only store/scan benchmark (tests/crosscheck/store_perf.cpp); not run by ctest.
add_executable(store_perf crosscheck/store_perf.cpp)
target_link_libraries(store_perf PRIVATE siaara_static)
target_compile_options(store_perf PRIVATE ${SIAARA_WARN})
# Test-only DICOMDIR helper for tests/crosscheck/dicomdir_crosscheck.py (not installed).
add_executable(dicomdir_tool crosscheck/dicomdir_tool.cpp)
target_link_libraries(dicomdir_tool PRIVATE siaara_static)
target_compile_options(dicomdir_tool PRIVATE ${SIAARA_WARN})
# De-identification: the table hash test reads the committed generated table.
target_compile_definitions(siaara_tests PRIVATE SIAARA_DEID_TABLE_PATH="${PROJECT_SOURCE_DIR}/src/deid/deid_table.inc")
# Test-only helper for tests/crosscheck/deid_crosscheck.py (not installed).
add_executable(deid_tool crosscheck/deid_tool.cpp)
target_link_libraries(deid_tool PRIVATE siaara_static)
target_compile_options(deid_tool PRIVATE ${SIAARA_WARN})
# Regenerates src/deid/deid_table.inc from the standard's XML when it is available and compares.
set(SIAARA_NEMA_PART15 "" CACHE FILEPATH "PS3.15 DocBook XML (part15.xml) for the de-identification table check")
set(SIAARA_NEMA_PART16 "" CACHE FILEPATH "PS3.16 DocBook XML (part16.xml) for the de-identification table check")
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND AND EXISTS "${SIAARA_NEMA_PART15}" AND EXISTS "${SIAARA_NEMA_PART16}")
  add_test(NAME deid_table_matches_generator
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/crosscheck/deid_table_check.py
                   ${PROJECT_SOURCE_DIR} ${SIAARA_NEMA_PART15} ${SIAARA_NEMA_PART16})
endif()
