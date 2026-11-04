# RmlUi 6.3 - https://github.com/mikke89/RmlUi (tag 6.3, ba95ffe8bfb6370efb2cdcca927eaad4710c5413)
# Core, Debugger (JASPERENGINE_HTMLUI_DEBUGGER) and the SVG plugin. The font engine is the
# engine's own (engine/core/htmlui/RmlFontEngine), so there is no FreeType; see
# docs/html-ui/FORK.md for the vendored subset and patches.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/JasperEnginePaths.cmake)
jasperengine_glob_escape(RMLUI_GLOB_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
file(GLOB RMLUI_SOURCES CONFIGURE_DEPENDS
    ${RMLUI_GLOB_DIR}/Source/Core/*.cpp
    ${RMLUI_GLOB_DIR}/Source/Core/Elements/*.cpp
    ${RMLUI_GLOB_DIR}/Source/Core/Layout/*.cpp
    ${RMLUI_GLOB_DIR}/Source/SVG/*.cpp
)
# JASPERENGINE_HTMLUI_DEBUGGER (engine/CMakeLists.txt): the editor, Debug exports, the tests
if(JASPERENGINE_HTMLUI_DEBUGGER)
    file(GLOB RMLUI_DEBUGGER_SOURCES CONFIGURE_DEPENDS ${RMLUI_GLOB_DIR}/Source/Debugger/*.cpp)
    list(APPEND RMLUI_SOURCES ${RMLUI_DEBUGGER_SOURCES})
endif()

add_library(

    rmlui

    STATIC

    ${RMLUI_SOURCES}

)

target_include_directories(rmlui SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Include")

# RMLUI_STATIC_LIB must be PUBLIC: without it Header.h makes RMLUICORE_API dllimport on
# Windows and visibility("default") elsewhere, which overrides -fvisibility=hidden
target_compile_definitions(
    rmlui

    PUBLIC

    RMLUI_STATIC_LIB
    # calc()/min()/max()/clamp() (upstream PR #983, docs/html-ui/FORK.md): PUBLIC, it changes the
    # layout of ComputedValues and the Unit enum every user of the headers sees
    RMLUI_MATH_EXPRESSIONS

    PRIVATE

    RMLUI_VERSION="6.3"
    RMLUI_NO_FILE_INTERFACE_DEFAULT   # a missing RmlFile is an error, never a silent fopen
    RMLUI_SVG_PLUGIN                  # Rml::Initialise registers the SVG plugin (<svg>, decorator: svg())
)

# The SVG plugin rasterises with LunaSVG, as upstream (docs/html-ui/svg-benchmark.md)
target_link_libraries(rmlui PRIVATE lunasvg)

target_compile_features(rmlui PUBLIC cxx_std_17)

# Keep every Rml:: symbol inside libjasperengine when the editor builds it shared
if(NOT MSVC)
    target_compile_options(rmlui PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif()

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
    target_compile_options(rmlui PRIVATE -pthread)
endif()
