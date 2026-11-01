# Local patch: stb_image_resize2.h's vertical gather and scatter index each of their two function
# tables on its own ((c ? a[i] : b[i]) instead of (c ? a : b)[i], the same call): GCC 13's UBSan
# object-size check reported the valid original at -O2. Keep it when updating stb_image_resize2.

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

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
	target_compile_options(stb PRIVATE -pthread)
endif()