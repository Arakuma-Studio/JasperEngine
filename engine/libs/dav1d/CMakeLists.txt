# dav1d 1.5.4 - https://code.videolan.org/videolan/dav1d (BSD-2-Clause, COPYING)
#
# The AV1 decoder of the HTML UI's <video> (engine/core/htmlui/media/Av1Decoder,
# docs/html-ui/phase-6-video.md §3.2). This file replaces upstream's meson build: the C sources
# only (no asm, config/config.h), both bit depths. The bitdepth templates (*_tmpl.c) are compiled
# twice, with BITDEPTH 8 and 16, as upstream does. README.jasper.md lists what is vendored.

set(DAV1D_DIR ${CMAKE_CURRENT_SOURCE_DIR})

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

set(DAV1D_PRIVATE_INCLUDES ${DAV1D_DIR}/config ${DAV1D_DIR} ${DAV1D_DIR}/include)
if(MSVC)
    # C11 atomics for MSVC (upstream's compat header)
    list(APPEND DAV1D_PRIVATE_INCLUDES ${DAV1D_DIR}/include/compat/msvc)
endif()

function(dav1d_configure target)
    target_include_directories(${target} PRIVATE ${DAV1D_PRIVATE_INCLUDES})
    # DAV1D_API empty: nothing of the decoder is exported from the editor's shared engine
    target_compile_definitions(${target} PRIVATE DAV1D_API=)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
        target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
    endif()
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -fvisibility=hidden)
    endif()
    if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
        target_compile_options(${target} PRIVATE -pthread)
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
