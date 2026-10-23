# jasperengine-core-tests: unit tests of the engine (engine/core, engine/renders) against the
# static engine, built by the standalone engine tree with -DJASPERENGINE_TESTS=ON
# (tools/build.sh unit-tests). docs/testing.md has the layout and naming rules: <area>/ mirrors
# engine/core/<area>/, files are <Subject>Test.cpp, fixtures/ and golden/ hold test data,
# support/ holds headers shared by this binary's tests.
# CPU suites build and run on Linux, Windows and macOS; *Gpu suites run on Linux (Xvfb).

file(GLOB_RECURSE CORE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*Test.cpp)

jasperengine_add_test_target(jasperengine-core-tests
    SOURCES ${CORE_TEST_SOURCES}
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
)
