# The libwebp decoder (README.jasper.md): the editor only, games never link it
file(GLOB_RECURSE WEBP_DECODER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_library(webp_decoder STATIC ${WEBP_DECODER_SOURCES})

# upstream's sources include "src/..." from the library's root
target_include_directories(webp_decoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(webp_decoder PROPERTIES POSITION_INDEPENDENT_CODE ON)
