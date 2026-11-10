# Brotli 1.2.1 (MIT), decoder and common code only. See README.jasper.md.
add_library(brotlidec STATIC
    c/common/constants.c
    c/common/context.c
    c/common/dictionary.c
    c/common/platform.c
    c/common/shared_dictionary.c
    c/common/transform.c
    c/dec/bit_reader.c
    c/dec/decode.c
    c/dec/huffman.c
    c/dec/prefix.c
    c/dec/state.c
    c/dec/static_init.c
)
target_include_directories(brotlidec SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/c/include")
target_compile_definitions(brotlidec PRIVATE BROTLI_BUILD_PORTABLE)
set_target_properties(brotlidec PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(brotlidec PRIVATE -fvisibility=hidden)
endif()
if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
    target_compile_options(brotlidec PRIVATE -pthread)
endif()
