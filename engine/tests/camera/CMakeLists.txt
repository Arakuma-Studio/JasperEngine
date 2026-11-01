# jasperengine-camera-tests: unit tests of camera shake and camera follow (engine/core/camera, the
# Shake Preset pool, CameraControlSystem, the view offset in RenderSystem::updateCamera), built
# with -DJASPERENGINE_CAMERA_TESTS=ON from the editor top-level build or the standalone engine
# build. Tests are the *.cpp files next to this one; the engine sources come from the engine
# library (shared in the editor build, static in the standalone build). No GPU, no window.

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

file(GLOB CAMERA_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

set(CAMERA_TEST_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

add_executable(jasperengine-camera-tests ${CAMERA_TEST_SOURCES})

target_include_directories(jasperengine-camera-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(jasperengine-camera-tests SYSTEM PRIVATE ${CAMERA_TEST_REPO_ROOT}/engine/libs/json ${CAMERA_TEST_REPO_ROOT}/engine/libs/tinygltf)

find_package(Threads REQUIRED)
target_link_libraries(jasperengine-camera-tests PRIVATE jasperengine gtest_main gtest Threads::Threads)

if(NOT MSVC)
    target_compile_options(jasperengine-camera-tests PRIVATE -Wall -Wextra)
endif()

set_target_properties(jasperengine-camera-tests PROPERTIES
    CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
