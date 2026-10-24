# Configures the project next to this file twice: without exceptions it must fail and name only
# legacy17; with legacy17 in JASPERENGINE_CXX_STANDARD_EXCEPTIONS it must pass.
#
#   cmake -P tools/tests/cxx-standard-check/run.cmake
cmake_minimum_required(VERSION 3.20)

set(source ${CMAKE_CURRENT_LIST_DIR})
# under build/ (git-ignored) of the working directory
set(binary ${CMAKE_CURRENT_BINARY_DIR}/build/cxx-standard-check)

file(REMOVE_RECURSE ${binary}/fail)
execute_process(
    COMMAND ${CMAKE_COMMAND} -S ${source} -B ${binary}/fail
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
)
string(REGEX REPLACE "[ \t\r\n]+" " " output "${output}")
if(result EQUAL 0)
    message(FATAL_ERROR "cxx-standard-check: configuring with a C++17 target passed: ${output}")
endif()
if(NOT output MATCHES "do not compile at C\\+\\+20: legacy17 \\(17\\)\\.")
    message(FATAL_ERROR "cxx-standard-check: the failure does not name only legacy17: ${output}")
endif()
message(STATUS "cxx-standard-check: a C++17 target fails the configure, named alone")

file(REMOVE_RECURSE ${binary}/pass)
execute_process(
    COMMAND ${CMAKE_COMMAND} -S ${source} -B ${binary}/pass -DJASPERENGINE_CXX_STANDARD_EXCEPTIONS=legacy17
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
)
string(REGEX REPLACE "[ \t\r\n]+" " " output "${output}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "cxx-standard-check: configuring with legacy17 exempted failed: ${output}")
endif()
if(NOT output MATCHES "C\\+\\+ standard check: 1 C\\+\\+ targets at C\\+\\+20")
    message(FATAL_ERROR "cxx-standard-check: no success line for the one checked target: ${output}")
endif()
message(STATUS "cxx-standard-check: passes with legacy17 in JASPERENGINE_CXX_STANDARD_EXCEPTIONS")
file(REMOVE_RECURSE ${binary})
