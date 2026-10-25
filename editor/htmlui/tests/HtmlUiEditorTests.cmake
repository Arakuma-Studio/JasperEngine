# jasperengine-htmlui-editor-tests (phase-2 §5.4): the HTML UI importers of editor/htmlui against
# tools/htmlui/fixture_server.py (a local HTTP server: the tests need no internet), plus one
# optional live test (HTMLUI_LIVE_NETWORK=1). Included by the root CMakeLists.txt with
# -DJASPERENGINE_HTMLUI_EDITOR_TESTS=ON (include(), not add_subdirectory(): the editor's
# relative source and include paths must keep resolving against the root), built next to the
# editor and run headless by tools/build.sh htmlui-editor-tests.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "jasperengine-htmlui-editor-tests runs on Linux only (Xvfb, the fixture server, curl)")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The editor's sources (the importers reach CommandRunner, Out, Project, ...) without its main(),
# with gtest_main: these tests start no engine
file(GLOB HTMLUI_EDITOR_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*.cpp)
jasperengine_add_test_target(jasperengine-htmlui-editor-tests
    EDITOR
    GTEST_MAIN
    SOURCES ${HTMLUI_EDITOR_TEST_SOURCES}
    DEFINITIONS
        JASPER_EDITOR_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        JASPER_EDITOR_TEST_FIXTURES="${CMAKE_SOURCE_DIR}/engine/tests/htmlui/fixtures/compat/net"
        JASPER_EDITOR_TEST_PYTHON="${Python3_EXECUTABLE}"
        JASPER_EDITOR_TEST_EDITOR_BINARY="$<TARGET_FILE:jasperengine-editor>"
    LABELS htmlui
)
# the CLI tests run the editor binary
add_dependencies(jasperengine-htmlui-editor-tests jasperengine-editor)
