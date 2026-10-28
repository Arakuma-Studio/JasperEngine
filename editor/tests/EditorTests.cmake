# jasperengine-editor-tests: unit tests of editor logic (editor/tests/<area>/ mirrors editor/<area>/,
# fixtures in editor/tests/fixtures, shared helpers in editor/tests/support; docs/testing.md), and
# in editor/tests/engine/<area>/ (mirroring engine/core/<area>/) the tests of engine code that
# drive it through the editor's shared engine (scenes with physics, models, shader keys).
# Included by the root CMakeLists.txt with -DJASPERENGINE_TESTS=ON (include(), not
# add_subdirectory(): the editor's relative source and include paths must keep resolving against
# the root). The editor's sources without its main() are compiled with jasperengine-editor's
# command lines, so ccache shares their objects; the binary sits next to the editor and its shared
# engine. Runs on Linux, Windows and macOS (tools/build.sh unit-tests on Linux).

file(GLOB_RECURSE EDITOR_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/*Test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/support/*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/reflect/legacy/*.cpp)
# the reflection codec's benchmark against the legacy codecs (docs/specs/S08-reflection.md R17)
list(APPEND EDITOR_TEST_SOURCES ${CMAKE_SOURCE_DIR}/tools/performance/reflect_codec.cpp)

# the process LoggerCrashFlushTest crashes: the logger's queue, a file sink and the editor's
# crash handler (editor/tests/util/LogCrashHelper.cpp)
add_executable(je_log_crash_helper
    ${CMAKE_CURRENT_LIST_DIR}/util/LogCrashHelper.cpp
    ${CMAKE_SOURCE_DIR}/${EDITOR_DIR}/util/CrashHandler.cpp)
target_include_directories(je_log_crash_helper PRIVATE ${CMAKE_SOURCE_DIR}/${EDITOR_DIR} ${GENERATED_DIR})
target_link_libraries(je_log_crash_helper PRIVATE jasperengine)
if(WIN32)
    target_link_libraries(je_log_crash_helper PRIVATE dbghelp)
endif()
set_target_properties(je_log_crash_helper PROPERTIES
    CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    FOLDER "tests")

jasperengine_add_test_target(jasperengine-editor-tests
    EDITOR
    SOURCES ${EDITOR_TEST_SOURCES}
    INCLUDES ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/engine/tests/core
    ASSET_DIR ${CMAKE_CURRENT_LIST_DIR}/fixtures
    # tinyexr: FloatImageTest writes EXRs with custom channel names through it
    LINK tinyexr
    DEFINITIONS
        # the repository root (the data-safety corpus reads tools/fixtures)
        JASPER_TEST_REPO_DIR="${CMAKE_SOURCE_DIR}"
        JASPER_TEST_FIXTURES_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures"
        # tests that run the editor binary (CLI exit codes)
        JASPER_TEST_EDITOR_BINARY="$<TARGET_FILE:jasperengine-editor>"
        JASPER_TEST_LOG_CRASH_HELPER="$<TARGET_FILE:je_log_crash_helper>"
        # whether this build compiles V-HACD (engine/CMakeLists.txt): the test of the build without
        # it is registered only there, not skipped here (S02 R11, no skipped editor test)
        JASPER_TEST_CONVEX_DECOMPOSITION=$<BOOL:${JASPERENGINE_CONVEX_DECOMPOSITION}>
)
add_dependencies(jasperengine-editor-tests jasperengine-editor je_log_crash_helper)

# A process with nothing but the engine (the editor's static App builds a Scene before main, so
# the test binary can't make a PlacementWorld before Jolt is set up); run by PlacementWorldTest
add_executable(jasperengine-placement-fresh-process ${CMAKE_CURRENT_LIST_DIR}/engine/physics/standalone/PlacementWorldBeforeAnyScene.cpp)
target_link_libraries(jasperengine-placement-fresh-process PRIVATE jasperengine)
set_target_properties(jasperengine-placement-fresh-process PROPERTIES
    CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    FOLDER "tests")
add_dependencies(jasperengine-editor-tests jasperengine-placement-fresh-process)

# A process of its own for the texture synthesis' out-of-memory check (an address-space limit
# measured from the process size, which the engine-running test binary can't give); run by
# TexSynthOutOfMemory.IsAFailedStatusNotATerminate, Linux only
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(jasperengine-texsynth-oom
        ${CMAKE_CURRENT_LIST_DIR}/texsynth/standalone/SynthesisOutOfMemory.cpp
        ${JASPERENGINE_TEXSYNTH_SOURCES}
        ${CMAKE_SOURCE_DIR}/${EDITOR_DIR}/util/PngWriter.cpp)
    target_include_directories(jasperengine-texsynth-oom PRIVATE ${CMAKE_SOURCE_DIR}/${EDITOR_DIR} ${GENERATED_DIR})
    target_link_libraries(jasperengine-texsynth-oom PRIVATE jasperengine)
    if(TARGET stb)
        target_link_libraries(jasperengine-texsynth-oom PRIVATE stb)
    endif()
    set_target_properties(jasperengine-texsynth-oom PROPERTIES
        CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
        FOLDER "tests")
    add_dependencies(jasperengine-editor-tests jasperengine-texsynth-oom)
endif()
