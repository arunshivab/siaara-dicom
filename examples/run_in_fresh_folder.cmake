# Runs EXE with one argument, WORK, after making WORK an empty folder (ctest helper).
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${EXE}" "${WORK}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "${EXE} failed: ${result}")
endif()
