# jasperengine-lattice-tests: unit tests of the lattice math (spec §3) and of the presets,
# resample, mirror and selection helpers (spec §4). Built with -DJASPERENGINE_LATTICE_TESTS=ON
# from the editor build or the standalone engine build, or on its own:
#   cmake -S engine/tests/lattice -B build/lattice-tests && cmake --build build/lattice-tests
# It compiles the math and lattice sources only (no engine library, no GPU, no display), so it
# builds in seconds and runs anywhere. Tests are Test*.cpp files next to this one.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(jasperengine-lattice-tests CXX)
    if(NOT DEFINED JASPERENGINE_CXX_STANDARD)
        set(JASPERENGINE_CXX_STANDARD 17)
    endif()
    enable_testing()
endif()

set(LATTICE_TEST_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
get_filename_component(LATTICE_TEST_ENGINE_DIR "${LATTICE_TEST_ENGINE_DIR}" ABSOLUTE)

# googletest from the repository's yaml-cpp test tree (not shipped with the engine), as the
# HTML UI tests take it; they may have added it already in the same build
if(NOT TARGET gtest)
    set(JASPER_GTEST_DIR "${LATTICE_TEST_ENGINE_DIR}/../libs/yaml-cpp/test/gtest-1.11.0")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)         # gtest 1.11 declares 2.8.12: CMake 4 rejects it, 3.x warns
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(${JASPER_GTEST_DIR} ${CMAKE_BINARY_DIR}/gtest EXCLUDE_FROM_ALL)
    if(MSVC)
        target_compile_options(gtest PRIVATE /w)
    else()
        target_compile_options(gtest PRIVATE -w)
    endif()
endif()

file(GLOB LATTICE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

# what the solver links against: the math classes Matrix4 pulls in, and Angle (their degree
# conversions), whose one engine dependency EngineStubs.cpp provides
set(LATTICE_TEST_ENGINE_SOURCES
    ${LATTICE_TEST_ENGINE_DIR}/core/math/Vector2.cpp
    ${LATTICE_TEST_ENGINE_DIR}/core/math/Vector3.cpp
    ${LATTICE_TEST_ENGINE_DIR}/core/math/Vector4.cpp
    ${LATTICE_TEST_ENGINE_DIR}/core/math/Matrix3.cpp
    ${LATTICE_TEST_ENGINE_DIR}/core/math/Matrix4.cpp
    ${LATTICE_TEST_ENGINE_DIR}/core/math/Quaternion.cpp
    ${LATTICE_TEST_ENGINE_DIR}/core/math/AABB.cpp
    ${LATTICE_TEST_ENGINE_DIR}/core/math/OBB.cpp
    ${LATTICE_TEST_ENGINE_DIR}/core/math/Plane.cpp
    ${LATTICE_TEST_ENGINE_DIR}/core/util/Angle.cpp
    ${LATTICE_TEST_ENGINE_DIR}/core/lattice/LatticeSolver.cpp
    ${LATTICE_TEST_ENGINE_DIR}/core/lattice/LatticePresets.cpp
)

add_executable(jasperengine-lattice-tests ${LATTICE_TEST_SOURCES} ${LATTICE_TEST_ENGINE_SOURCES})

# the engine's include layout (headers only: nothing below is compiled or linked)
target_include_directories(jasperengine-lattice-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LATTICE_TEST_ENGINE_DIR}/shaders
    ${LATTICE_TEST_ENGINE_DIR}/core
    ${LATTICE_TEST_ENGINE_DIR}/core/action
    ${LATTICE_TEST_ENGINE_DIR}/core/buffer
    ${LATTICE_TEST_ENGINE_DIR}/core/component
    ${LATTICE_TEST_ENGINE_DIR}/core/ecs
    ${LATTICE_TEST_ENGINE_DIR}/core/io
    ${LATTICE_TEST_ENGINE_DIR}/core/manager
    ${LATTICE_TEST_ENGINE_DIR}/core/math
    ${LATTICE_TEST_ENGINE_DIR}/core/object
    ${LATTICE_TEST_ENGINE_DIR}/core/pool
    ${LATTICE_TEST_ENGINE_DIR}/core/registry
    ${LATTICE_TEST_ENGINE_DIR}/core/render
    ${LATTICE_TEST_ENGINE_DIR}/core/script
    ${LATTICE_TEST_ENGINE_DIR}/core/shader
    ${LATTICE_TEST_ENGINE_DIR}/core/subsystem
    ${LATTICE_TEST_ENGINE_DIR}/core/texture
    ${LATTICE_TEST_ENGINE_DIR}/core/util
    ${LATTICE_TEST_ENGINE_DIR}/renders
)
target_include_directories(jasperengine-lattice-tests SYSTEM PRIVATE
    ${LATTICE_TEST_ENGINE_DIR}/libs
    ${LATTICE_TEST_ENGINE_DIR}/libs/sokol
    ${LATTICE_TEST_ENGINE_DIR}/libs/lua
    ${LATTICE_TEST_ENGINE_DIR}/libs/luabridge3
)

target_link_libraries(jasperengine-lattice-tests PRIVATE gtest gtest_main)

if(NOT MSVC)
    set_source_files_properties(
        ${LATTICE_TEST_SOURCES}
        ${LATTICE_TEST_ENGINE_DIR}/core/lattice/LatticeSolver.cpp
        ${LATTICE_TEST_ENGINE_DIR}/core/lattice/LatticePresets.cpp
        PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra"
    )
endif()

set_target_properties(jasperengine-lattice-tests PROPERTIES
    CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_test(NAME jasperengine-lattice-tests COMMAND jasperengine-lattice-tests)
