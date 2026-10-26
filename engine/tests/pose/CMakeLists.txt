# jasperengine-pose-tests: unit tests of the pose animation core (engine/core/pose), built with
# -DJASPERENGINE_POSE_TESTS=ON from the editor top-level build or the standalone engine build.
# Tests are the *.cpp files next to this one; the pose sources come from the engine library
# (shared in the editor build, static in the standalone build). No GPU, no window.

# googletest from the repository's yaml-cpp test tree (not shipped with the engine); another
# test suite of the same build may have added it already
if(NOT TARGET gtest)
    set(JASPER_GTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/yaml-cpp/test/gtest-1.11.0")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)         # gtest 1.11 declares 2.8.12: CMake 4 rejects it, 3.x warns
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(${JASPER_GTEST_DIR} ${CMAKE_BINARY_DIR}/gtest EXCLUDE_FROM_ALL)
    target_compile_options(gtest PRIVATE ${EXTERNAL_WARNING_FLAG})
    target_compile_options(gtest_main PRIVATE ${EXTERNAL_WARNING_FLAG})
endif()

file(GLOB POSE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

# editor pose logic without ImGui or editor state, tested here because the editor has no
# googletest target (the editor files are compiled into this executable)
set(POSE_TEST_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(POSE_TEST_EDITOR_SOURCES
    ${POSE_TEST_REPO_ROOT}/editor/pose/PoseImage.cpp
    ${POSE_TEST_REPO_ROOT}/editor/pose/PoseUiLogic.cpp
    ${POSE_TEST_REPO_ROOT}/editor/pose/PoseViewportLogic.cpp
    ${POSE_TEST_REPO_ROOT}/editor/command/CommandHistory.cpp
    ${POSE_TEST_REPO_ROOT}/editor/command/type/WriteFilesCmd.cpp
    ${POSE_TEST_REPO_ROOT}/editor/command/type/RemoveFilesCmd.cpp
    ${POSE_TEST_REPO_ROOT}/editor/pose/PoseTemplates.cpp
)

add_executable(jasperengine-pose-tests ${POSE_TEST_SOURCES} ${POSE_TEST_EDITOR_SOURCES})

target_include_directories(jasperengine-pose-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${POSE_TEST_REPO_ROOT} ${POSE_TEST_REPO_ROOT}/editor)

find_package(Threads REQUIRED)
target_link_libraries(jasperengine-pose-tests PRIVATE jasperengine stb gtest_main gtest Threads::Threads)

if(NOT MSVC)
    target_compile_options(jasperengine-pose-tests PRIVATE -Wall -Wextra)
endif()

set_target_properties(jasperengine-pose-tests PROPERTIES
    CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# PoseTemplatesTest: the built-in template files and the fixture models' skeletons, read from the
# source tree (the editor embeds the same template files)
target_compile_definitions(jasperengine-pose-tests PRIVATE
    POSE_TEMPLATES_DIR="${POSE_TEST_REPO_ROOT}/editor/resources/pose-templates"
    POSE_FIXTURE_MODELS_DIR="${POSE_TEST_REPO_ROOT}/tools/fixtures/pose-animation/assets/models")
target_include_directories(jasperengine-pose-tests PRIVATE ${POSE_TEST_REPO_ROOT}/engine/libs/json)
