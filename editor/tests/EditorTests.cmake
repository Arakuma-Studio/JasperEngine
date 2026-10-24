# jasperengine-editor-tests: googletest unit tests of the editor and the engine core it drives
# (shader variant keys: ShaderPool, the .sdat format, the scene/project encoding of keys).
# Included by the root CMakeLists.txt with -DJASPERENGINE_EDITOR_TESTS=ON (include(), not
# add_subdirectory(): the editor's relative source and include paths must keep resolving
# against the root), built next to the editor and run by tools/build.sh editor-tests.
# Needs no display, network or GPU. Tests are *Test.cpp files next to this one.

# googletest from the repository's yaml-cpp test tree (as engine/tests/htmlui); the HTML UI
# editor tests (editor/htmlui/tests) add the same targets when both are on
if(NOT TARGET gtest)
    set(JASPER_EDITOR_GTEST_DIR "${CMAKE_SOURCE_DIR}/libs/yaml-cpp/test/gtest-1.11.0")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(${JASPER_EDITOR_GTEST_DIR} ${CMAKE_BINARY_DIR}/gtest-editor EXCLUDE_FROM_ALL)
    target_compile_options(gtest PRIVATE -w)
    target_compile_options(gtest_main PRIVATE -w)
endif()

# The editor's sources without its main(); the same command lines as jasperengine-editor's,
# so ccache shares the objects
set(EDITOR_TEST_EDITOR_SOURCES ${JASPERENGINE_EDITOR_SOURCES})
list(REMOVE_ITEM EDITOR_TEST_EDITOR_SOURCES ${EDITOR_DIR}/main.cpp)

# The tests themselves see googletest; the editor sources keep the editor's command lines
file(GLOB EDITOR_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*Test.cpp)
add_library(jasperengine-editor-tests-objects OBJECT ${EDITOR_TEST_SOURCES})
target_link_libraries(jasperengine-editor-tests-objects PRIVATE gtest jasperengine yaml-cpp)
target_include_directories(jasperengine-editor-tests-objects PRIVATE ${CMAKE_SOURCE_DIR}/${EDITOR_DIR} ${GENERATED_DIR})
target_include_directories(jasperengine-editor-tests-objects SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/${IMGUI_DIR} ${CMAKE_SOURCE_DIR}/${IMGUI_DIR}/backends)
set_target_properties(jasperengine-editor-tests-objects PROPERTIES
    CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
)
add_dependencies(jasperengine-editor-tests-objects generate_api_suggestions)

add_executable(jasperengine-editor-tests
    ${EDITOR_TEST_EDITOR_SOURCES}
    $<TARGET_OBJECTS:jasperengine-editor-tests-objects>
)
configure_jasperengine_editor_target(jasperengine-editor-tests)
# googletest by file, not by target: its include directories would change the editor sources' command lines
add_dependencies(jasperengine-editor-tests gtest gtest_main)
target_link_libraries(jasperengine-editor-tests PRIVATE $<TARGET_FILE:gtest_main> $<TARGET_FILE:gtest>)
# next to the editor and libjasperengine.so ($ORIGIN rpath)
set_target_properties(jasperengine-editor-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# A process with nothing but the engine (the editor's static App builds a Scene before main, so
# the test binary can't make a PlacementWorld before Jolt is set up); run by PlacementWorldTest
add_executable(jasperengine-placement-fresh-process ${CMAKE_CURRENT_LIST_DIR}/standalone/PlacementWorldBeforeAnyScene.cpp)
target_link_libraries(jasperengine-placement-fresh-process PRIVATE jasperengine)
set_target_properties(jasperengine-placement-fresh-process PROPERTIES
    CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_dependencies(jasperengine-editor-tests jasperengine-placement-fresh-process)
