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
# Test-only TLS helper for tests/crosscheck/tls_crosscheck.py (not installed).
add_executable(tls_tool crosscheck/tls_tool.cpp)
target_link_libraries(tls_tool PRIVATE siaara_static)
target_compile_options(tls_tool PRIVATE ${SIAARA_TEST_WARN})
