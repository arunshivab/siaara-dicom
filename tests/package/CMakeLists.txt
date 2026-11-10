# Consumer project for the installed package: configure with
#   cmake -S tests/package -B build-consumer -DCMAKE_PREFIX_PATH=<install prefix>
#         [-DSIAARA_EXPECT_VERSION=x.y.z]
# It builds one program against the shared and one against the static library. Both check the
# ABI version, print the toolkit version and write, read back and check a small DICOM file (run
# by the release workflow, docs/RELEASING.md). No version is written here: with
# SIAARA_EXPECT_VERSION the package version and siaara_version_string() must both equal it.
cmake_minimum_required(VERSION 3.20)
project(siaara_consumer LANGUAGES C CXX)
find_package(siaara REQUIRED CONFIG)
message(STATUS "siaara package version: ${siaara_VERSION}")
set(SIAARA_EXPECT_VERSION "" CACHE STRING "Version the installed package must have (empty: not checked)")
if(SIAARA_EXPECT_VERSION AND NOT siaara_VERSION VERSION_EQUAL SIAARA_EXPECT_VERSION)
  message(FATAL_ERROR "The installed package is ${siaara_VERSION}, expected ${SIAARA_EXPECT_VERSION}")
endif()
add_executable(consumer_shared main.c)
target_link_libraries(consumer_shared PRIVATE siaara::siaara)
add_executable(consumer_static main.c)
target_link_libraries(consumer_static PRIVATE siaara::siaara_static)
if(SIAARA_EXPECT_VERSION)
  target_compile_definitions(consumer_shared PRIVATE SIAARA_EXPECT_VERSION="${SIAARA_EXPECT_VERSION}")
  target_compile_definitions(consumer_static PRIVATE SIAARA_EXPECT_VERSION="${SIAARA_EXPECT_VERSION}")
endif()
