# SIAARA examples (docs/GETTING_STARTED.md). Two ways to build them:
#   - as part of the toolkit build (add_subdirectory from the top-level CMakeLists.txt): they are
#     built against the build tree and run by ctest, so they always compile and work;
#   - on their own against an installed package:
#       cmake -S examples -B build-examples -DCMAKE_PREFIX_PATH=<install prefix>
#       cmake --build build-examples
cmake_minimum_required(VERSION 3.20)
if(NOT TARGET siaara)
  project(siaara_examples LANGUAGES C CXX)
  find_package(siaara REQUIRED CONFIG)
  add_library(siaara_example_lib INTERFACE)
  target_link_libraries(siaara_example_lib INTERFACE siaara::siaara)
  enable_testing()
else()
  add_library(siaara_example_lib INTERFACE)
  target_link_libraries(siaara_example_lib INTERFACE siaara)
endif()

add_executable(siaara_example_read_file c/read_file.c)
add_executable(siaara_example_network c/network.c)
add_executable(siaara_example_attachment_preview c/attachment_preview.c)
add_executable(siaara_example_read_file_cpp cpp/read_file.cpp)
foreach(t siaara_example_read_file siaara_example_network siaara_example_attachment_preview siaara_example_read_file_cpp)
  target_link_libraries(${t} PRIVATE siaara_example_lib)
endforeach()
set_target_properties(siaara_example_read_file siaara_example_network siaara_example_attachment_preview
                      PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
set_target_properties(siaara_example_read_file_cpp PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
if(MSVC)
  foreach(t siaara_example_read_file siaara_example_network siaara_example_attachment_preview siaara_example_read_file_cpp)
    target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endforeach()
endif()

set(_work ${CMAKE_CURRENT_BINARY_DIR}/work)
add_test(NAME example_c_read_file COMMAND siaara_example_read_file)
add_test(NAME example_c_network
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:siaara_example_network> -DWORK=${_work}/network
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_in_fresh_folder.cmake)
add_test(NAME example_cpp_read_file COMMAND siaara_example_read_file_cpp)
# Untrusted bytes (a mail attachment) to a caption and a PNG / JPEG: docs/GETTING_STARTED.md.
add_test(NAME example_c_attachment_preview COMMAND siaara_example_attachment_preview)
set_tests_properties(example_c_read_file example_cpp_read_file example_c_attachment_preview
                     PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
if(WIN32 AND TARGET siaara)
  string(REPLACE ";" "\;" _siaara_example_path "$ENV{PATH}")
  set_tests_properties(example_c_read_file example_c_network example_cpp_read_file example_c_attachment_preview PROPERTIES
                       ENVIRONMENT "PATH=$<TARGET_FILE_DIR:siaara>\;${_siaara_example_path}")
endif()

# The C# example (examples/csharp/QuickStart) is built from source and run by ctest when the .NET
# SDK pinned by global.json is installed; it needs the shared library of this build.
# dotnet cannot load a sanitizer build of the library, and a 64-bit dotnet cannot load a 32-bit one.
if(TARGET siaara AND NOT SIAARA_SANITIZE AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  find_program(SIAARA_DOTNET dotnet)
  if(SIAARA_DOTNET)
    execute_process(COMMAND ${SIAARA_DOTNET} --version WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
                    RESULT_VARIABLE _dotnet_result OUTPUT_QUIET ERROR_QUIET)
    if(_dotnet_result EQUAL 0)
      add_test(NAME example_csharp_quickstart
               COMMAND ${SIAARA_DOTNET} run -c Release --project ${CMAKE_CURRENT_SOURCE_DIR}/csharp/QuickStart
                       --artifacts-path ${CMAKE_CURRENT_BINARY_DIR}/csharp-artifacts
                       -p:SiaaraFromSource=true -p:SiaaraNativeLibrary=$<TARGET_FILE:siaara>
                       -- ${_work}/csharp)
      # Untrusted bytes (a mail attachment) to a caption and a PNG / JPEG, in C#. After the
      # QuickStart test: both build the same binding projects.
      add_test(NAME example_csharp_attachment_preview
               COMMAND ${SIAARA_DOTNET} run -c Release --project ${CMAKE_CURRENT_SOURCE_DIR}/csharp/AttachmentPreview
                       --artifacts-path ${CMAKE_CURRENT_BINARY_DIR}/csharp-artifacts-preview
                       -p:SiaaraFromSource=true -p:SiaaraNativeLibrary=$<TARGET_FILE:siaara>
                       -- ${_work}/csharp-preview)
      # A ZIP (with a DICOMDIR) from a mail grouped into patient / study / series, in C#.
      add_test(NAME example_csharp_study_grouping
               COMMAND ${SIAARA_DOTNET} run -c Release --project ${CMAKE_CURRENT_SOURCE_DIR}/csharp/StudyGrouping
                       --artifacts-path ${CMAKE_CURRENT_BINARY_DIR}/csharp-artifacts-grouping
                       -p:SiaaraFromSource=true -p:SiaaraNativeLibrary=$<TARGET_FILE:siaara>
                       -- ${_work}/csharp-grouping)
      set_tests_properties(example_csharp_quickstart example_csharp_attachment_preview example_csharp_study_grouping
                           PROPERTIES TIMEOUT 900 ENVIRONMENT "DOTNET_CLI_TELEMETRY_OPTOUT=1;DOTNET_NOLOGO=1")
      set_tests_properties(example_csharp_attachment_preview example_csharp_study_grouping PROPERTIES
                           DEPENDS example_csharp_quickstart RESOURCE_LOCK siaara_dotnet_build)
      set_tests_properties(example_csharp_quickstart PROPERTIES RESOURCE_LOCK siaara_dotnet_build)
    endif()
  endif()
endif()
