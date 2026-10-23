# jasperengine-editor-tests: unit tests of editor logic, <Subject>Test.cpp files under
# editor/tests/<area>/ mirroring editor/ (fixtures in editor/tests/fixtures/). Included by the
# root CMakeLists.txt with -DJASPERENGINE_TESTS=ON (include(), not add_subdirectory(): the
# editor's relative source and include paths keep resolving against the root); built next to
# the editor and run by tools/build.sh unit-tests.

if(NOT TARGET gtest)
    # googletest from the repository's yaml-cpp test tree (as engine/tests/htmlui)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/libs/yaml-cpp/test/gtest-1.11.0 ${CMAKE_BINARY_DIR}/gtest-editor EXCLUDE_FROM_ALL)
    target_compile_options(gtest PRIVATE ${EXTERNAL_WARNING_FLAG})
    target_compile_options(gtest_main PRIVATE ${EXTERNAL_WARNING_FLAG})
    jasperengine_apply_cxx_standard(gtest gtest_main)
endif()

# The editor's sources without its main(), with the same command lines as jasperengine-editor's
# so ccache shares the objects
set(EDITOR_TEST_EDITOR_SOURCES ${JASPERENGINE_EDITOR_SOURCES})
list(REMOVE_ITEM EDITOR_TEST_EDITOR_SOURCES ${EDITOR_DIR}/main.cpp)

file(GLOB_RECURSE EDITOR_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*Test.cpp ${CMAKE_CURRENT_LIST_DIR}/support/*.cpp)
add_library(jasperengine-editor-tests-objects OBJECT ${EDITOR_TEST_SOURCES})
target_link_libraries(jasperengine-editor-tests-objects PRIVATE gtest jasperengine yaml-cpp)
target_include_directories(jasperengine-editor-tests-objects PRIVATE
    ${CMAKE_SOURCE_DIR}/${EDITOR_DIR} ${CMAKE_CURRENT_LIST_DIR} ${GENERATED_DIR} ${CMAKE_SOURCE_DIR}/engine/tests/core)
target_include_directories(jasperengine-editor-tests-objects SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/${IMGUI_DIR} ${CMAKE_SOURCE_DIR}/${IMGUI_DIR}/backends)
target_compile_definitions(jasperengine-editor-tests-objects PRIVATE
    JASPER_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    JASPER_TEST_FIXTURES_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures"
    JASPER_TEST_BINARY_DIR="${CMAKE_BINARY_DIR}"
    JASPER_TEST_EDITOR_BINARY="$<TARGET_FILE:jasperengine-editor>"
    # the editor headers the tests include (Project.h -> util/CxxStandards.h) read these, as
    # configure_jasperengine_editor_target gives them to the editor
    "JASPERENGINE_CXX_STANDARD_LIST=${JASPERENGINE_CXX_STANDARD_LIST}"
    "JASPERENGINE_CXX_FLOOR_GNU=\"${JASPERENGINE_CXX_FLOOR_GNU}\""
    "JASPERENGINE_CXX_FLOOR_CLANG=\"${JASPERENGINE_CXX_FLOOR_Clang}\""
    "JASPERENGINE_CXX_FLOOR_APPLECLANG=\"${JASPERENGINE_CXX_FLOOR_AppleClang}\""
    "JASPERENGINE_CXX_FLOOR_MSVC=\"${JASPERENGINE_CXX_FLOOR_MSVC}\""
)
jasperengine_apply_cxx_standard(jasperengine-editor-tests-objects)
add_dependencies(jasperengine-editor-tests-objects generate_api_suggestions)

add_executable(jasperengine-editor-tests
    ${EDITOR_TEST_EDITOR_SOURCES}
    $<TARGET_OBJECTS:jasperengine-editor-tests-objects>
)
configure_jasperengine_editor_target(jasperengine-editor-tests)
jasperengine_apply_cxx_standard(jasperengine-editor-tests)
# googletest by file, not by target: its include directories would change the editor sources' command lines
add_dependencies(jasperengine-editor-tests gtest gtest_main)
target_link_libraries(jasperengine-editor-tests PRIVATE $<TARGET_FILE:gtest_main> $<TARGET_FILE:gtest>)
# tests that run the editor binary (CLI exit codes)
add_dependencies(jasperengine-editor-tests jasperengine-editor)
# next to the editor and libjasperengine.so ($ORIGIN rpath)
set_target_properties(jasperengine-editor-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
