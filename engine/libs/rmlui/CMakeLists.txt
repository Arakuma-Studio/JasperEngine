# RmlUi 6.3 - https://github.com/mikke89/RmlUi (tag 6.3, ba95ffe8bfb6370efb2cdcca927eaad4710c5413)
# Core and Debugger only. The font engine is the engine's own (engine/core/htmlui/RmlFontEngine),
# so there is no FreeType; see docs/html-ui/FORK.md for the vendored subset and patches.
file(GLOB RMLUI_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/Elements/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/Layout/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Debugger/*.cpp
)

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

    PRIVATE

    RMLUI_VERSION="6.3"
    RMLUI_NO_FILE_INTERFACE_DEFAULT   # a missing RmlFile is an error, never a silent fopen
)

target_compile_features(rmlui PUBLIC cxx_std_17)

# Keep every Rml:: symbol inside libjasperengine when the editor builds it shared
if(NOT MSVC)
    target_compile_options(rmlui PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif()

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
    target_compile_options(rmlui PRIVATE -pthread)
endif()
