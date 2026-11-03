# Tests jasperengine_check_cxx_toolchain() (engine/cmake/JasperEngineToolchain.cmake).
#
#   cmake -DJE_TEST_ID=GNU -DJE_TEST_VERSION=10.5 -P tools/tests/check_toolchain_floor.cmake
#       checks that one compiler: fails below its floor with the configure-time message
#   cmake -P tools/tests/check_toolchain_floor.cmake
#       runs every case below and fails if one passes or fails unexpectedly
cmake_minimum_required(VERSION 3.20)

include(${CMAKE_CURRENT_LIST_DIR}/../../engine/cmake/JasperEngineToolchain.cmake)

if(DEFINED JE_TEST_ID)
    jasperengine_check_cxx_toolchain(ID ${JE_TEST_ID} VERSION ${JE_TEST_VERSION})
    message(STATUS "${JE_TEST_ID} ${JE_TEST_VERSION}: supported")
    return()
endif()

# <id> <version> <PASS|FAIL>
set(cases
    "GNU 10.5 FAIL"
    "GNU 11.4 PASS"
    "GNU 13.2.0 PASS"
    "Clang 15.0 FAIL"
    "Clang 16.0 PASS"
    "AppleClang 14.0.3 FAIL"
    "AppleClang 15.0 PASS"
    "MSVC 19.29 FAIL"
    "MSVC 19.29.30133 FAIL"
    "MSVC 19.30 PASS"
    "MSVC 19.44.35207 PASS"
)
set(failures 0)
foreach(case IN LISTS cases)
    separate_arguments(fields UNIX_COMMAND "${case}")
    list(GET fields 0 id)
    list(GET fields 1 version)
    list(GET fields 2 expected)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -DJE_TEST_ID=${id} -DJE_TEST_VERSION=${version} -P ${CMAKE_CURRENT_LIST_FILE}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output
    )
    string(REGEX REPLACE "[ \t\r\n]+" " " output "${output}")
    if(result EQUAL 0)
        set(actual PASS)
    else()
        set(actual FAIL)
    endif()
    if(NOT actual STREQUAL expected)
        message(SEND_ERROR "${id} ${version}: expected ${expected}, got ${actual}: ${output}")
        math(EXPR failures "${failures} + 1")
    elseif(actual STREQUAL "FAIL" AND NOT output MATCHES
            "the ${id} compiler ${version} is older than the supported minimum ${id} [0-9.]+\\. Update it, or choose another compiler in Preferences > External Tools")
        message(SEND_ERROR "${id} ${version}: failed without the floor message: ${output}")
        math(EXPR failures "${failures} + 1")
    else()
        message(STATUS "${id} ${version}: ${actual} as expected")
    endif()
endforeach()
if(failures GREATER 0)
    message(FATAL_ERROR "check_toolchain_floor: ${failures} case(s) failed")
endif()
message(STATUS "check_toolchain_floor: all cases passed")
