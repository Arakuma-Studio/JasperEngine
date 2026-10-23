# jasperengine-jobs-tests: unit tests of the job system (jobs/), the allocators (memory/), the
# asset I/O service (io/) and the thread policy (thread/), built only by the standalone engine
# build with -DJASPERENGINE_JOBS_TESTS=ON. je-bench-jobs and je-bench-memory are the
# benchmarks tools/perf/check_bench.py gates on (their JSON output).
#
# S02's jasperengine-core-tests takes these sources over when it lands; the layout already
# follows it (engine/tests/core/<area>/<Subject>Test.cpp).

if(NOT TARGET gtest)
    set(JASPER_GTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/yaml-cpp/test/gtest-1.11.0")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(${JASPER_GTEST_DIR} ${CMAKE_BINARY_DIR}/gtest EXCLUDE_FROM_ALL)
    target_compile_options(gtest PRIVATE -w)
endif()

file(GLOB JOBS_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/jobs/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/memory/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/io/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/thread/*.cpp
)

# the allocation counter is compiled into each executable; it counts at malloc (wrapped by
# the linker: ThorVG already replaces the plain operator new) and replaces the aligned new
set(JOBS_TEST_SUPPORT ${CMAKE_CURRENT_SOURCE_DIR}/support/AllocationCounter.cpp)
set(JOBS_TEST_WRAP_MALLOC "-Wl,--wrap=malloc" "-Wl,--wrap=calloc" "-Wl,--wrap=realloc" "-rdynamic")

add_executable(jasperengine-jobs-tests
    ${JOBS_TEST_SOURCES}
    ${JOBS_TEST_SUPPORT}
    ${CMAKE_CURRENT_SOURCE_DIR}/support/TestMain.cpp
)
target_link_libraries(jasperengine-jobs-tests PRIVATE jasperengine gtest Threads::Threads)
target_compile_definitions(jasperengine-jobs-tests PRIVATE
    JASPER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    JASPER_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

add_executable(je-bench-jobs ${CMAKE_CURRENT_SOURCE_DIR}/bench/JobsBench.cpp ${JOBS_TEST_SUPPORT})
target_link_libraries(je-bench-jobs PRIVATE jasperengine Threads::Threads)

add_executable(je-bench-memory ${CMAKE_CURRENT_SOURCE_DIR}/bench/MemoryBench.cpp ${JOBS_TEST_SUPPORT})
target_link_libraries(je-bench-memory PRIVATE jasperengine Threads::Threads)

foreach(target jasperengine-jobs-tests je-bench-jobs je-bench-memory)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_link_options(${target} PRIVATE ${JOBS_TEST_WRAP_MALLOC})
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED ON
    )
endforeach()
