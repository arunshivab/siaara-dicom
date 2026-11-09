cmake_minimum_required(VERSION 3.20)
project(siaara VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

option(SIAARA_BUILD_TESTS "Build tests" ON)
option(SIAARA_BUILD_TOOLS "Build command-line tools" ON)
option(SIAARA_WERROR "Treat warnings as errors" ON)
option(SIAARA_SANITIZE "Build with ASan+UBSan (GCC/Clang)" OFF)
option(SIAARA_TSAN "Build with ThreadSanitizer (GCC/Clang; not with SIAARA_SANITIZE)" OFF)
option(SIAARA_FUZZ "Build the fuzz targets in fuzz/ (libFuzzer with Clang, replay executables otherwise); implies SIAARA_SANITIZE" OFF)
option(SIAARA_REPRODUCIBLE "Strip build paths from binaries (-ffile-prefix-map, /pathmap) for reproducible builds" ON)

if(SIAARA_FUZZ AND NOT MSVC)
  set(SIAARA_SANITIZE ON)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
  check_cxx_source_compiles([[
    #include <cstddef>
    #include <cstdint>
    extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }
  ]] SIAARA_LIBFUZZER)
  unset(CMAKE_REQUIRED_FLAGS)
  if(SIAARA_LIBFUZZER)
    # Coverage instrumentation for everything; only the fuzz binaries link the libFuzzer main.
    add_compile_options(-fsanitize=fuzzer-no-link)
  endif()
endif()
if(SIAARA_SANITIZE AND SIAARA_TSAN)
  message(FATAL_ERROR "SIAARA_SANITIZE and SIAARA_TSAN cannot be combined")
endif()

# Reproducible builds: no absolute source/build paths in the binaries (debug info, __FILE__), and
# deterministic static archives. Nothing in the code uses __DATE__ / __TIME__; SOURCE_DATE_EPOCH
# is honoured by the packaging scripts (tools/release/).
if(SIAARA_REPRODUCIBLE)
  if(MSVC)
    # cl.exe /pathmap and /Brepro (deterministic object and PE timestamps); /Brepro is left out of
    # Debug, where incremental linking is on.
    add_compile_options("/pathmap:${PROJECT_SOURCE_DIR}=." "/pathmap:${PROJECT_BINARY_DIR}=build"
                        "$<$<NOT:$<CONFIG:Debug>>:/Brepro>")
    add_link_options("$<$<NOT:$<CONFIG:Debug>>:/Brepro>")
  else()
    add_compile_options("-ffile-prefix-map=${PROJECT_SOURCE_DIR}=." "-ffile-prefix-map=${PROJECT_BINARY_DIR}=build")
    if(NOT APPLE)
      # GNU ar: 'D' writes zero timestamps, uids and gids into the archive members.
      set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> qcD <TARGET> <LINK_FLAGS> <OBJECTS>")
      set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> qcD <TARGET> <LINK_FLAGS> <OBJECTS>")
      set(CMAKE_C_ARCHIVE_APPEND "<CMAKE_AR> qD <TARGET> <LINK_FLAGS> <OBJECTS>")
      set(CMAKE_CXX_ARCHIVE_APPEND "<CMAKE_AR> qD <TARGET> <LINK_FLAGS> <OBJECTS>")
      add_link_options(LINKER:--build-id=sha1)
    endif()
  endif()
endif()

if(MSVC)
  set(SIAARA_WARN /W4 /permissive- /utf-8 /wd4100)
  set(SIAARA_WARN_C /W4 /WX)
  if(SIAARA_WERROR)
    list(APPEND SIAARA_WARN /WX)
  endif()
else()
  set(SIAARA_WARN -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wno-unused-parameter)
  set(SIAARA_WARN_C -Wall -Wextra -Wpedantic -Werror)
  if(SIAARA_WERROR)
    list(APPEND SIAARA_WARN -Werror)
  endif()
  if(SIAARA_SANITIZE)
    # UBSan findings are fatal so that the test suite and the fuzzers fail on them.
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
  if(SIAARA_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
  endif()
endif()

file(GLOB_RECURSE SIAARA_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(siaara_obj OBJECT ${SIAARA_SOURCES})
target_include_directories(siaara_obj
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(siaara_obj PRIVATE ${SIAARA_WARN})
target_compile_definitions(siaara_obj PRIVATE SIAARA_BUILDING=1)

include(GNUInstallDirs)
add_library(siaara SHARED $<TARGET_OBJECTS:siaara_obj>)
target_include_directories(siaara PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Export only the C API (siaara_*); see tools/ci/abi/ for the checked baseline.
if(APPLE)
  target_link_options(siaara PRIVATE "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/capi/siaara_macos.exports")
  set_property(TARGET siaara APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/capi/siaara_macos.exports)
elseif(UNIX)
  target_link_options(siaara PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/capi/siaara.map")
  set_property(TARGET siaara APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/capi/siaara.map)
endif()
add_library(siaara_static STATIC $<TARGET_OBJECTS:siaara_obj>)
# In the build tree the static library also exposes the internal C++ headers (src/) to the tests,
# tools and fuzz targets; installed, it offers the C API only, like the shared library.
target_include_directories(siaara_static
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
          $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(siaara_static PUBLIC SIAARA_STATIC=1)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(siaara PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  target_link_libraries(siaara_static PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()
if(WIN32)
  # Winsock2 (OS library) for src/net; SChannel/SSPI and CryptoAPI (OS libraries) for TLS.
  target_link_libraries(siaara PRIVATE ws2_32 secur32 crypt32)
  target_link_libraries(siaara_static PUBLIC ws2_32 secur32 crypt32)
endif()
if(APPLE)
  # Network.framework (TLS), Security.framework (identities, trust), CoreFoundation (OS frameworks).
  target_link_libraries(siaara PRIVATE "-framework Network" "-framework Security" "-framework CoreFoundation")
  target_link_libraries(siaara_static PUBLIC "-framework Network" "-framework Security" "-framework CoreFoundation")
endif()

# ---------------------------------------------------------------- install and package config
# find_package(siaara) gives siaara::siaara (shared) and siaara::siaara_static (C API).
include(CMakePackageConfigHelpers)
set(SIAARA_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/siaara)
install(TARGETS siaara siaara_static EXPORT siaaraTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/siaara/siaara.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/siaara)
install(EXPORT siaaraTargets NAMESPACE siaara:: DESTINATION ${SIAARA_CMAKE_DIR})
configure_package_config_file(cmake/siaaraConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/siaaraConfig.cmake
                              INSTALL_DESTINATION ${SIAARA_CMAKE_DIR})
# 0.x releases: only the same minor version is compatible.
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/siaaraConfigVersion.cmake
                                 VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/siaaraConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/siaaraConfigVersion.cmake
        DESTINATION ${SIAARA_CMAKE_DIR})

if(SIAARA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(SIAARA_BUILD_TESTS OR SIAARA_FUZZ)
  add_subdirectory(fuzz)
endif()
if(SIAARA_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
