file(GLOB SIAARA_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_executable(siaara_tests ${SIAARA_TEST_SOURCES})
target_link_libraries(siaara_tests PRIVATE siaara_static)
target_compile_options(siaara_tests PRIVATE ${SIAARA_WARN})
add_test(NAME siaara_tests COMMAND siaara_tests --data ${CMAKE_CURRENT_SOURCE_DIR}/data)
add_executable(jpeg_tool crosscheck/jpeg_tool.cpp)  # test-only helper for crosscheck/jpeg_crosscheck.py
target_link_libraries(jpeg_tool PRIVATE siaara_static)
target_compile_options(jpeg_tool PRIVATE ${SIAARA_WARN})
