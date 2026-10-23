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

# googletest from the repository's yaml-cpp test tree (as engine/tests/htmlui)
set(JASPER_EDITOR_GTEST_DIR "${CMAKE_SOURCE_DIR}/libs/yaml-cpp/test/gtest-1.11.0")
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(${JASPER_EDITOR_GTEST_DIR} ${CMAKE_BINARY_DIR}/gtest-editor EXCLUDE_FROM_ALL)
target_compile_options(gtest PRIVATE -w)
target_compile_options(gtest_main PRIVATE -w)
jasperengine_apply_cxx_standard(gtest gtest_main)

# The editor's sources (the importers reach CommandRunner, Out, Project, ...), without its main();
# the same command lines as jasperengine-editor's, so ccache shares the objects
set(HTMLUI_EDITOR_TEST_EDITOR_SOURCES ${JASPERENGINE_EDITOR_SOURCES})
list(REMOVE_ITEM HTMLUI_EDITOR_TEST_EDITOR_SOURCES ${EDITOR_DIR}/main.cpp)

# The tests themselves see googletest; the editor sources keep the editor's command lines
file(GLOB HTMLUI_EDITOR_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*.cpp)
add_library(jasperengine-htmlui-editor-tests-objects OBJECT ${HTMLUI_EDITOR_TEST_SOURCES})
target_link_libraries(jasperengine-htmlui-editor-tests-objects PRIVATE gtest jasperengine yaml-cpp)
target_include_directories(jasperengine-htmlui-editor-tests-objects PRIVATE ${CMAKE_SOURCE_DIR}/${EDITOR_DIR} ${GENERATED_DIR})
target_include_directories(jasperengine-htmlui-editor-tests-objects SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/${IMGUI_DIR} ${CMAKE_SOURCE_DIR}/${IMGUI_DIR}/backends)
target_compile_definitions(jasperengine-htmlui-editor-tests-objects PRIVATE
    JASPER_EDITOR_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    JASPER_EDITOR_TEST_FIXTURES="${CMAKE_SOURCE_DIR}/engine/tests/htmlui/fixtures/compat/net"
    JASPER_EDITOR_TEST_PYTHON="${Python3_EXECUTABLE}"
    JASPER_EDITOR_TEST_EDITOR_BINARY="$<TARGET_FILE:jasperengine-editor>"
)
set_target_properties(jasperengine-htmlui-editor-tests-objects PROPERTIES
    CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
)
add_dependencies(jasperengine-htmlui-editor-tests-objects generate_api_suggestions)

add_executable(jasperengine-htmlui-editor-tests
    ${HTMLUI_EDITOR_TEST_EDITOR_SOURCES}
    $<TARGET_OBJECTS:jasperengine-htmlui-editor-tests-objects>
)
configure_jasperengine_editor_target(jasperengine-htmlui-editor-tests)
# googletest by file, not by target: its include directories would change the editor sources' command lines
add_dependencies(jasperengine-htmlui-editor-tests gtest gtest_main)
target_link_libraries(jasperengine-htmlui-editor-tests PRIVATE $<TARGET_FILE:gtest_main> $<TARGET_FILE:gtest>)
# the CLI tests run the editor binary
add_dependencies(jasperengine-htmlui-editor-tests jasperengine-editor)
# next to the editor and libjasperengine.so ($ORIGIN rpath)
set_target_properties(jasperengine-htmlui-editor-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
