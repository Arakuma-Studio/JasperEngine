# jasperengine-editor-tests: unit tests of editor logic (editor/tests/<area>/ mirrors editor/<area>/,
# fixtures in editor/tests/fixtures, shared helpers in editor/tests/support; docs/testing.md).
# Included by the root CMakeLists.txt with -DJASPERENGINE_TESTS=ON (include(), not
# add_subdirectory(): the editor's relative source and include paths must keep resolving against
# the root). The editor's sources without its main() are compiled with jasperengine-editor's
# command lines, so ccache shares their objects; the binary sits next to the editor and its shared
# engine. Runs on Linux, Windows and macOS (tools/build.sh unit-tests on Linux).

file(GLOB_RECURSE EDITOR_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/*Test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/support/*.cpp)

jasperengine_add_test_target(jasperengine-editor-tests
    EDITOR
    SOURCES ${EDITOR_TEST_SOURCES}
    INCLUDES ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/engine/tests/core
    ASSET_DIR ${CMAKE_CURRENT_LIST_DIR}/fixtures
    DEFINITIONS
        # the repository root (the data-safety corpus reads tools/fixtures)
        JASPER_TEST_REPO_DIR="${CMAKE_SOURCE_DIR}"
        JASPER_TEST_FIXTURES_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures"
        # tests that run the editor binary (CLI exit codes)
        JASPER_TEST_EDITOR_BINARY="$<TARGET_FILE:jasperengine-editor>"
)
add_dependencies(jasperengine-editor-tests jasperengine-editor)
