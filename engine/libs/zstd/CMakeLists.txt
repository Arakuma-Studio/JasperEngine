# Zstandard 1.5.7 - https://github.com/facebook/zstd (tag v1.5.7), BSD-3-Clause (LICENSE). The
# supercompression of cooked KTX2 textures (S14). Only lib/common, lib/compress, lib/decompress
# and the public headers are vendored (README.jasper.md); this file replaces the upstream build.
#
# zstd_decompress: the decoder (and the common code), what every game links (Ktx2.cpp).
# zstd: the encoder on top of it, for the editor's texture cook and the tests that write KTX2
# files; linking it links zstd_decompress too, so no object is in two libraries.
# Both single-threaded C, no assembly (ZSTD_DISABLE_ASM: one code path on every compiler), no
# legacy formats, hidden visibility so the shared editor engine never exports ZSTD_* symbols.

set(ZSTD_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

set(ZSTD_COMMON_SOURCES
    ${ZSTD_LIB_DIR}/common/debug.c
    ${ZSTD_LIB_DIR}/common/entropy_common.c
    ${ZSTD_LIB_DIR}/common/error_private.c
    ${ZSTD_LIB_DIR}/common/fse_decompress.c
    ${ZSTD_LIB_DIR}/common/xxhash.c
    ${ZSTD_LIB_DIR}/common/zstd_common.c
)
set(ZSTD_DECOMPRESS_SOURCES
    ${ZSTD_LIB_DIR}/decompress/huf_decompress.c
    ${ZSTD_LIB_DIR}/decompress/zstd_ddict.c
    ${ZSTD_LIB_DIR}/decompress/zstd_decompress.c
    ${ZSTD_LIB_DIR}/decompress/zstd_decompress_block.c
)
file(GLOB ZSTD_COMPRESS_SOURCES CONFIGURE_DEPENDS ${ZSTD_LIB_DIR}/compress/*.c)
# zstdmt_compress.c calls the pool; without ZSTD_MULTITHREAD it runs jobs inline
list(APPEND ZSTD_COMPRESS_SOURCES ${ZSTD_LIB_DIR}/common/pool.c ${ZSTD_LIB_DIR}/common/threading.c)

function(jasperengine_zstd_library name)
    add_library(${name} STATIC ${ARGN})
    target_include_directories(${name} SYSTEM PUBLIC ${ZSTD_LIB_DIR})
    target_compile_definitions(${name} PRIVATE
        ZSTD_DISABLE_ASM=1
        ZSTD_LEGACY_SUPPORT=0
        # the decoder's error strings stay: a broken cooked file logs why it failed
        XXH_NAMESPACE=JE_ZSTD_)
    # the API's default-visibility attribute off, so the hidden preset applies to it too
    target_compile_definitions(${name} PUBLIC
        ZSTDLIB_VISIBLE=
        ZSTDERRORLIB_VISIBLE=
        ZDICTLIB_VISIBLE=)
    set_target_properties(${name} PROPERTIES
        C_VISIBILITY_PRESET hidden
        POSITION_INDEPENDENT_CODE ON
        FOLDER "libs")
endfunction()

jasperengine_zstd_library(zstd_decompress ${ZSTD_COMMON_SOURCES} ${ZSTD_DECOMPRESS_SOURCES})
jasperengine_zstd_library(zstd ${ZSTD_COMPRESS_SOURCES})
target_link_libraries(zstd PUBLIC zstd_decompress)
