add_executable(siaara-dump siaara_dump.cpp)
target_link_libraries(siaara-dump PRIVATE siaara_static)
target_compile_options(siaara-dump PRIVATE ${SIAARA_WARN})
add_executable(siaara-echoscu siaara_echoscu.cpp)
target_link_libraries(siaara-echoscu PRIVATE siaara_static)
target_compile_options(siaara-echoscu PRIVATE ${SIAARA_WARN})
add_executable(siaara-storescp siaara_storescp.cpp)
target_link_libraries(siaara-storescp PRIVATE siaara_static)
target_compile_options(siaara-storescp PRIVATE ${SIAARA_WARN})
add_executable(siaara-webcli siaara_webcli.cpp)
target_link_libraries(siaara-webcli PRIVATE siaara_static)
target_compile_options(siaara-webcli PRIVATE ${SIAARA_WARN})
add_executable(siaara-deid siaara_deid.cpp)
target_link_libraries(siaara-deid PRIVATE siaara_static)
target_compile_options(siaara-deid PRIVATE ${SIAARA_WARN})
add_executable(siaara-repair siaara_repair.cpp)
target_link_libraries(siaara-repair PRIVATE siaara_static)
target_compile_options(siaara-repair PRIVATE ${SIAARA_WARN})
foreach(tool findscu movescu getscu storescu)
  add_executable(siaara-${tool} siaara_${tool}.cpp)
  target_link_libraries(siaara-${tool} PRIVATE siaara_static)
  target_compile_options(siaara-${tool} PRIVATE ${SIAARA_WARN})
endforeach()
foreach(t siaara-dump siaara-echoscu siaara-storescp siaara-webcli siaara-deid
          siaara-findscu siaara-movescu siaara-getscu siaara-storescu siaara-repair)
  siaara_harden(${t})
endforeach()
install(TARGETS siaara-dump siaara-echoscu siaara-storescp siaara-webcli siaara-deid
        siaara-findscu siaara-movescu siaara-getscu siaara-storescu siaara-repair
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# F16: every tool prints its usage to stdout and exits 0 for --help.
if(SIAARA_BUILD_TESTS)
  foreach(t siaara-dump siaara-echoscu siaara-storescp siaara-webcli siaara-deid
            siaara-findscu siaara-movescu siaara-getscu siaara-storescu siaara-repair)
    add_test(NAME help_${t} COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:${t}> -DNAME=${t}
                                     -P ${CMAKE_CURRENT_SOURCE_DIR}/check_help.cmake)
  endforeach()
endif()

# docs/CONFORMANCE.md: its tables are generated from the code by tools/release/gen_conformance.py,
# which asks this program (not installed) for the facts of the built library. The ctest fails when
# the document has drifted from the code.
if(SIAARA_BUILD_TESTS)
  add_executable(siaara_conformance_dump release/conformance_dump.cpp)
  target_link_libraries(siaara_conformance_dump PRIVATE siaara_static)
  target_compile_options(siaara_conformance_dump PRIVATE ${SIAARA_WARN})
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    # The version has one source (the project() line); nothing else may carry a literal version.
    add_test(NAME version_single_source
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/release/version.py --check)
    add_test(NAME conformance_statement_up_to_date
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/release/gen_conformance.py
                     --dump $<TARGET_FILE:siaara_conformance_dump> --check)
  endif()
endif()
