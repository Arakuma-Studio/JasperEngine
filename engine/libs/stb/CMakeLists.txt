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
# UBSan: stb_image_resize2 v2.12's coefficient packer stores pairs of floats as one 64-bit word
# at float alignment (STBIR_MOVE_2 in stbir__pack_coefficients). The sanitized builds stop at the
# first report (-fno-sanitize-recover), which a suppression cannot skip: this file alone is built
# without the alignment check.
if(JASPERENGINE_SANITIZE_UNDEFINED AND NOT MSVC)
    set_source_files_properties(stb_image_resize2.c PROPERTIES COMPILE_OPTIONS -fno-sanitize=alignment)
endif()
# UBSan: stb_image_write v1.16's JPEG bit writer shifts its int bit buffer left past the sign
# (stbiw__jpg_writeBits; upstream's FloatImageTest writes a JPEG): this file alone is built
# without the shift check, as above
if(JASPERENGINE_SANITIZE_UNDEFINED AND NOT MSVC)
    set_source_files_properties(stb_image_write.c PROPERTIES COMPILE_OPTIONS -fno-sanitize=shift)
endif()
