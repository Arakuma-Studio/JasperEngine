# Title Art's module sources (docs/specs/S80-title-art-core.md §6.15), included by
# engine/CMakeLists.txt when JASPERENGINE_TITLE_ART is ON. Globbed so every title spec adds files
# in its own folder without editing CMake: the root (document, codec, migrations, hashes, schema,
# registry: S80), text/, geometry/, render/, effects/, generators/ (S80, S83), runtime/ (S81),
# analysis/, gpu/ (S83), anim/ (S85). three_d/ is S86's, compiled under S81's sub-options
# (JASPERENGINE_TITLE_ART_3D, JASPERENGINE_TITLE_ART_MESHER), so the glob leaves it out.

file(GLOB_RECURSE JASPERENGINE_TITLE_ART_SRCS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp)
list(FILTER JASPERENGINE_TITLE_ART_SRCS EXCLUDE REGEX "/titleart/three_d/")
# S81 R51: three_d/ under JASPERENGINE_TITLE_ART_3D; the mesher's files (three_d/mesher/ and S86's
# earcut users) under JASPERENGINE_TITLE_ART_MESHER as well
if(JASPERENGINE_TITLE_ART_3D)
    file(GLOB_RECURSE jasperengineTitleArt3DSrcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/three_d/*.cpp)
    if(NOT JASPERENGINE_TITLE_ART_MESHER)
        list(FILTER jasperengineTitleArt3DSrcs EXCLUDE REGEX "/three_d/mesher/")
    endif()
    list(APPEND JASPERENGINE_TITLE_ART_SRCS ${jasperengineTitleArt3DSrcs})
endif()
list(SORT JASPERENGINE_TITLE_ART_SRCS)
