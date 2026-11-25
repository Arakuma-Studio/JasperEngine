# The project test runner's editor side (docs/specs/project-test-runner.md): the test root, Play
# Data profiles, the editor's Play-session host, Edit Mode cases, runs, the window and run_tests.

list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/test/EditModeTests.cpp
    ${EDITOR_DIR}/test/GameTests.cpp
    ${EDITOR_DIR}/test/PlayData.cpp
    ${EDITOR_DIR}/test/TestRun.cpp
    ${EDITOR_DIR}/test/TestRunHost.cpp
    ${EDITOR_DIR}/window/TestRunnerWindow.cpp
    ${EDITOR_DIR}/mcp/tools/TestTools.cpp
)
