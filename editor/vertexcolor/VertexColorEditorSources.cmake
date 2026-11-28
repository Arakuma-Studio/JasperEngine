# Vertex colors in the editor (docs/specs/V1-vertex-colors.md): editing facade, undo, file writer,
# Vertex Paint window, scene tools, Inspector, bakes and exports. Globbed, so the streams adding
# editor files do not meet here.
file(GLOB JASPERENGINE_VERTEXCOLOR_EDITOR_SRCS CONFIGURE_DEPENDS ${EDITOR_DIR}/vertexcolor/*.cpp)
list(APPEND JASPERENGINE_EDITOR_SOURCES ${JASPERENGINE_VERTEXCOLOR_EDITOR_SRCS})
