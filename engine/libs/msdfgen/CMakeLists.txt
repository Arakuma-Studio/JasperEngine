# msdfgen 1.13.0 (core only) - https://github.com/Chlumsky/msdfgen (MIT, NOTICE.md)
#
# Multi-channel signed distance fields of glyph outlines: the HTML UI's MTSDF glyph atlas
# (engine/core/htmlui/RmlFontMsdf.cpp, S16 R16). Only core/ is vendored: no ext/ (FreeType,
# Skia, PNG), no OpenMP. msdfgen-config.h stands in for the header its CMake generates.

file(GLOB MSDFGEN_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp")

add_library(

    msdfgen

    STATIC

    ${MSDFGEN_CORE_SOURCES}

)

target_include_directories(msdfgen PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
	target_compile_options(msdfgen PRIVATE -pthread)
endif()
