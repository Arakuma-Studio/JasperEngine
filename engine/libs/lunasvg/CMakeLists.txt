# LunaSVG 3.5.0 - https://github.com/sammycage/lunasvg (commit cf3594d5232e075fb344365191f0ecadd1519e1e)
# with its bundled plutovg 1.3.3 (plutovg/), both MIT. The SVG rasteriser of the engine: RmlUi's
# SVG plugin (<svg>, decorator: svg()) and TextureData (.svg textures). The version the phase-0
# benchmark measured (docs/html-ui/svg-benchmark.md). Only include/, source/ and plutovg's
# include/ and source/ are vendored; this file replaces the upstream build.
#
# Every file goes through the engine's file layer (Data, ResourcePack), so LunaSVG never reads
# the disk: no system font scan (fonts are registered by engine/core/util/LunaSVGRuntime) and
# no external <image href> files (data: URIs work).

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/JasperEnginePaths.cmake)
jasperengine_glob_escape(LUNASVG_GLOB_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
file(GLOB LUNASVG_SOURCES CONFIGURE_DEPENDS ${LUNASVG_GLOB_DIR}/source/*.cpp)
file(GLOB PLUTOVG_SOURCES CONFIGURE_DEPENDS ${LUNASVG_GLOB_DIR}/plutovg/source/*.c)

add_library(

    lunasvg

    STATIC

    ${LUNASVG_SOURCES}
    ${PLUTOVG_SOURCES}

)

target_include_directories(lunasvg SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(lunasvg PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/source"
    "${CMAKE_CURRENT_SOURCE_DIR}/plutovg/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/plutovg/source"
)

# *_BUILD_STATIC must be PUBLIC: without them the headers mark the API dllimport on Windows and
# visibility("default") elsewhere, which overrides -fvisibility=hidden
target_compile_definitions(
    lunasvg

    PUBLIC

    LUNASVG_BUILD_STATIC
    PLUTOVG_BUILD_STATIC

    PRIVATE

    LUNASVG_BUILD
    PLUTOVG_BUILD
    LUNASVG_DISABLE_LOAD_SYSTEM_FONTS
    LUNASVG_DISABLE_EXTERNAL_RESOURCES
    PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD
)

target_compile_features(lunasvg PUBLIC cxx_std_17)
set_target_properties(lunasvg PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

if(NOT MSVC)
    # keep every lunasvg/plutovg symbol inside libjasperengine when the editor builds it shared
    target_compile_options(lunasvg PRIVATE -fvisibility=hidden $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>)
    if(NOT APPLE AND NOT EMSCRIPTEN)
        target_link_libraries(lunasvg PRIVATE m)
    endif()
endif()

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
    target_compile_options(lunasvg PRIVATE -pthread)
endif()
