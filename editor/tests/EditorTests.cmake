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

# HttpTest's HTTP server on 127.0.0.1 (POSIX sockets)
if(NOT WIN32)
    list(APPEND EDITOR_TEST_SOURCES ${CMAKE_CURRENT_LIST_DIR}/util/TestHttpServer.cpp)
else()
    list(FILTER EDITOR_TEST_SOURCES EXCLUDE REGEX "/HttpTest\\.cpp$")
endif()
# VideoMp4Test: the Linux FFmpeg backend and the media cache against that server
if(WIN32 OR APPLE)
    list(FILTER EDITOR_TEST_SOURCES EXCLUDE REGEX "/VideoMp4Test\\.cpp$")
endif()

# the child ProcessExecTest runs (argument echo, environment, timeouts, a process tree, floods)
add_executable(je_vcs_process_helper ${CMAKE_CURRENT_LIST_DIR}/vcs/ProcessHelper.cpp)
set_target_properties(je_vcs_process_helper PROPERTIES
    CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    FOLDER "tests")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(je_vcs_process_helper PRIVATE Threads::Threads)
endif()

jasperengine_add_test_target(jasperengine-editor-tests
    EDITOR
    SOURCES ${EDITOR_TEST_SOURCES}
    # (engine/physics: upstream's flat physics tests include "PhysicsTestScene.h" as they did)
    INCLUDES ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/engine/tests/core ${CMAKE_CURRENT_LIST_DIR}/engine/physics
    ASSET_DIR ${CMAKE_CURRENT_LIST_DIR}/fixtures
    # tinyexr: FloatImageTest writes EXRs with custom channel names through it
    LINK tinyexr
    DEFINITIONS
        # the repository root (the data-safety corpus reads tools/fixtures)
        JASPER_TEST_REPO_DIR="${CMAKE_SOURCE_DIR}"
        JASPER_TEST_FIXTURES_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures"
        # upstream's name of the same folder
        JASPER_EDITOR_TEST_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/fixtures"
        # the video tests' clips (tools/media/make_test_clips.sh) and the HTML UI's WebM fixtures
        JASPER_MEDIA_TEST_CLIPS="${CMAKE_SOURCE_DIR}/engine/tests/media/clips"
        JASPER_HTMLUI_VIDEO_FIXTURES="${CMAKE_SOURCE_DIR}/engine/tests/htmlui/fixtures/video"
        # tests that run the editor binary (CLI exit codes)
        JASPER_TEST_EDITOR_BINARY="$<TARGET_FILE:jasperengine-editor>"
        JASPER_TEST_LOG_CRASH_HELPER="$<TARGET_FILE:je_log_crash_helper>"
        JASPER_TEST_VCS_PROCESS_HELPER="$<TARGET_FILE:je_vcs_process_helper>"
        # whether this build compiles V-HACD (engine/CMakeLists.txt): the test of the build without
        # it is registered only there, not skipped here (S02 R11, no skipped editor test)
        JASPER_TEST_CONVEX_DECOMPOSITION=$<BOOL:${JASPERENGINE_CONVEX_DECOMPOSITION}>
        # the engine's include directories and definitions, and the compiler (the flags file is
        # generated below): LineRendererStreamTest compiles Factory's component code
        JASPER_EDITOR_TEST_ENGINE_FLAGS="${CMAKE_BINARY_DIR}/editor-tests-engine-flags.txt"
        JASPER_EDITOR_TEST_CXX="${CMAKE_CXX_COMPILER}"
)
add_dependencies(jasperengine-editor-tests jasperengine-editor je_log_crash_helper je_vcs_process_helper)

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

# The engine's include directories and definitions, for tests that compile the C++ the editor
# generates for exports (LineRendererStreamTest: Factory's component code against the engine
# headers, with the compiler that builds the tests; as a game's code sees them, every component
# through Scene.h, without the engine's own JASPERENGINE_EXPLICIT_COMPONENT_INCLUDES)
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/editor-tests-engine-flags.txt CONTENT
"$<JOIN:$<TARGET_PROPERTY:jasperengine,INCLUDE_DIRECTORIES>,\n>\n#defines\n$<JOIN:$<FILTER:$<TARGET_PROPERTY:jasperengine,COMPILE_DEFINITIONS>,EXCLUDE,^JASPERENGINE_EXPLICIT_COMPONENT_INCLUDES$>,\n>\n")
