# jasperengine-core-tests: unit tests of engine core against the static engine, built by the
# standalone engine build with -DJASPERENGINE_TESTS=ON (tools/build.sh unit-tests). Tests are
# <Subject>Test.cpp files under engine/tests/core/<area>/, mirroring engine/core/<area>/.
# je-bench-jobs and je-bench-memory are the job system and allocator benchmarks
# tools/perf/check_bench.py gates on (their JSON output; S06 R23-R26).
if(NOT TARGET gtest)
    # googletest from the repository's yaml-cpp test tree (as engine/tests/htmlui)
    set(JASPER_GTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/yaml-cpp/test/gtest-1.11.0")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(${JASPER_GTEST_DIR} ${CMAKE_BINARY_DIR}/gtest EXCLUDE_FROM_ALL)
    target_compile_options(gtest PRIVATE ${EXTERNAL_WARNING_FLAG})
    target_compile_options(gtest_main PRIVATE ${EXTERNAL_WARNING_FLAG})
    jasperengine_apply_cxx_standard(gtest gtest_main)
endif()

find_package(Threads REQUIRED)

file(GLOB_RECURSE CORE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*Test.cpp)

# The allocation counter (the zero-allocation tests and benches) counts at malloc, wrapped by
# the linker: the vendored ThorVG already replaces the plain operator new. It replaces the
# aligned new.
set(CORE_TEST_ALLOCATION_COUNTER ${CMAKE_CURRENT_SOURCE_DIR}/support/AllocationCounter.cpp)
set(CORE_TEST_WRAP_MALLOC "-Wl,--wrap=malloc" "-Wl,--wrap=calloc" "-Wl,--wrap=realloc" "-rdynamic")

# support/TestMain.cpp is the main: it starts the engine without a window (the process job
# system, the main thread) before the tests run
add_executable(jasperengine-core-tests
    ${CORE_TEST_SOURCES}
    ${CORE_TEST_ALLOCATION_COUNTER}
    ${CMAKE_CURRENT_SOURCE_DIR}/support/TestMain.cpp
)
target_link_libraries(jasperengine-core-tests PRIVATE jasperengine gtest Threads::Threads)
target_compile_definitions(jasperengine-core-tests PRIVATE
    JASPER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    JASPER_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

add_executable(je-bench-jobs ${CMAKE_CURRENT_SOURCE_DIR}/bench/JobsBench.cpp ${CORE_TEST_ALLOCATION_COUNTER})
add_executable(je-bench-memory ${CMAKE_CURRENT_SOURCE_DIR}/bench/MemoryBench.cpp ${CORE_TEST_ALLOCATION_COUNTER})
foreach(target je-bench-jobs je-bench-memory)
    target_link_libraries(${target} PRIVATE jasperengine Threads::Threads)
endforeach()

foreach(target jasperengine-core-tests je-bench-jobs je-bench-memory)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_link_options(${target} PRIVATE ${CORE_TEST_WRAP_MALLOC})
    jasperengine_apply_cxx_standard(${target})
endforeach()
