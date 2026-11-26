# engine/core/vertexcolor (docs/specs/V1-vertex-colors.md): the pure vertex color module (data model,
# .vcolors file, op engine, bakes, split, selection, runtime layer), with no ECS, render or editor
# include. Globbed, so the streams adding files do not meet here. No contraction, so ops give the same
# bytes on every target (R27).
file(GLOB JASPERENGINE_VERTEXCOLOR_SRCS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp)

set(JASPERENGINE_VERTEXCOLOR_FLAGS "")
if(MSVC)
    list(APPEND JASPERENGINE_VERTEXCOLOR_FLAGS /fp:precise)
else()
    list(APPEND JASPERENGINE_VERTEXCOLOR_FLAGS -ffp-contract=off -fno-math-errno)
endif()
if(JASPERENGINE_VERTEXCOLOR_SRCS)
    set_source_files_properties(${JASPERENGINE_VERTEXCOLOR_SRCS} PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_VERTEXCOLOR_FLAGS}")
endif()
