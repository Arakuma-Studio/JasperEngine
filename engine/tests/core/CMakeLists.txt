# jasperengine-core-tests: unit tests of the engine (engine/core, engine/renders) against the
# static engine, built by the standalone engine tree with -DJASPERENGINE_TESTS=ON
# (tools/build.sh unit-tests). docs/testing.md has the layout and naming rules: <area>/ mirrors
# engine/core/<area>/, files are <Subject>Test.cpp, fixtures/ and golden/ hold test data,
# support/ holds headers shared by this binary's tests.
# CPU suites build and run on Linux, Windows and macOS; *Gpu suites run on Linux (Xvfb).
#
# je-bench-jobs and je-bench-memory are the job system and allocator benchmarks (S06 R23-R26)
# tools/perf/check_bench.py gates on, from their JSON output (docs/jobs-memory.md).

file(GLOB_RECURSE CORE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*Test.cpp)

# The allocation counter of the zero-allocation tests and the benches. It counts at malloc,
# wrapped by GNU ld (the vendored ThorVG already replaces the plain operator new), so it counts
# on Linux; elsewhere those tests skip.
set(CORE_TEST_ALLOCATION_COUNTER ${CMAKE_CURRENT_SOURCE_DIR}/support/AllocationCounter.cpp)
set(CORE_TEST_COUNTS_ALLOCATIONS OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CORE_TEST_COUNTS_ALLOCATIONS ON)
endif()

jasperengine_add_test_target(jasperengine-core-tests
    SOURCES ${CORE_TEST_SOURCES} ${CORE_TEST_ALLOCATION_COUNTER}
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/support
    # the KTX2 tests write Zstandard supercompressed files (games link the decoder only)
    LINK zstd
)

# a unit of its own: it redefines JASPERENGINE_PROFILER to 0 before the profiling macros
jasperengine_skip_unity_build(
    REASON "ProfilerCompileOutTest compiles the profiling macros with the profiler off"
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/profiler/ProfilerCompileOutTest.cpp)

# the logger's backtraces name the test functions (LoggerTest.ErrorCarriesBacktrace): dladdr
# sees the executable's symbols
set_target_properties(jasperengine-core-tests PROPERTIES ENABLE_EXPORTS ON)

find_package(Threads REQUIRED)
add_executable(je-bench-jobs ${CMAKE_CURRENT_SOURCE_DIR}/bench/JobsBench.cpp ${CORE_TEST_ALLOCATION_COUNTER})
add_executable(je-bench-memory ${CMAKE_CURRENT_SOURCE_DIR}/bench/MemoryBench.cpp ${CORE_TEST_ALLOCATION_COUNTER})
foreach(target je-bench-jobs je-bench-memory)
    target_link_libraries(${target} PRIVATE jasperengine Threads::Threads)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    jasperengine_apply_cxx_standard(${target})
    jasperengine_target_warnings(${target})
    set_target_properties(${target} PROPERTIES FOLDER "tests")
endforeach()

foreach(target jasperengine-core-tests je-bench-jobs je-bench-memory)
    if(CORE_TEST_COUNTS_ALLOCATIONS)
        target_compile_definitions(${target} PRIVATE JE_TEST_COUNTS_ALLOCATIONS)
        target_link_options(${target} PRIVATE "-Wl,--wrap=malloc" "-Wl,--wrap=calloc" "-Wl,--wrap=realloc" "-rdynamic")
    endif()
endforeach()
