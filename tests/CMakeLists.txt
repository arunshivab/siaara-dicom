# Test-only code may use the plain C stdio API; the library itself never does.
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

file(GLOB SIAARA_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_executable(siaara_tests ${SIAARA_TEST_SOURCES})
target_link_libraries(siaara_tests PRIVATE siaara_static)
target_compile_options(siaara_tests PRIVATE ${SIAARA_WARN})
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
target_compile_options(jpegls_tool PRIVATE ${SIAARA_WARN})
# Test-only helper for tests/crosscheck/*.py and the codec benchmarks (not installed).
add_executable(siaara_codec_tool crosscheck/codec_tool.cpp)
target_link_libraries(siaara_codec_tool PRIVATE siaara_static)
target_compile_options(siaara_codec_tool PRIVATE ${SIAARA_WARN})
add_executable(jpeg_tool crosscheck/jpeg_tool.cpp)  # test-only helper for crosscheck/jpeg_crosscheck.py
target_link_libraries(jpeg_tool PRIVATE siaara_static)
target_compile_options(jpeg_tool PRIVATE ${SIAARA_WARN})
# Test-only helper for tests/crosscheck/pixel_crosscheck.py and the render benchmark (not installed).
add_executable(pixel_tool crosscheck/pixel_tool.cpp)
target_link_libraries(pixel_tool PRIVATE siaara_static)
target_compile_options(pixel_tool PRIVATE ${SIAARA_WARN})
# Test-only helper for tests/crosscheck/charset_crosscheck.py (not installed).
add_executable(charset_tool crosscheck/charset_tool.cpp)
target_link_libraries(charset_tool PRIVATE siaara_static)
target_compile_options(charset_tool PRIVATE ${SIAARA_WARN})
# Test-only store/scan benchmark (tests/crosscheck/store_perf.cpp); not run by ctest.
add_executable(store_perf crosscheck/store_perf.cpp)
target_link_libraries(store_perf PRIVATE siaara_static)
target_compile_options(store_perf PRIVATE ${SIAARA_WARN})
# Test-only DICOMDIR helper for tests/crosscheck/dicomdir_crosscheck.py (not installed).
add_executable(dicomdir_tool crosscheck/dicomdir_tool.cpp)
target_link_libraries(dicomdir_tool PRIVATE siaara_static)
target_compile_options(dicomdir_tool PRIVATE ${SIAARA_WARN})
