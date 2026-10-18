# dav1d 1.5.4 - https://code.videolan.org/videolan/dav1d (BSD-2-Clause, COPYING)
#
# The AV1 decoder of the HTML UI's <video> (engine/core/htmlui/media/Av1Decoder,
# docs/html-ui/phase-6-video.md §3.2). This file replaces upstream's meson build (config/config.h,
# config/config.asm), both bit depths. The bitdepth templates (*_tmpl.c) are compiled twice, with
# BITDEPTH 8 and 16, as upstream does. The asm, with dav1d's run-time CPU dispatch: x86-64 with NASM
# (2.14 or newer; CMake's ASM_NASM, so MSVC too), aarch64 and armv7 NEON assembled by the C compiler
# (not with MSVC: upstream needs gas-preprocessor there). 32-bit x86 and WebAssembly stay C (dav1d
# has no wasm SIMD). Without NASM an x86-64 build falls back to C and says so.
# README.jasper.md lists what is vendored.

set(DAV1D_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# The engine's definitions and options (window title, Lua, -mfpu, ...) are not dav1d's, and NASM
# would be handed them too
set_property(DIRECTORY PROPERTY COMPILE_DEFINITIONS "")
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")

option(JASPERENGINE_DAV1D_ASM "dav1d's asm (x86-64 NASM, aarch64/armv7 NEON); off: C only" ON)

set(DAV1D_SOURCES
    src/cdf.c
    src/cpu.c
    src/ctx.c
    src/data.c
    src/decode.c
    src/dequant_tables.c
    src/getbits.c
    src/intra_edge.c
    src/itx_1d.c
    src/lf_mask.c
    src/lib.c
    src/log.c
    src/mem.c
    src/msac.c
    src/obu.c
    src/pal.c
    src/picture.c
    src/qm.c
    src/ref.c
    src/refmvs.c
    src/scan.c
    src/tables.c
    src/thread_task.c
    src/warpmv.c
    src/wedge.c
)
if(WIN32)
    list(APPEND DAV1D_SOURCES src/win32/thread.c)
endif()

set(DAV1D_TEMPLATE_SOURCES
    src/cdef_apply_tmpl.c
    src/cdef_tmpl.c
    src/fg_apply_tmpl.c
    src/filmgrain_tmpl.c
    src/ipred_prepare_tmpl.c
    src/ipred_tmpl.c
    src/itx_tmpl.c
    src/lf_apply_tmpl.c
    src/loopfilter_tmpl.c
    src/looprestoration_tmpl.c
    src/lr_apply_tmpl.c
    src/mc_tmpl.c
    src/recon_tmpl.c
)

# ---- the asm (upstream src/meson.build's lists, both bit depths)

set(DAV1D_ASM_X86_64
    src/x86/cpuid.asm
    src/x86/msac.asm
    src/x86/pal.asm
    src/x86/refmvs.asm
    src/x86/itx_avx512.asm
    src/x86/cdef_avx2.asm
    src/x86/itx_avx2.asm
    src/x86/cdef_sse.asm
    src/x86/itx_sse.asm
    src/x86/cdef_avx512.asm
    src/x86/filmgrain_avx512.asm
    src/x86/ipred_avx512.asm
    src/x86/loopfilter_avx512.asm
    src/x86/looprestoration_avx512.asm
    src/x86/mc_avx512.asm
    src/x86/filmgrain_avx2.asm
    src/x86/ipred_avx2.asm
    src/x86/loopfilter_avx2.asm
    src/x86/looprestoration_avx2.asm
    src/x86/mc_avx2.asm
    src/x86/filmgrain_sse.asm
    src/x86/ipred_sse.asm
    src/x86/loopfilter_sse.asm
    src/x86/looprestoration_sse.asm
    src/x86/mc_sse.asm
    src/x86/cdef16_avx512.asm
    src/x86/filmgrain16_avx512.asm
    src/x86/ipred16_avx512.asm
    src/x86/itx16_avx512.asm
    src/x86/loopfilter16_avx512.asm
    src/x86/looprestoration16_avx512.asm
    src/x86/mc16_avx512.asm
    src/x86/cdef16_avx2.asm
    src/x86/filmgrain16_avx2.asm
    src/x86/ipred16_avx2.asm
    src/x86/itx16_avx2.asm
    src/x86/loopfilter16_avx2.asm
    src/x86/looprestoration16_avx2.asm
    src/x86/mc16_avx2.asm
    src/x86/cdef16_sse.asm
    src/x86/filmgrain16_sse.asm
    src/x86/ipred16_sse.asm
    src/x86/itx16_sse.asm
    src/x86/loopfilter16_sse.asm
    src/x86/looprestoration16_sse.asm
    src/x86/mc16_sse.asm
)

set(DAV1D_ASM_AARCH64
    src/arm/64/itx.S
    src/arm/64/looprestoration_common.S
    src/arm/64/msac.S
    src/arm/64/refmvs.S
    src/arm/64/cdef.S
    src/arm/64/filmgrain.S
    src/arm/64/ipred.S
    src/arm/64/loopfilter.S
    src/arm/64/looprestoration.S
    src/arm/64/mc.S
    src/arm/64/mc_dotprod.S
    src/arm/64/cdef16.S
    src/arm/64/filmgrain16.S
    src/arm/64/ipred16.S
    src/arm/64/itx16.S
    src/arm/64/loopfilter16.S
    src/arm/64/looprestoration16.S
    src/arm/64/mc16.S
    src/arm/64/mc16_sve.S
)

set(DAV1D_ASM_ARM
    src/arm/32/itx.S
    src/arm/32/looprestoration_common.S
    src/arm/32/msac.S
    src/arm/32/refmvs.S
    src/arm/32/cdef.S
    src/arm/32/filmgrain.S
    src/arm/32/ipred.S
    src/arm/32/loopfilter.S
    src/arm/32/looprestoration.S
    src/arm/32/mc.S
    src/arm/32/cdef16.S
    src/arm/32/filmgrain16.S
    src/arm/32/ipred16.S
    src/arm/32/itx16.S
    src/arm/32/loopfilter16.S
    src/arm/32/looprestoration16.S
    src/arm/32/mc16.S
)

# the target's architecture (an Android ABI, one macOS architecture, or the processor)
if(ANDROID)
    set(DAV1D_TARGET_ARCH "${CMAKE_ANDROID_ARCH_ABI}")
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(DAV1D_TARGET_ARCH "${CMAKE_OSX_ARCHITECTURES}")
elseif(CMAKE_GENERATOR_PLATFORM)
    set(DAV1D_TARGET_ARCH "${CMAKE_GENERATOR_PLATFORM}")
else()
    set(DAV1D_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${DAV1D_TARGET_ARCH}" DAV1D_TARGET_ARCH)

set(DAV1D_ASM_SOURCES "")
set(DAV1D_ASM_KIND "none")
if(JASPERENGINE_DAV1D_ASM AND NOT EMSCRIPTEN)
    if(DAV1D_TARGET_ARCH MATCHES "^(x86_64|amd64|x64)$")
        include(CheckLanguage)
        check_language(ASM_NASM)
        if(CMAKE_ASM_NASM_COMPILER)
            enable_language(ASM_NASM)
            execute_process(COMMAND "${CMAKE_ASM_NASM_COMPILER}" -v OUTPUT_VARIABLE DAV1D_NASM_VERSION ERROR_QUIET)
            string(REGEX MATCH "version ([0-9]+\\.[0-9]+)" DAV1D_NASM_VERSION "${DAV1D_NASM_VERSION}")
            if(CMAKE_MATCH_1 VERSION_LESS 2.14)
                message(STATUS "dav1d: NASM ${CMAKE_MATCH_1} is older than 2.14: the AV1 decoder is built without its x86-64 asm (about 3x slower)")
            else()
                set(DAV1D_ASM_SOURCES ${DAV1D_ASM_X86_64} src/x86/cpu.c)
                set(DAV1D_ASM_KIND "x86-64 (NASM ${CMAKE_MATCH_1})")
            endif()
        else()
            message(STATUS "dav1d: NASM not found: the AV1 decoder is built without its x86-64 asm (about 3x slower); install nasm 2.14 or newer")
        endif()
    elseif(MSVC)
        # arm with MSVC: upstream assembles the .S files through gas-preprocessor and armasm
    elseif(DAV1D_TARGET_ARCH MATCHES "^(aarch64|arm64|arm64-v8a)$")
        enable_language(ASM)
        set(DAV1D_ASM_SOURCES ${DAV1D_ASM_AARCH64} src/arm/cpu.c)
        set(DAV1D_ASM_KIND "aarch64 NEON")
    elseif(DAV1D_TARGET_ARCH MATCHES "^(armeabi-v7a|armv7.*|arm)$")
        enable_language(ASM)
        set(DAV1D_ASM_SOURCES ${DAV1D_ASM_ARM} src/arm/cpu.c)
        set(DAV1D_ASM_KIND "armv7 NEON")
    endif()
endif()
message(STATUS "dav1d: asm ${DAV1D_ASM_KIND} (target ${DAV1D_TARGET_ARCH})")

set(DAV1D_PRIVATE_INCLUDES ${DAV1D_DIR}/config ${DAV1D_DIR} ${DAV1D_DIR}/include)
if(MSVC)
    # C11 atomics for MSVC (upstream's compat header)
    list(APPEND DAV1D_PRIVATE_INCLUDES ${DAV1D_DIR}/include/compat/msvc)
endif()

function(dav1d_configure target)
    target_include_directories(${target} PRIVATE ${DAV1D_PRIVATE_INCLUDES})
    # DAV1D_API empty: nothing of the decoder is exported from the editor's shared engine
    target_compile_definitions(${target} PRIVATE DAV1D_API=)
    if(DAV1D_ASM_SOURCES)
        target_compile_definitions(${target} PRIVATE DAV1D_ASM)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
        target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
    endif()
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE $<$<NOT:$<COMPILE_LANGUAGE:ASM_NASM>>:-fvisibility=hidden>)
    endif()
    if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
        target_compile_options(${target} PRIVATE -pthread)
    endif()
    # NASM: the include roots of the .asm files (config.asm, ext/x86/x86inc.asm), Mach-O's prefix
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:ASM_NASM>:-I${DAV1D_DIR}/src/ -I${DAV1D_DIR}/config/>)
    if(APPLE)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:ASM_NASM>:-DDAV1D_NASM_PREFIX>)
    endif()
    # armv7: the .S files are ARM-mode code (asm.S switches to Thumb only on Windows); the NDK
    # compiles C as Thumb
    if(DAV1D_ASM_KIND STREQUAL "armv7 NEON")
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:ASM>:-marm>)
    endif()
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
endfunction()

foreach(bitdepth 8 16)
    add_library(dav1d_bitdepth_${bitdepth} OBJECT ${DAV1D_TEMPLATE_SOURCES})
    target_compile_definitions(dav1d_bitdepth_${bitdepth} PRIVATE BITDEPTH=${bitdepth})
    dav1d_configure(dav1d_bitdepth_${bitdepth})
endforeach()

add_library(

    dav1d

    STATIC

    ${DAV1D_SOURCES}
    ${DAV1D_ASM_SOURCES}
    $<TARGET_OBJECTS:dav1d_bitdepth_8>
    $<TARGET_OBJECTS:dav1d_bitdepth_16>

)

dav1d_configure(dav1d)
target_include_directories(dav1d SYSTEM PUBLIC ${DAV1D_DIR}/include)
target_compile_definitions(dav1d INTERFACE DAV1D_API=)

find_package(Threads REQUIRED)
target_link_libraries(dav1d PRIVATE Threads::Threads)
if(NOT MSVC AND NOT APPLE AND NOT EMSCRIPTEN)
    target_link_libraries(dav1d PRIVATE m)
endif()
