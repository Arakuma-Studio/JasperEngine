# Editor only (the Vertex Animation importers read Houdini/OpenVAT EXR textures; editor/util/FloatImage):
# exported games never read EXR. tinyexr v1 (tinyexr.h and the exr_reader.hh / streamreader.hh it
# includes) from the v3.2.0 release, unmodified.
# Deflate (ZIP/ZIPS/PXR24) goes through the stb_image zlib decoder and the stb_image_write
# compressor the engine already links (TINYEXR_USE_STB_ZLIB), so neither miniz nor zlib is vendored.
add_library(tinyexr STATIC tinyexr.cc)

target_include_directories(tinyexr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(tinyexr PUBLIC TINYEXR_USE_MINIZ=0 TINYEXR_USE_STB_ZLIB=1 TINYEXR_USE_THREAD=0 TINYEXR_USE_OPENMP=0)
# stbi_zlib_decode_buffer / stbi_zlib_compress come from the engine library (engine/libs/stb)
target_link_libraries(tinyexr PRIVATE jasperengine)

set_target_properties(tinyexr PROPERTIES POSITION_INDEPENDENT_CODE ON)
