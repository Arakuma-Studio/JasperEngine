# Basis Universal v2.50 - https://github.com/BinomialLLC/basis_universal (tag v2_50), Apache-2.0
# (LICENSE, NOTICE; the encoder's third-party parts in LICENSES/). What is vendored and changed:
# README.jasper.md. Spec S14.
#
# basisu_transcoder: the transcoder of the engine (the web runtime, the editor's previews, the
# tests), trimmed to what the web transcode lists need (engine/core/texture/BasisTranscode.cpp,
# TextureFallback.cpp; spec S14 R21, D5, deviation 23): the source formats the cook writes
# (ETC1S, UASTC LDR 4x4, UASTC HDR 4x4) through Basis's container-independent transcoders.
# basisu_encoder: the ETC1S / UASTC / UASTC HDR encoders (with their own transcoder copy), for
# the editor's cook. Both use engine/libs/zstd (zstd/zstd.h forwards there), SSE and OpenCL off.

set(BASISU_COMMON_DEFINITIONS
    BASISU_SUPPORT_SSE=0
    BASISU_SUPPORT_OPENCL=0
    BASISD_SUPPORT_KTX2=1
    BASISD_SUPPORT_KTX2_ZSTD=1)

set(BASISU_GNU_FE $<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>)

add_library(basisu_transcoder STATIC ${CMAKE_CURRENT_SOURCE_DIR}/transcoder/basisu_transcoder.cpp)
target_include_directories(basisu_transcoder SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/transcoder)
target_compile_definitions(basisu_transcoder PUBLIC ${BASISU_COMMON_DEFINITIONS})
# The D5 targets only (private: only the .cpp reads them; the encoder's own copy keeps every
# target): no XUASTC, ASTC LDR or XUBC7 sources (their decoders and BC7/ASTC packers), no PVRTC1/2,
# ATC or FXT1, and ETC1S to ETC2, BC1/3/4/5, EAC and uncompressed only (no ETC1S -> ASTC or BC7
# tables, TextureFallback leaves them out); the ETC1S -> BC1 tables are computed at init
# (jasper_etc1s_to_bc1_tables.inl). README.jasper.md lists the source changes these need.
# Byte reads of the UASTC bit fields on x86 too, as on ARM and wasm: the default's unaligned
# uint16 loads are undefined behaviour (UBSan stops the sanitizer runs on them). Only the
# transcoder's .cpp reads it (both copies)
target_compile_definitions(basisu_transcoder PRIVATE
    BASISD_USE_UNALIGNED_WORD_READS=0
    BASISD_SUPPORT_XUASTC=0
    BASISD_SUPPORT_PVRTC1=0
    BASISD_SUPPORT_PVRTC2=0
    BASISD_SUPPORT_ATC=0
    BASISD_SUPPORT_FXT1=0
    BASISD_SUPPORT_ASTC=0
    BASISD_SUPPORT_BC7_MODE5=0
    BASISD_SUPPORT_ASTC_HIGHER_OPAQUE_QUALITY=0
    JASPER_BASISD_COMPUTE_DXT1_TABLES=1)
target_link_libraries(basisu_transcoder PRIVATE zstd_decompress)
target_compile_options(basisu_transcoder PRIVATE $<${BASISU_GNU_FE}:-fno-strict-aliasing>)
# the web downloads the transcoder: optimised for size there (spec S14 R21)
if(EMSCRIPTEN)
    target_compile_options(basisu_transcoder PRIVATE -Os)
endif()
set_target_properties(basisu_transcoder PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
    FOLDER "libs")

if(JASPERENGINE_TEXTURE_ENCODERS)
    set(BASISU_ENCODER_SOURCES
        encoder/basisu_backend.cpp
        encoder/basisu_basis_file.cpp
        encoder/basisu_comp.cpp
        encoder/basisu_enc.cpp
        encoder/basisu_etc.cpp
        encoder/basisu_frontend.cpp
        encoder/basisu_gpu_texture.cpp
        encoder/basisu_pvrtc1_4.cpp
        encoder/basisu_resampler.cpp
        encoder/basisu_resample_filters.cpp
        encoder/basisu_ssim.cpp
        encoder/basisu_uastc_enc.cpp
        encoder/basisu_bc7e_scalar.cpp
        encoder/basisu_dds_export.cpp
        encoder/basisu_bc7enc.cpp
        encoder/jpgd.cpp
        encoder/basisu_kernels_sse.cpp
        encoder/basisu_bc15_spmd.cpp
        encoder/basisu_bc15_spmd_sse.cpp
        encoder/basisu_opencl.cpp
        encoder/pvpngreader.cpp
        encoder/basisu_uastc_hdr_4x4_enc.cpp
        encoder/basisu_astc_hdr_6x6_enc.cpp
        encoder/basisu_astc_hdr_common.cpp
        encoder/basisu_astc_ldr_common.cpp
        encoder/basisu_astc_ldr_encode.cpp
        encoder/basisu_astc_ldr_fencode.cpp
        encoder/basisu_xbc7_encode.cpp
        encoder/basisu_tinyexr.cpp
        encoder/3rdparty/android_astc_decomp.cpp
        transcoder/basisu_transcoder.cpp)
    add_library(basisu_encoder STATIC ${BASISU_ENCODER_SOURCES})
    target_include_directories(basisu_encoder SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(basisu_encoder PUBLIC
        ${BASISU_COMMON_DEFINITIONS}
        BASISU_SUPPORT_ASTCENC=0
        BASISU_DISABLE_ANDROID_ASTC_DECOMP=0)
    target_compile_definitions(basisu_encoder PRIVATE _LARGEFILE64_SOURCE=1 _FILE_OFFSET_BITS=64 BASISD_USE_UNALIGNED_WORD_READS=0)
    target_link_libraries(basisu_encoder PRIVATE zstd)
    target_compile_options(basisu_encoder PRIVATE
        $<${BASISU_GNU_FE}:-fno-strict-aliasing -ffp-contract=off>
        $<$<CXX_COMPILER_ID:MSVC>:/fp:precise /bigobj>)
    set_target_properties(basisu_encoder PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        POSITION_INDEPENDENT_CODE ON
        FOLDER "libs")
endif()
