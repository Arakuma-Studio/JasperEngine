# 3D titles (docs/specs/S86-title-3d.md), included by engine/CMakeLists.txt after Title Art's
# module sources. Two switches, both sub-options of JASPERENGINE_TITLE_ART (S81 owns their export
# wiring through ModuleUsage; declaring them again there is harmless, option() keeps the first):
# - JASPERENGINE_TITLE_ART_3D: the 3D layer renderer, its document members, materials, tiers,
#   the cooked mesh reader and the environment sampling (three_d/*.cpp);
# - JASPERENGINE_TITLE_ART_MESHER: the CPU mesher (three_d/mesher/*.cpp) and earcut. A game
#   whose 3D titles all ship cooked .titlemesh files leaves it out.

option(JASPERENGINE_TITLE_ART_3D "Compile 3D titles (engine/core/titleart/three_d)" ON)
option(JASPERENGINE_TITLE_ART_MESHER "Compile the 3D title mesher (engine/core/titleart/three_d/mesher)" ON)
if(NOT JASPERENGINE_TITLE_ART_3D)
    set(JASPERENGINE_TITLE_ART_MESHER OFF)
endif()
if(JASPERENGINE_SHARED AND NOT (JASPERENGINE_TITLE_ART_3D AND JASPERENGINE_TITLE_ART_MESHER))
    message(FATAL_ERROR "The editor's engine (JASPERENGINE_SHARED) has 3D titles: JASPERENGINE_TITLE_ART_3D and _MESHER stay ON")
endif()

set(JASPERENGINE_TITLE_ART_3D_SRCS "")
if(JASPERENGINE_TITLE_ART_3D)
    file(GLOB JASPERENGINE_TITLE_ART_3D_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*.cpp)
    if(JASPERENGINE_TITLE_ART_MESHER)
        file(GLOB jasperengineTitleMesherSrcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/mesher/*.cpp)
        list(APPEND JASPERENGINE_TITLE_ART_3D_SRCS ${jasperengineTitleMesherSrcs})
        add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../libs/earcut ${CMAKE_BINARY_DIR}/libs/earcut)
        list(APPEND JASPERENGINE_TITLE_ART_LIBS earcut)
    endif()
    list(SORT JASPERENGINE_TITLE_ART_3D_SRCS)
    list(APPEND JASPERENGINE_TITLE_ART_SRCS ${JASPERENGINE_TITLE_ART_3D_SRCS})
endif()
