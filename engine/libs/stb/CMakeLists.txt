add_library(

    stb

    STATIC

    stb_vorbis.c
    stb_rect_pack.c
    stb_truetype.c
    stb_image.c
    stb_image_write.c
    stb_image_resize2.c

)

find_package(Threads REQUIRED)
target_link_libraries(stb PRIVATE Threads::Threads)

# stb_image_resize2 is deterministic across CPUs only without fused multiply-adds (its DETERMINISM
# notes): the texture mip chain relies on it
if(NOT MSVC)
    target_compile_options(stb PRIVATE -ffp-contract=off)
endif()

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
	target_compile_options(stb PRIVATE -pthread)
endif()