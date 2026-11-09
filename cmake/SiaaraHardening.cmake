# siaara_hardening: compiler and linker hardening for the library and the tools (review R-014).
# An INTERFACE target linked PRIVATE by siaara_obj, siaara and the tools; every flag is probed
# first, so a toolchain without one simply does not get it (no -Werror on unknown flags).
#
#   GCC/Clang:  -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 (optimised
#               builds; 2 on Apple) -fcf-protection=full (x86) -mbranch-protection=standard
#               (ARM64); Linux linker: -z relro -z now -z noexecstack
#   MSVC:       /guard:cf /Qspectre /sdl; linker /DYNAMICBASE /NXCOMPAT /GUARD:CF, and on x64
#               /HIGHENTROPYVA /CETCOMPAT /guard:ehcont (ARM64: /HIGHENTROPYVA)
#   Debug:      _GLIBCXX_ASSERTIONS (libstdc++), _LIBCPP_HARDENING_MODE=EXTENSIVE (libc++), all targets
# tools/ci/check_binaries.py hardening verifies the result on the built binaries.
include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)

add_library(siaara_hardening INTERFACE)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _siaara_cpu)
if(CMAKE_OSX_ARCHITECTURES)
  set(_siaara_cpu "${CMAKE_OSX_ARCHITECTURES}")
endif()
set(_siaara_x86 OFF)
set(_siaara_arm64 OFF)
if(_siaara_cpu MATCHES "x86_64|amd64|i.86|x86")
  set(_siaara_x86 ON)
endif()
if(_siaara_cpu MATCHES "aarch64|arm64")
  set(_siaara_arm64 ON)
endif()
if(MSVC)
  # The Visual Studio generator's platform (-A) decides the target, not the host.
  if(CMAKE_GENERATOR_PLATFORM MATCHES "^[Ww]in32$")
    set(_siaara_msvc_arch x86)
  elseif(CMAKE_GENERATOR_PLATFORM MATCHES "^ARM64$" OR CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "ARM64")
    set(_siaara_msvc_arch arm64)
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(_siaara_msvc_arch x86)
  else()
    set(_siaara_msvc_arch x64)
  endif()
endif()

set(_siaara_optimised "$<NOT:$<CONFIG:Debug>>")

if(MSVC)
  target_compile_options(siaara_hardening INTERFACE /guard:cf /Qspectre /sdl)
  target_link_options(siaara_hardening INTERFACE /DYNAMICBASE /NXCOMPAT /GUARD:CF)
  if(_siaara_msvc_arch STREQUAL "x64")
    target_compile_options(siaara_hardening INTERFACE /guard:ehcont)
    target_link_options(siaara_hardening INTERFACE /HIGHENTROPYVA /CETCOMPAT /guard:ehcont)
  elseif(_siaara_msvc_arch STREQUAL "arm64")
    target_link_options(siaara_hardening INTERFACE /HIGHENTROPYVA)
  endif()
else()
  foreach(flag -fstack-protector-strong -fstack-clash-protection)
    string(MAKE_C_IDENTIFIER "SIAARA_HAS${flag}" var)
    check_cxx_compiler_flag(${flag} ${var})
    if(${var})
      target_compile_options(siaara_hardening INTERFACE ${flag})
    endif()
  endforeach()
  if(_siaara_x86 AND NOT _siaara_arm64)
    check_cxx_compiler_flag(-fcf-protection=full SIAARA_HAS_CF_PROTECTION)
    if(SIAARA_HAS_CF_PROTECTION)
      target_compile_options(siaara_hardening INTERFACE -fcf-protection=full)
    endif()
  endif()
  if(_siaara_arm64 AND NOT _siaara_x86)
    check_cxx_compiler_flag(-mbranch-protection=standard SIAARA_HAS_BRANCH_PROTECTION)
    if(SIAARA_HAS_BRANCH_PROTECTION)
      target_compile_options(siaara_hardening INTERFACE -mbranch-protection=standard)
    endif()
  endif()
  # _FORTIFY_SOURCE needs optimisation; distributions often predefine it, so undefine first.
  # It is left out of sanitizer builds (ASan intercepts the same functions).
  if(NOT SIAARA_SANITIZE AND NOT SIAARA_TSAN)
    if(APPLE)
      set(_siaara_fortify 2)
    else()
      set(_siaara_fortify 3)
    endif()
    target_compile_options(siaara_hardening INTERFACE
      "$<${_siaara_optimised}:-U_FORTIFY_SOURCE>" "$<${_siaara_optimised}:-D_FORTIFY_SOURCE=${_siaara_fortify}>")
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    foreach(lflag "LINKER:-z,relro" "LINKER:-z,now" "LINKER:-z,noexecstack")
      string(MAKE_C_IDENTIFIER "SIAARA_HAS${lflag}" var)
      check_linker_flag(CXX "${lflag}" ${var})
      if(${var})
        target_link_options(siaara_hardening INTERFACE "${lflag}")
      endif()
    endforeach()
  endif()
endif()

# Standard library assertions (bounds checks in operator[], front(), ...) in Debug builds, for
# every target (library, tests, tools, fuzzers) so all translation units agree.
add_compile_definitions(
  "$<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>"
  "$<$<CONFIG:Debug>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE>")
