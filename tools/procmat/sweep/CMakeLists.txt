# The compile sweep and measurements of S90 M0 (docs/specs/S90-procedural-material.md, Deviations): a
# standalone target, so it builds in minutes without the engine or the editor. Linux and Mesa only.
#
#   tools/build.sh exec linux cmake -S tools/procmat/sweep -B build/procmat-sweep -DCMAKE_BUILD_TYPE=Release
#   tools/build.sh exec linux cmake --build build/procmat-sweep -j4
cmake_minimum_required(VERSION 3.22)
project(procmat-sweep CXX C)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(${REPO_ROOT}/engine/libs/shadercompiler ${CMAKE_BINARY_DIR}/shadercompiler)

add_executable(procmat-sweep main.cpp GlPass.cpp ${REPO_ROOT}/editor/procmat/SpirvInterpreter.cpp)
target_include_directories(procmat-sweep PRIVATE
    ${REPO_ROOT}/editor/procmat
    ${REPO_ROOT}/engine/libs/json
    ${REPO_ROOT}/engine/libs/shadercompiler/libs/glslang)
target_compile_options(procmat-sweep PRIVATE -Wall -Wextra)
target_link_libraries(procmat-sweep PRIVATE shadercompiler EGL)
find_package(Threads REQUIRED)
target_link_libraries(procmat-sweep PRIVATE Threads::Threads)
