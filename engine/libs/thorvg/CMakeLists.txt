# ThorVG 1.2.0 - https://github.com/thorvg/thorvg (commit caa5af1167d7b788a3498d69c577e9d6e284dbb7), MIT.
# The engine's Lottie renderer (<lottie>, engine/core/htmlui/vector). CPU (software) engine only;
# loaders lottie (without jerryscript: no expressions), png, jpg, raw and sfnt (TTF/OTF text
# layers). The SVG loader is not vendored: SVG goes through LunaSVG (libs/lunasvg). Upstream
# builds with meson; this file and config/config.h replace it.
# Local patch: src/renderer/tvgInitializer.cpp and src/common/tvgCommon.h no longer replace the
# global operator new / delete (upstream maps them to malloc / free for the whole program: here
# that covered the engine library without its sized delete, broke `new`'s bad_alloc contract and
# made AddressSanitizer report alloc-dealloc mismatches). Without it a tvg::malloc/calloc block
# must be freed with tvg::free and a `new` object deleted: src/renderer/cpu_engine/tvgSwRle.cpp
# rleRender allocates its SwRle with `new` (upstream: tvg::calloc, while rleFree deletes it).
# Keep both when updating ThorVG.

set(THORVG_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/JasperEnginePaths.cmake)
jasperengine_glob_escape(THORVG_SRC_GLOB "${THORVG_SRC}")
file(GLOB THORVG_SOURCES CONFIGURE_DEPENDS
    ${THORVG_SRC_GLOB}/common/*.cpp
    ${THORVG_SRC_GLOB}/renderer/*.cpp
    ${THORVG_SRC_GLOB}/renderer/cpu_engine/*.cpp
    ${THORVG_SRC_GLOB}/loaders/raw/*.cpp
    ${THORVG_SRC_GLOB}/loaders/lottie/*.cpp
    ${THORVG_SRC_GLOB}/loaders/png/*.cpp
    ${THORVG_SRC_GLOB}/loaders/jpg/*.cpp
    ${THORVG_SRC_GLOB}/loaders/sfnt/*.cpp
)

add_library(

    thorvg

    STATIC

    ${THORVG_SOURCES}

)

# thorvg_lottie.h (LottieAnimation) sits next to the Lottie loader upstream
target_include_directories(thorvg SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/inc"
    "${THORVG_SRC}/loaders/lottie"
)
# config/ first: the sources include "config.h"
target_include_directories(thorvg BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/config"
    "${THORVG_SRC}/common"
    "${THORVG_SRC}/renderer"
    "${THORVG_SRC}/renderer/cpu_engine"
    "${THORVG_SRC}/loaders/raw"
    "${THORVG_SRC}/loaders/lottie"
    "${THORVG_SRC}/loaders/png"
    "${THORVG_SRC}/loaders/jpg"
    "${THORVG_SRC}/loaders/sfnt"
)

# TVG_STATIC must be PUBLIC: without it thorvg.h marks the API dllimport on Windows and
# visibility("default") elsewhere, which overrides -fvisibility=hidden
target_compile_definitions(thorvg PUBLIC TVG_STATIC)
if(WIN32)
    target_compile_definitions(thorvg PRIVATE NOMINMAX)
endif()

target_compile_features(thorvg PUBLIC cxx_std_17)

if(NOT MSVC)
    # keep every tvg:: symbol inside libjasperengine when the editor builds it shared
    target_compile_options(thorvg PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif()

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
    target_compile_options(thorvg PRIVATE -pthread)
endif()
