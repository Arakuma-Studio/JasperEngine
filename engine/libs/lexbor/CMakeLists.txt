# lexbor 3.1.0 - https://github.com/lexbor/lexbor (Apache-2.0)
#
# The HTML5 parser of the HTML UI compatibility layer (engine/core/htmlui/compat/html/HtmlParser,
# docs/html-ui/phase-2-compat.md §3.7.1). Upstream has no 3.1.0 tag: the vendored sources are
# master at commit f4cbbcd91359a0ec9499e3ce7e263de629482d61 (2026-09-28), whose `version` file
# says 3.1.0 (see PATCHES.md). Only the modules the HTML parser needs are vendored: core, dom,
# html, ns, tag and the platform ports; this file replaces the upstream build. One local patch
# records the source offsets of start tags, attributes, comments and text (PATCHES.md).

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/JasperEnginePaths.cmake)
jasperengine_glob_escape(LEXBOR_GLOB_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
file(GLOB LEXBOR_SOURCES CONFIGURE_DEPENDS
    ${LEXBOR_GLOB_DIR}/lexbor/core/*.c
    ${LEXBOR_GLOB_DIR}/lexbor/dom/*.c
    ${LEXBOR_GLOB_DIR}/lexbor/dom/interfaces/*.c
    ${LEXBOR_GLOB_DIR}/lexbor/html/*.c
    ${LEXBOR_GLOB_DIR}/lexbor/html/interfaces/*.c
    ${LEXBOR_GLOB_DIR}/lexbor/html/tokenizer/*.c
    ${LEXBOR_GLOB_DIR}/lexbor/html/tree/*.c
    ${LEXBOR_GLOB_DIR}/lexbor/html/tree/insertion_mode/*.c
    ${LEXBOR_GLOB_DIR}/lexbor/ns/*.c
    ${LEXBOR_GLOB_DIR}/lexbor/tag/*.c
)

# memory/fs/perf of the platform: windows_nt on Windows, posix everywhere else (Linux, macOS,
# Android, iOS, Emscripten)
if(WIN32)
    set(LEXBOR_PORT windows_nt)
else()
    set(LEXBOR_PORT posix)
endif()
file(GLOB LEXBOR_PORT_SOURCES CONFIGURE_DEPENDS ${LEXBOR_GLOB_DIR}/lexbor/ports/${LEXBOR_PORT}/lexbor/core/*.c)

add_library(

    lexbor

    STATIC

    ${LEXBOR_SOURCES}
    ${LEXBOR_PORT_SOURCES}

)

target_include_directories(lexbor SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# LEXBOR_STATIC must be PUBLIC: without it the headers mark the API dllimport on Windows and
# visibility("default") elsewhere, which overrides -fvisibility=hidden. (The source-offset
# fields of lxb_dom_node_t are defined by the patched dom/interfaces/node.h itself, so every
# build system and every includer sees the same layout: PATCHES.md.)
target_compile_definitions(lexbor PUBLIC LEXBOR_STATIC)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(lexbor PRIVATE _POSIX_C_SOURCE=199309L)
endif()
if(MSVC)
    target_compile_definitions(lexbor PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set_target_properties(lexbor PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

if(NOT MSVC)
    # keep every lxb_*/lexbor_* symbol inside libjasperengine when the editor builds it shared
    target_compile_options(lexbor PRIVATE -fvisibility=hidden)
    # ceil() in core/diyfp.c
    if(NOT APPLE AND NOT EMSCRIPTEN)
        target_link_libraries(lexbor PRIVATE m)
    endif()
endif()

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
    target_compile_options(lexbor PRIVATE -pthread)
endif()
