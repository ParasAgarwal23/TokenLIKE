# Runs CMD and requires an exact exit code (a crash must not count as the
# expected failure). Optionally requires a substring in stderr or stdout.
#   cmake -DEXPECT=1 -DCMD="exe;arg;..." [-DSTDERR_HAS=text] [-DSTDOUT_HAS=text] -P expect_exit.cmake
execute_process(COMMAND ${CMD} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT "${rc}" STREQUAL "${EXPECT}")
  message(FATAL_ERROR "exit code ${rc}, expected ${EXPECT}\nstdout: ${out}\nstderr: ${err}")
endif()
if(DEFINED STDERR_HAS AND NOT err MATCHES "${STDERR_HAS}")
  message(FATAL_ERROR "stderr does not contain '${STDERR_HAS}':\n${err}")
endif()
if(DEFINED STDOUT_HAS AND NOT out MATCHES "${STDOUT_HAS}")
  message(FATAL_ERROR "stdout does not contain '${STDOUT_HAS}':\n${out}")
endif()
message(STATUS "exit ${rc} as expected")
