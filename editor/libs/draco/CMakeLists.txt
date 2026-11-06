# The Draco decoder (README.jasper.md): the editor only, games never link it
file(GLOB_RECURSE DRACO_DECODER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/draco/*.cc)

add_library(draco_decoder STATIC ${DRACO_DECODER_SOURCES})

target_include_directories(draco_decoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

set_target_properties(draco_decoder PROPERTIES POSITION_INDEPENDENT_CODE ON)
