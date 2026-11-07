# astc-encoder 5.7.0 - https://github.com/ARM-software/astc-encoder (tag 5.7.0), Apache-2.0
# (LICENSE.txt). ASTC LDR encoding and decoding of the editor's texture cook (S14,
# README.jasper.md). Editor only. Replaces upstream's Source/cmake_core.cmake for the library.
#
# Always invariant (upstream's ASTCENC_INVARIANCE=ON): no fused multiply-adds, precise floating
# point, so SSE4.1 (x86-64), NEON (arm64) and scalar builds write byte-identical blocks and the
# cook cache key need not name the ISA.

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/JasperEnginePaths.cmake)
jasperengine_glob_escape(ASTCENC_GLOB_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
file(GLOB ASTCENC_SOURCES CONFIGURE_DEPENDS ${ASTCENC_GLOB_DIR}/Source/astcenc_*.cpp)

add_library(astcenc STATIC ${ASTCENC_SOURCES})

target_include_directories(astcenc SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Source)

set(ASTCENC_IS_GNU_FE $<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>)
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" ASTCENC_PROCESSOR)
if(CMAKE_OSX_ARCHITECTURES)
    # a universal macOS build compiles both slices from one set of flags: no SIMD then
    list(LENGTH CMAKE_OSX_ARCHITECTURES ASTCENC_ARCH_COUNT)
    if(ASTCENC_ARCH_COUNT EQUAL 1)
        string(TOLOWER "${CMAKE_OSX_ARCHITECTURES}" ASTCENC_PROCESSOR)
    else()
        set(ASTCENC_PROCESSOR "universal")
    endif()
endif()

if(ASTCENC_PROCESSOR MATCHES "^(x86_64|amd64)$")
    target_compile_definitions(astcenc PRIVATE
        ASTCENC_NEON=0 ASTCENC_SVE=0 ASTCENC_SSE=41 ASTCENC_AVX=0 ASTCENC_POPCNT=1 ASTCENC_F16C=0
        ASTCENC_X86_GATHERS=0)
    target_compile_options(astcenc PRIVATE $<${ASTCENC_IS_GNU_FE}:-msse4.1 -mpopcnt>)
elseif(ASTCENC_PROCESSOR MATCHES "^(arm64|aarch64)$")
    target_compile_definitions(astcenc PRIVATE
        ASTCENC_NEON=1 ASTCENC_SVE=0 ASTCENC_SSE=0 ASTCENC_AVX=0 ASTCENC_POPCNT=0 ASTCENC_F16C=0)
else()
    target_compile_definitions(astcenc PRIVATE
        ASTCENC_NEON=0 ASTCENC_SVE=0 ASTCENC_SSE=0 ASTCENC_AVX=0 ASTCENC_POPCNT=0 ASTCENC_F16C=0)
endif()

target_compile_options(astcenc PRIVATE
    $<${ASTCENC_IS_GNU_FE}:-ffp-contract=off -fno-math-errno>
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-fno-unsafe-math-optimizations>
    $<$<CXX_COMPILER_ID:MSVC>:/fp:precise>)
set_target_properties(astcenc PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
    FOLDER "libs")
