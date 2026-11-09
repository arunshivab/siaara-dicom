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
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
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

add_library(siaara SHARED $<TARGET_OBJECTS:siaara_obj>)
target_include_directories(siaara PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_library(siaara_static STATIC $<TARGET_OBJECTS:siaara_obj>)
target_include_directories(siaara_static
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(siaara_static PUBLIC SIAARA_STATIC=1)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(siaara PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  target_link_libraries(siaara_static PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

if(SIAARA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(SIAARA_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
