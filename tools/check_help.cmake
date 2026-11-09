# Test helper (F16): `<tool> --help` must exit 0 and print "usage: <tool>" on stdout.
execute_process(COMMAND ${EXE} --help RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "${NAME} --help exited with ${rc}")
endif()
string(FIND "${out}" "usage: ${NAME}" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "${NAME} --help printed no usage on stdout: ${out}${err}")
endif()
