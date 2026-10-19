# dav1d's asm against its C functions (check.c), built with the engine's dav1d CMake for one
# architecture: run.sh cross-compiles it for aarch64 and armv7 and runs it under QEMU.
cmake_minimum_required(VERSION 3.20)
project(dav1d_asm_check C)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../engine/libs/dav1d ${CMAKE_BINARY_DIR}/dav1d)
add_executable(dav1d-asm-check check.c)
# the check reads dav1d's CPU dispatch symbols (src/cpu.h); hidden in the library, linked statically
target_link_libraries(dav1d-asm-check PRIVATE dav1d)
