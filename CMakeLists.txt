cmake_minimum_required(VERSION 3.20)

# The oldest macOS the editor runs on: the engine's (engine/CMakeLists.txt), the Xcode
# project's and the bundle's LSMinimumSystemVersion. project() reads it, so it comes first;
# without it the editor is built for the build machine's macOS only.
if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version of the editor")
endif()

project(jasperengine-editor)

# Build options of our own targets (docs/testing.md), shared with the standalone engine tree:
# sanitizers (before any target: they apply to the whole tree), warnings (on here; off in
# engine/, which exported games build), warnings as errors, unity builds
option(JASPERENGINE_WARNINGS "Warnings on the engine, editor and test targets" ON)
include(${CMAKE_CURRENT_SOURCE_DIR}/engine/cmake/JasperSanitizers.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/engine/cmake/JasperWarnings.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/engine/cmake/JasperUnityBuild.cmake)
# precompiled headers and the linker of development and test builds (docs/testing.md "Build speed")
include(${CMAKE_CURRENT_SOURCE_DIR}/engine/cmake/JasperPch.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/engine/cmake/JasperLinker.cmake)
# (the editor tree ships no game: a Release editor keeps the default linker, AUTO picks lld for
# Debug and RelWithDebInfo; set before any target, engine/ and the vendored libraries included)
jasperengine_select_linker(OFF)

# git describe --tags --match 'v[0-9]*' --always --dirty.
# Only version tags count, so the doriax-base fork-point tag never names a build.
# JASPERENGINE_EDITOR_VERSION overrides it; otherwise ask git here.
if(NOT JASPERENGINE_EDITOR_VERSION)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --match "v[0-9]*" --always --dirty
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE JASPERENGINE_EDITOR_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
endif()

# Git allows characters that would break the generated C++ and .rc string literals
string(REGEX REPLACE "[^A-Za-z0-9._+-]" "" JASPERENGINE_EDITOR_VERSION "${JASPERENGINE_EDITOR_VERSION}")

if(NOT JASPERENGINE_EDITOR_VERSION)
    set(JASPERENGINE_EDITOR_VERSION "unknown")
endif()

# Windows VERSIONINFO wants four integers, which only a vX.Y[.Z] tag can give.
set(JASPERENGINE_VERSION_NUMERIC "0,0,0,0")
if(JASPERENGINE_EDITOR_VERSION MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    set(JASPERENGINE_VERSION_NUMERIC "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0")
elseif(JASPERENGINE_EDITOR_VERSION MATCHES "^v?([0-9]+)\\.([0-9]+)$")
    set(JASPERENGINE_VERSION_NUMERIC "${CMAKE_MATCH_1},${CMAKE_MATCH_2},0,0")
endif()

message(STATUS "JasperEngine version is ${JASPERENGINE_EDITOR_VERSION}")

if(APPLE)
    enable_language(OBJCXX)
endif()

set(JASPERENGINE_SHARED ON)

# The texture cook's encoders (engine/libs/{bc7enc_rdo,astcenc,basisu}, S14): the editor only,
# games never link them
set(JASPERENGINE_TEXTURE_ENCODERS ON)

# The editor builds, exports and plays every shader through the engine's shader
# compiler (engine/libs/shadercompiler, ShaderVariantCompiler)
set(JASPERENGINE_RUNTIME_SHADERS ON CACHE BOOL "Compile missing shader variants at runtime" FORCE)

# Each desktop OS is served by its own native backend and nothing else, so this
# is a detection rather than a choice.
if(NOT API_BACKEND)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(API_BACKEND "linux")
    elseif(WIN32)
        set(API_BACKEND "win")
    elseif(APPLE)
        set(API_BACKEND "mac")
    else()
        message(FATAL_ERROR "No native editor backend for ${CMAKE_SYSTEM_NAME} (expected Linux, Windows, or macOS)")
    endif()
endif()
message(STATUS "API backend is set to ${API_BACKEND}")

# The native Linux and Windows backends render through Vulkan or OpenGL; macOS
# is tied to Metal.
if(API_BACKEND STREQUAL "linux" OR API_BACKEND STREQUAL "win")
    # Cached so a reconfigure keeps the backend the build directory was made with
    set(GRAPHIC_BACKEND "glcore" CACHE STRING "Editor graphic backend: vulkan or glcore")
    if(NOT GRAPHIC_BACKEND STREQUAL "vulkan" AND NOT GRAPHIC_BACKEND STREQUAL "glcore")
        message(FATAL_ERROR "API_BACKEND=${API_BACKEND} supports GRAPHIC_BACKEND vulkan or glcore")
    endif()
else()
    set(GRAPHIC_BACKEND "metal")
endif()
message(STATUS "Graphic backend is set to ${GRAPHIC_BACKEND}")

# Only macOS keeps the menu bar outside the window, so it is the one platform
# where a native menu is worth its own implementation. ImGui draws it elsewhere.
if(NOT DEFINED NATIVE_MENU)
    if(API_BACKEND STREQUAL "mac")
        set(NATIVE_MENU ON)
    else()
        set(NATIVE_MENU OFF)
    endif()
endif()
message(STATUS "Native menu is set to ${NATIVE_MENU}")

# GLVND links libOpenGL.so.0, which lives in libopengl0 and is absent from a stock
# desktop. libGL.so.1 ships with any driver, and is what the engine already links.
set(OpenGL_GL_PREFERENCE LEGACY)

if(WIN32)
    enable_language(RC)

    set(PLATFORM_EXEC_FLAGS WIN32)
 
    if(MSVC)
        set(PLATFORM_PROPERTIES_FLAGS LINK_FLAGS "/ENTRY:mainCRTStartup")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    elseif(MINGW)
        # Same call as the CRT above: a shared engine means one GCC runtime for
        # the editor, the DLL and the plugins, shipped beside the binaries.
        get_filename_component(JASPERENGINE_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
        find_file(JASPERENGINE_MINGW_STDCXX libstdc++-6.dll PATHS ${JASPERENGINE_MINGW_BIN} NO_DEFAULT_PATH)
        find_file(JASPERENGINE_MINGW_PTHREAD libwinpthread-1.dll PATHS ${JASPERENGINE_MINGW_BIN} NO_DEFAULT_PATH)
        # The unwind DLL is named after the toolchain's exception model
        find_file(JASPERENGINE_MINGW_GCC NAMES libgcc_s_seh-1.dll libgcc_s_dw2-1.dll libgcc_s_sjlj-1.dll
                  PATHS ${JASPERENGINE_MINGW_BIN} NO_DEFAULT_PATH)

        if(JASPERENGINE_MINGW_STDCXX AND JASPERENGINE_MINGW_PTHREAD AND JASPERENGINE_MINGW_GCC)
            set(JASPERENGINE_MINGW_RUNTIME ${JASPERENGINE_MINGW_STDCXX} ${JASPERENGINE_MINGW_PTHREAD} ${JASPERENGINE_MINGW_GCC})
        else()
            message(WARNING "Incomplete GCC runtime next to ${CMAKE_CXX_COMPILER}: the editor will only start from a shell that has it on PATH.")
        endif()
    endif()
endif()

if(APPLE)
    set(PLATFORM_EXEC_FLAGS MACOSX_BUNDLE)
endif()

set(IMGUI_DIR libs/imgui)
set(EDITOR_DIR editor)
set(JASPERENGINE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/engine)

# The compiler floors (the engine checks them again for its standalone builds)
include(${JASPERENGINE_ROOT}/cmake/JasperEngineToolchain.cmake)
jasperengine_check_cxx_toolchain()

# Warnings from vendored code are not ours to fix and only hide our own
if(MSVC)
    set(EXTERNAL_WARNING_FLAG /w)
else()
    set(EXTERNAL_WARNING_FLAG -w)
endif()

# Icon and version resource for one binary. Each target needs its own so
# OriginalFilename matches the file on disk. SignPath wants metadata on every
# signed binary, so the DLL gets one too: pass an empty icon_name for it.
function(jasperengine_add_windows_resource out_var target_name icon_name original_filename description)
    set(JASPERENGINE_ORIGINAL_FILENAME ${original_filename})
    set(JASPERENGINE_FILE_DESCRIPTION ${description})
    set(JASPERENGINE_INTERNAL_NAME ${target_name})

    if(icon_name)
        set(JASPERENGINE_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${EDITOR_DIR}/resources/icons/${icon_name}")
        set(JASPERENGINE_ICON_RESOURCE "GLFW_ICON ICON \"${JASPERENGINE_ICON_FILE}\"")
    else()
        set(JASPERENGINE_ICON_RESOURCE "")
    endif()

    if(original_filename MATCHES "\\.dll$")
        set(JASPERENGINE_FILE_TYPE "0x2L     // VFT_DLL")
    else()
        set(JASPERENGINE_FILE_TYPE "0x1L     // VFT_APP")
    endif()

    set(generated_rc "${CMAKE_CURRENT_BINARY_DIR}/resources/${target_name}.rc")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/${EDITOR_DIR}/resources/icons/jasperengine-editor.rc.in"
        ${generated_rc}
        @ONLY
    )
    if(icon_name)
        set_property(SOURCE ${generated_rc} APPEND PROPERTY OBJECT_DEPENDS ${JASPERENGINE_ICON_FILE})
    endif()

    add_library(${target_name}-resource OBJECT ${generated_rc})
    # rc.exe chokes on the editor's include list, and the .rc needs no search path
    set_property(TARGET ${target_name}-resource PROPERTY INCLUDE_DIRECTORIES "")

    set(${out_var} $<TARGET_OBJECTS:${target_name}-resource> PARENT_SCOPE)
endfunction()

if(WIN32)
    include(${JASPERENGINE_ROOT}/platform/win/WindowsManifest.cmake)
endif()

set(JASPERENGINE_EDITOR_ICON_OBJECTS)
if(WIN32)
    jasperengine_add_windows_resource(
        JASPERENGINE_EDITOR_ICON_OBJECTS
        jasperengine-editor
        "jasperengine-editor.ico"
        "jasperengine-editor.exe"
        "JasperEngine Editor"
    )
endif()

# Linux has no .rc or .icns equivalent, so the icon reaches the desktop as theme
# files (installed at the end of this file) plus the _NET_WM_ICON pixels below.
set(JASPERENGINE_LINUX_ICON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${EDITOR_DIR}/resources/icons/linux")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(wm_icon_header "// Generated from editor/resources/icons/linux. Do not edit.\n")
    string(APPEND wm_icon_header "#pragma once\n\n")
    set(wm_icon_entries "")

    # Sizes a window manager or dock actually draws, up to a HiDPI dock's 256.
    # 512 is left out: on the wire it alone would outweigh the whole set.
    foreach(icon_size IN ITEMS 16 32 48 64 128 256)
        set(icon_png "${JASPERENGINE_LINUX_ICON_DIR}/jasperengine-editor-${icon_size}.png")
        file(READ "${icon_png}" icon_hex HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," icon_bytes "${icon_hex}")
        string(APPEND wm_icon_header "static const unsigned char jasperengine_wm_icon_${icon_size}[] = {${icon_bytes}};\n\n")
        string(APPEND wm_icon_entries "    { jasperengine_wm_icon_${icon_size}, sizeof(jasperengine_wm_icon_${icon_size}) },\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${icon_png}")
    endforeach()

    string(APPEND wm_icon_header "struct JasperEngineWmIconPng { const unsigned char* data; unsigned int size; };\n\n")
    string(APPEND wm_icon_header "static const JasperEngineWmIconPng jasperengine_wm_icon_pngs[] = {\n${wm_icon_entries}};\n")

    # Staged through a .in because configure_file only rewrites on a real change.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/resources/jasperengine_wm_icon.h.in" "${wm_icon_header}")
    configure_file(
        "${CMAKE_CURRENT_BINARY_DIR}/resources/jasperengine_wm_icon.h.in"
        "${CMAKE_CURRENT_BINARY_DIR}/resources/jasperengine_wm_icon.h"
        COPYONLY
    )
    include_directories("${CMAKE_CURRENT_BINARY_DIR}/resources")
endif()

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(BACKEND_SOURCES)
set(BACKEND_LIBS)

if(API_BACKEND STREQUAL "linux")
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "API_BACKEND=linux is only available on Linux")
    endif()
    find_package(X11 REQUIRED COMPONENTS Xrandr)

    list(APPEND BACKEND_SOURCES
        ${EDITOR_DIR}/backend/BackendLinux.cpp
        ${JASPERENGINE_ROOT}/platform/linux/WindowLinux.cpp
        ${JASPERENGINE_ROOT}/platform/linux/GamepadDB.cpp
        ${JASPERENGINE_ROOT}/platform/linux/HostServicesLinux.cpp
    )
    list(APPEND BACKEND_LIBS X11::X11 X11::Xrandr)

    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        find_package(Vulkan REQUIRED)
        list(APPEND BACKEND_LIBS Vulkan::Vulkan)
        add_definitions("-DVK_USE_PLATFORM_XLIB_KHR")
    else()
        find_package(OpenGL REQUIRED)
    endif()
elseif(API_BACKEND STREQUAL "win")
    if(NOT WIN32)
        message(FATAL_ERROR "API_BACKEND=win is only available on Windows")
    endif()
    list(APPEND BACKEND_SOURCES
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${EDITOR_DIR}/backend/BackendWin.cpp
        ${JASPERENGINE_ROOT}/platform/win/WindowWin.cpp
        ${JASPERENGINE_ROOT}/platform/win/GamepadWin.cpp
        ${JASPERENGINE_ROOT}/platform/win/HostServicesWin.cpp
    )

    list(APPEND BACKEND_LIBS
        user32
        gdi32
        dwmapi
        shell32
        ole32
        uuid
        advapi32
    )

    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        find_package(Vulkan REQUIRED)
        list(APPEND BACKEND_LIBS Vulkan::Vulkan)
        add_definitions("-DVK_USE_PLATFORM_WIN32_KHR")
    else()
        list(APPEND BACKEND_LIBS opengl32)
    endif()
elseif(API_BACKEND STREQUAL "mac")
    if(NOT APPLE)
        message(FATAL_ERROR "API_BACKEND=mac is only available on macOS")
    endif()

    list(APPEND BACKEND_SOURCES
        ${IMGUI_DIR}/backends/imgui_impl_osx.mm
        ${EDITOR_DIR}/backend/BackendMac.mm
        ${JASPERENGINE_ROOT}/platform/mac/WindowMac.mm
        ${JASPERENGINE_ROOT}/platform/mac/HostServicesMac.mm
        ${JASPERENGINE_ROOT}/platform/apple/JasperEngineGameController.mm
    )

    list(APPEND BACKEND_LIBS
        "-framework AppKit"
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework GameController"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
    )

    set_source_files_properties(
        ${IMGUI_DIR}/backends/imgui_impl_osx.mm
        ${IMGUI_DIR}/backends/imgui_impl_metal.mm
        ${EDITOR_DIR}/backend/BackendMac.mm
        ${JASPERENGINE_ROOT}/platform/mac/WindowMac.mm
        ${JASPERENGINE_ROOT}/platform/mac/HostServicesMac.mm
        ${EDITOR_DIR}/util/GraphicUtils.cpp
        ${JASPERENGINE_ROOT}/platform/apple/JasperEngineGameController.mm
        PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
    set_source_files_properties(
        ${EDITOR_DIR}/util/GraphicUtils.cpp
        PROPERTIES LANGUAGE OBJCXX
    )
else()
    message(FATAL_ERROR "Unknown API_BACKEND '${API_BACKEND}' (expected linux, win, or mac)")
endif()

# The native backends share the frame loop and pick a renderer by GRAPHIC_BACKEND
if(API_BACKEND STREQUAL "linux" OR API_BACKEND STREQUAL "win")
    list(APPEND BACKEND_SOURCES ${EDITOR_DIR}/EditorFrame.cpp)
    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        list(APPEND BACKEND_SOURCES ${EDITOR_DIR}/backend/renderer/RendererVulkan.cpp)
    else()
        list(APPEND BACKEND_SOURCES ${EDITOR_DIR}/backend/renderer/RendererGL.cpp)
    endif()
endif()

add_definitions("-DJASPERENGINE_EDITOR")
# ImGui reports the items it draws through its test-engine hooks (editor/util/UiItemLocator.cpp,
# MCP find_ui_item); they run only while a lookup records
add_definitions("-DIMGUI_ENABLE_TEST_ENGINE")
# 32-bit characters: a character beyond U+FFFF (emoji, rare CJK) typed or pasted into an editor
# field stays itself instead of becoming U+FFFD (docs/editor/text-and-languages.md)
add_definitions("-DIMGUI_USE_WCHAR32")
add_definitions("-DJASPERENGINE_CRASH_GUARD")

add_definitions("-DNO_LUA_INIT")
if(GRAPHIC_BACKEND STREQUAL "vulkan")
    add_definitions("-DSOKOL_VULKAN")
elseif(GRAPHIC_BACKEND STREQUAL "metal")
    add_definitions("-DSOKOL_METAL")
else()
    add_definitions("-DSOKOL_GLCORE")
endif()
if(NATIVE_MENU)
    add_definitions("-DJASPERENGINE_NATIVE_MENU")
endif()
add_definitions("-DWITH_MINIAUDIO") # For SoLoud
set(NFD_PORTAL ON CACHE BOOL "Use xdg-desktop-portal instead of GTK") # For NFD
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "Enable parse tools" FORCE) # For yaml-cpp

set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/${EDITOR_DIR}/EditorVersion.h.in"
    "${GENERATED_DIR}/EditorVersion.h"
    @ONLY
)

# MCP agent reference docs (docs/mcp/guide/*.md, docs/unity-glossary.md) embedded in the editor
# and served as jasper://docs/<name> (editor/mcp/tools/DocsTools.cpp)
file(GLOB MCP_GUIDE_DOCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/docs/mcp/guide/*.md")
list(APPEND MCP_GUIDE_DOCS "${CMAKE_CURRENT_SOURCE_DIR}/docs/unity-glossary.md")
set(mcp_docs_header "// Generated from docs/mcp/guide by CMakeLists.txt. Do not edit.\n#pragma once\n\n")
set(mcp_docs_entries "")
foreach(mcp_doc IN LISTS MCP_GUIDE_DOCS)
    get_filename_component(mcp_doc_name "${mcp_doc}" NAME_WE)
    string(MAKE_C_IDENTIFIER "${mcp_doc_name}" mcp_doc_id)
    file(READ "${mcp_doc}" mcp_doc_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," mcp_doc_bytes "${mcp_doc_hex}")
    string(APPEND mcp_docs_header "static const unsigned char mcp_doc_${mcp_doc_id}[] = {${mcp_doc_bytes}0x00};\n")
    string(APPEND mcp_docs_entries "    { \"${mcp_doc_name}\", mcp_doc_${mcp_doc_id}, sizeof(mcp_doc_${mcp_doc_id}) - 1 },\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${mcp_doc}")
endforeach()
string(APPEND mcp_docs_header "\nstruct McpEmbeddedDoc { const char* name; const unsigned char* data; unsigned int size; };\n")
string(APPEND mcp_docs_header "static const McpEmbeddedDoc mcp_embedded_docs[] = {\n${mcp_docs_entries}};\n")
file(WRITE "${GENERATED_DIR}/mcp_docs.h.in" "${mcp_docs_header}")
configure_file("${GENERATED_DIR}/mcp_docs.h.in" "${GENERATED_DIR}/mcp_docs.h" COPYONLY)

# Built-in pose animation templates (editor/resources/pose-templates/*.poseanim) embedded in the
# editor (editor/pose/PoseTemplatesBuiltin.cpp, the only includer)
file(GLOB POSE_TEMPLATE_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${EDITOR_DIR}/resources/pose-templates/*.poseanim")
list(SORT POSE_TEMPLATE_FILES)
# The sound essentials (40 CC0 clips and their manifest, editor/resources/sound-library/essentials)
# embedded in the editor (editor/audio/SoundEssentials.cpp). The generator fails the build when a
# clip's size or SHA-256 disagrees with the manifest.
set(SOUND_ESSENTIALS_DIR "${CMAKE_SOURCE_DIR}/editor/resources/sound-library/essentials")
set(SOUND_ESSENTIALS_HEADER "${GENERATED_DIR}/sound_essentials.h")
set(EMBED_ESSENTIALS_SCRIPT "${CMAKE_SOURCE_DIR}/tools/sound-library/embed_essentials.py")
file(GLOB_RECURSE SOUND_ESSENTIALS_CLIPS CONFIGURE_DEPENDS "${SOUND_ESSENTIALS_DIR}/*.ogg")
add_custom_command(
    OUTPUT ${SOUND_ESSENTIALS_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${EMBED_ESSENTIALS_SCRIPT} ${SOUND_ESSENTIALS_DIR} ${SOUND_ESSENTIALS_HEADER}
    DEPENDS ${EMBED_ESSENTIALS_SCRIPT} ${SOUND_ESSENTIALS_DIR}/manifest.json ${SOUND_ESSENTIALS_CLIPS}
    COMMENT "Embedding the sound essentials (sound_essentials.h)"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

set(pose_templates_header "// Generated from editor/resources/pose-templates by CMakeLists.txt. Do not edit.\n#pragma once\n\n")
set(pose_templates_entries "")
set(pose_template_index 0)
foreach(pose_template IN LISTS POSE_TEMPLATE_FILES)
    get_filename_component(pose_template_name "${pose_template}" NAME)
    file(READ "${pose_template}" pose_template_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," pose_template_bytes "${pose_template_hex}")
    string(APPEND pose_templates_header "static const unsigned char pose_template_${pose_template_index}[] = {${pose_template_bytes}0x00};\n")
    string(APPEND pose_templates_entries "    { \"${pose_template_name}\", pose_template_${pose_template_index}, sizeof(pose_template_${pose_template_index}) - 1 },\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pose_template}")
    math(EXPR pose_template_index "${pose_template_index} + 1")
endforeach()
string(APPEND pose_templates_header "\nstruct PoseTemplateEmbeddedFile { const char* name; const unsigned char* data; unsigned int size; };\n")
string(APPEND pose_templates_header "static const PoseTemplateEmbeddedFile pose_template_embedded_files[] = {\n${pose_templates_entries}    { nullptr, nullptr, 0 }\n};\n")
file(WRITE "${GENERATED_DIR}/pose_templates.h.in" "${pose_templates_header}")
configure_file("${GENERATED_DIR}/pose_templates.h.in" "${GENERATED_DIR}/pose_templates.h" COPYONLY)

# HTML/CSS UI example library (docs/mcp/guide/html-ui-examples/**) embedded in the editor: written
# into every project's .jasperengine/engine-api/html-ui-examples, served as MCP resources and used
# as create_ui_document templates (editor/util/HtmlUiExamples.cpp, the only includer)
set(HTML_UI_EXAMPLES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/docs/mcp/guide/html-ui-examples")
file(GLOB_RECURSE HTML_UI_EXAMPLE_FILES CONFIGURE_DEPENDS "${HTML_UI_EXAMPLES_DIR}/*")
list(SORT HTML_UI_EXAMPLE_FILES)
set(html_ui_examples_header "// Generated from docs/mcp/guide/html-ui-examples by CMakeLists.txt. Do not edit.\n#pragma once\n\n")
set(html_ui_examples_entries "")
set(html_ui_example_index 0)
foreach(html_ui_example IN LISTS HTML_UI_EXAMPLE_FILES)
    file(RELATIVE_PATH html_ui_example_path "${HTML_UI_EXAMPLES_DIR}" "${html_ui_example}")
    file(READ "${html_ui_example}" html_ui_example_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," html_ui_example_bytes "${html_ui_example_hex}")
    string(APPEND html_ui_examples_header "static const unsigned char html_ui_example_${html_ui_example_index}[] = {${html_ui_example_bytes}0x00};\n")
    string(APPEND html_ui_examples_entries "    { \"${html_ui_example_path}\", html_ui_example_${html_ui_example_index}, sizeof(html_ui_example_${html_ui_example_index}) - 1 },\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${html_ui_example}")
    math(EXPR html_ui_example_index "${html_ui_example_index} + 1")
endforeach()
string(APPEND html_ui_examples_header "\nstruct HtmlUiEmbeddedFile { const char* path; const unsigned char* data; unsigned int size; };\n")
string(APPEND html_ui_examples_header "static const HtmlUiEmbeddedFile html_ui_embedded_files[] = {\n${html_ui_examples_entries}};\n")
file(WRITE "${GENERATED_DIR}/html_ui_examples.h.in" "${html_ui_examples_header}")
configure_file("${GENERATED_DIR}/html_ui_examples.h.in" "${GENERATED_DIR}/html_ui_examples.h" COPYONLY)

# Font Awesome icons by name ("magnet" -> ICON_FA_MAGNET): the toolbar buttons project editor
# scripts declare (---@toolbar Label icon=magnet), editor/util/EditorScripts
set(FONT_AWESOME_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/${EDITOR_DIR}/external/IconsFontAwesome6.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FONT_AWESOME_HEADER}")
file(STRINGS "${FONT_AWESOME_HEADER}" font_awesome_defines REGEX "^#define ICON_FA_[A-Z0-9_]+ ")
set(font_awesome_names "// Generated by CMake from IconsFontAwesome6.h. Do not edit.\n#pragma once\n#include \"external/IconsFontAwesome6.h\"\n")
string(APPEND font_awesome_names "struct FontAwesomeName { const char* name; const char* glyph; };\nstatic const FontAwesomeName fontAwesomeNames[] = {\n")
foreach(define IN LISTS font_awesome_defines)
    # (a glyph that is ';' splits its line in two list items: only the first one matches)
    if(define MATCHES "^#define ICON_FA_([A-Z0-9_]+) ")
        set(macro "${CMAKE_MATCH_1}")
        string(TOLOWER "${macro}" icon_name)
        string(REPLACE "_" "-" icon_name "${icon_name}")
        string(APPEND font_awesome_names "    {\"${icon_name}\", ICON_FA_${macro}},\n")
    endif()
endforeach()
string(APPEND font_awesome_names "};\n")
file(WRITE "${GENERATED_DIR}/font_awesome_names.h.in" "${font_awesome_names}")
configure_file("${GENERATED_DIR}/font_awesome_names.h.in" "${GENERATED_DIR}/font_awesome_names.h" COPYONLY)

# The engine's Lua API, described once (tools/lua_api, S23 R29): one parser reads the bindings
# and the engine headers into lua-api.json, and three emitters read only that JSON: the LuaLS
# stubs the editor writes into every project (.jasperengine/engine-api/lua, see
# Generator::writeEditorTooling; the .lua files are also left in the build tree), the code
# editor's completion header and the Markdown reference (with the JSON, in lua_api_data.h: the
# jasper://lua-api resource and `doc lua-api-reference`). Rebuilt only when the bindings, the
# engine headers, the stub fragments or tools/lua_api change.
set(BINDING_DIR "${CMAKE_SOURCE_DIR}/engine/core/script/binding")
set(ENGINE_CORE_DIR "${CMAKE_SOURCE_DIR}/engine/core")
set(API_SUGGESTIONS_HEADER "${GENERATED_DIR}/engine_api_suggestions.h")
set(LUA_STUBS_HEADER "${GENERATED_DIR}/engine_lua_stubs.h")
set(LUA_STUBS_DIR "${GENERATED_DIR}/engine-api-lua")
set(LUA_API_JSON "${GENERATED_DIR}/lua-api.json")
set(LUA_API_DATA_HEADER "${GENERATED_DIR}/lua_api_data.h")
set(LUA_API_MARKDOWN "${GENERATED_DIR}/lua-api-reference.md")

file(GLOB BINDING_FILES CONFIGURE_DEPENDS "${BINDING_DIR}/*.cpp")
# hand-written LuaLS fragments appended to the generated stubs (parameter-table shapes ...)
file(GLOB BINDING_STUB_FRAGMENTS CONFIGURE_DEPENDS "${BINDING_DIR}/stubs/*.lua")
file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS "${ENGINE_CORE_DIR}/*.h")
file(GLOB LUA_API_SCRIPTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/lua_api/*.py")

add_custom_command(
    OUTPUT ${LUA_API_JSON} ${LUA_STUBS_HEADER} ${API_SUGGESTIONS_HEADER} ${LUA_API_DATA_HEADER}
    COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${CMAKE_SOURCE_DIR}/tools"
            ${Python3_EXECUTABLE} -m lua_api all
            --binding-dir ${BINDING_DIR} --core-dir ${ENGINE_CORE_DIR} --json ${LUA_API_JSON}
            --stubs-header ${LUA_STUBS_HEADER} --stubs-dir ${LUA_STUBS_DIR}
            --suggestions-header ${API_SUGGESTIONS_HEADER}
            --markdown ${LUA_API_MARKDOWN} --data-header ${LUA_API_DATA_HEADER}
    DEPENDS ${LUA_API_SCRIPTS} ${BINDING_FILES} ${BINDING_STUB_FRAGMENTS} ${ENGINE_HEADERS}
    COMMENT "Describing the Lua API (lua-api.json) and writing its stubs, completion and reference"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

include_directories (${CMAKE_CURRENT_SOURCE_DIR})

include_directories (SYSTEM ${JASPERENGINE_ROOT}/libs/sokol)
include_directories (SYSTEM ${JASPERENGINE_ROOT}/libs/json)
include_directories (SYSTEM ${JASPERENGINE_ROOT}/libs/tinygltf)
# the native plugins export checks XML fragments (util/NativeExportAndroid.cpp, NativeExportApple.cpp)
include_directories (SYSTEM ${JASPERENGINE_ROOT}/libs/tinyxml2)
include_directories (SYSTEM ${JASPERENGINE_ROOT}/libs/lua)
include_directories (SYSTEM ${JASPERENGINE_ROOT}/libs/luabridge3)
include_directories (SYSTEM ${JASPERENGINE_ROOT}/libs/box2d/include)
include_directories (SYSTEM ${JASPERENGINE_ROOT}/libs/joltphysics)
include_directories (SYSTEM ${JASPERENGINE_ROOT}/libs/stb)
include_directories (SYSTEM ${JASPERENGINE_ROOT}/libs/tinygltf)
include_directories (SYSTEM ${JASPERENGINE_ROOT}/libs/harfbuzz)
include_directories (SYSTEM ${JASPERENGINE_ROOT}/libs/sheenbidi/Headers)
include_directories (SYSTEM ${JASPERENGINE_ROOT}/libs/soloud/include)

include_directories (${JASPERENGINE_ROOT}/core)
include_directories (${JASPERENGINE_ROOT}/core/action)
include_directories (${JASPERENGINE_ROOT}/core/buffer)
include_directories (${JASPERENGINE_ROOT}/core/component)
include_directories (${JASPERENGINE_ROOT}/core/ecs)
include_directories (${JASPERENGINE_ROOT}/core/io)
include_directories (${JASPERENGINE_ROOT}/core/io/watch)
include_directories (${JASPERENGINE_ROOT}/core/manager)
include_directories (${JASPERENGINE_ROOT}/core/math)
include_directories (${JASPERENGINE_ROOT}/core/object)
include_directories (${JASPERENGINE_ROOT}/core/object/sound)
include_directories (${JASPERENGINE_ROOT}/core/object/ui)
include_directories (${JASPERENGINE_ROOT}/core/object/environment)
include_directories (${JASPERENGINE_ROOT}/core/object/physics)
include_directories (${JASPERENGINE_ROOT}/core/pool)
include_directories (${JASPERENGINE_ROOT}/core/registry)
include_directories (${JASPERENGINE_ROOT}/core/render)
include_directories (${JASPERENGINE_ROOT}/core/script)
include_directories (${JASPERENGINE_ROOT}/core/shader)
include_directories (${JASPERENGINE_ROOT}/core/subsystem)
include_directories (${JASPERENGINE_ROOT}/core/texture)
# Keep util/ namespaced: Signal.h shadows <signal.h> on case-insensitive hosts.
# GamepadMappings.h, shared by GamepadDB (editor) and GamepadLinux (games)
include_directories (${JASPERENGINE_ROOT}/platform/common)
# renderer/Renderer.h, which EditorFrame.h includes from one directory up
include_directories (${EDITOR_DIR}/backend)
if(API_BACKEND STREQUAL "linux")
    include_directories (${JASPERENGINE_ROOT}/platform/linux) # WindowLinux.h, GamepadDB.h
elseif(API_BACKEND STREQUAL "win")
    include_directories (${JASPERENGINE_ROOT}/platform/win) # GamepadWin.h for BackendWin
elseif(API_BACKEND STREQUAL "mac")
    # WindowMac.h for BackendMac, JasperEngineGameController.h from the shared Apple path
    include_directories (${JASPERENGINE_ROOT}/platform/mac)
    include_directories (${JASPERENGINE_ROOT}/platform/apple)
endif()
include_directories (${JASPERENGINE_ROOT}/renders)

# cl's /MP for the engine's and the editor's targets under Visual Studio generators
set(JASPERENGINE_PARALLEL_BUILD_JOBS "" CACHE STRING "Parallel compile job limit of Visual Studio builds (/MP); empty or 0: every CPU")
jasperengine_msvc_parallel_compile()

add_subdirectory(${JASPERENGINE_ROOT})

# Signed alongside the exes, so it needs the same metadata. MinGW prefixes the name.
if(WIN32)
    jasperengine_add_windows_resource(
        JASPERENGINE_LIB_ICON_OBJECTS
        jasperengine
        ""
        "${CMAKE_SHARED_LIBRARY_PREFIX}jasperengine${CMAKE_SHARED_LIBRARY_SUFFIX}"
        "JasperEngine Runtime"
    )
    target_sources(jasperengine PRIVATE ${JASPERENGINE_LIB_ICON_OBJECTS})
endif()

# The engine compiles sokol's Vulkan backend, so it needs the headers and the
# loader itself. Only the standalone engine build sets this up on its own.
if(GRAPHIC_BACKEND STREQUAL "vulkan")
    target_link_libraries(jasperengine PUBLIC Vulkan::Vulkan)
endif()

# The engine directory set the standard for its own targets; the editor's, and the
# libraries below (which pick a standard of their own when none is set), use it too
set(CMAKE_CXX_STANDARD ${JASPERENGINE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD ${JASPERENGINE_CXX_STANDARD})
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)
endif()

add_subdirectory(libs/nfd)
add_subdirectory(libs/yaml-cpp)
add_subdirectory(libs/ufbx)
add_subdirectory(libs/tinyexr)
# The model decoders games never carry (docs/specs/S53-model-import-cook.md R6): Draco meshes and
# WebP images of glTF files, which the model cook transcodes
add_subdirectory(editor/libs/draco)
add_subdirectory(editor/libs/libwebp)
jasperengine_apply_cxx_standard(nfd yaml-cpp tinyexr draco_decoder)
# The standards the project setting offers (editor/util/CxxStandards.h) and the compiler floors
# the Play plugin checks (Generator::getToolchainFloorCheck), from the engine's definitions
get_directory_property(JASPERENGINE_CXX_STANDARDS DIRECTORY ${JASPERENGINE_ROOT} DEFINITION JASPERENGINE_CXX_STANDARDS)
string(REPLACE ";" "," JASPERENGINE_CXX_STANDARD_LIST "${JASPERENGINE_CXX_STANDARDS}")

foreach(lib IN ITEMS nfd yaml-cpp ufbx tinyexr draco_decoder webp_decoder)
    target_compile_options(${lib} PRIVATE ${EXTERNAL_WARNING_FLAG})
    # Reaches the headers they export to the editor, which ImGui gets below
    get_target_property(lib_includes ${lib} INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(${lib} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${lib_includes}")
endforeach()

# Add ImGui source files
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)

# ImGui platform backends go with API_BACKEND above, renderers with GRAPHIC_BACKEND
if(GRAPHIC_BACKEND STREQUAL "vulkan")
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
elseif(GRAPHIC_BACKEND STREQUAL "metal")
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_metal.mm)
else()
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
endif()

# tinyexpr evaluates the expressions typed into numeric fields (editor/util/NumericExpression):
# x^y^z is x^(y^z) and log is the natural logarithm, as in Unity and most calculators
set(TINYEXPR_SOURCES libs/tinyexpr/tinyexpr.c)
set_property(SOURCE ${TINYEXPR_SOURCES} APPEND PROPERTY COMPILE_OPTIONS ${EXTERNAL_WARNING_FLAG})
set_property(SOURCE ${TINYEXPR_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS TE_POW_FROM_RIGHT TE_NAT_LOG)

# miniz reads and writes the zip archives of the prebuilt players and of Android packages
# (editor/players, editor/android; docs/specs/S30-shipping.md D8). Editor only: players never link it.
set(MINIZ_SOURCES libs/miniz/miniz.c)
set_property(SOURCE ${MINIZ_SOURCES} APPEND PROPERTY COMPILE_OPTIONS ${EXTERNAL_WARNING_FLAG})
set_property(SOURCE ${MINIZ_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

# ImGui has no target of its own; only one of these backends builds per platform
set_property(
    SOURCE
        ${IMGUI_SOURCES}
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_osx.mm
    APPEND PROPERTY COMPILE_OPTIONS ${EXTERNAL_WARNING_FLAG}
)

include_directories (${EDITOR_DIR})

add_custom_target(generate_api_suggestions DEPENDS ${API_SUGGESTIONS_HEADER} ${LUA_STUBS_HEADER} ${LUA_API_DATA_HEADER})
add_custom_target(generate_sound_essentials DEPENDS ${SOUND_ESSENTIALS_HEADER})

# The texture synthesis core (editor/texsynth: Embark's texture-synthesis ported, NOTICE.md),
# tested by jasperengine-editor-tests (editor/tests/texsynth); its CLI is SynthesisCli.cpp
# (CommandLine)
set(JASPERENGINE_TEXSYNTH_SOURCES
    ${EDITOR_DIR}/texsynth/SynthImage.cpp
    ${EDITOR_DIR}/texsynth/Synthesizer.cpp
    ${EDITOR_DIR}/texsynth/SynthesisRecipe.cpp
    ${EDITOR_DIR}/texsynth/TextureSynthesis.cpp
)

# The sprite normal generator core (editor/spritenormals: spritefusion-sprite-normals ported, NOTICE.md),
# also built alone into jasperengine-spritenormals-tests (-DJASPERENGINE_SPRITENORMALS_TESTS=ON)
set(JASPERENGINE_SPRITENORMALS_SOURCES
    ${EDITOR_DIR}/spritenormals/SpriteNormals.cpp
    ${EDITOR_DIR}/spritenormals/SpriteNormalsField.cpp
    ${EDITOR_DIR}/spritenormals/SpriteNormalsInflate.cpp
    ${EDITOR_DIR}/spritenormals/SpriteMedia.cpp
    ${EDITOR_DIR}/spritenormals/SpriteNormalsRecipe.cpp
    ${EDITOR_DIR}/spritenormals/SpriteNormalsPreview.cpp
    ${EDITOR_DIR}/spritenormals/SpriteNormalsPaint.cpp
)
# Upstream parity: Rust never contracts a*b+c into an FMA (the generator calls std::fma itself
# exactly where upstream calls mul_add); the media decoding (resampling, frame compositing) and the
# paint layer's compositing likewise keep their bytes on every target. MSVC: /fp:precise (no
# contraction without /fp:contract); clang-cl (the MSVC front end of Clang, which contracts under
# /fp:precise): Clang's own flag through /clang:; GCC and Clang: -ffp-contract=off
set(JASPERENGINE_SPRITENORMALS_NO_FMA_SOURCES
    ${EDITOR_DIR}/spritenormals/SpriteNormals.cpp
    ${EDITOR_DIR}/spritenormals/SpriteNormalsField.cpp
    ${EDITOR_DIR}/spritenormals/SpriteNormalsInflate.cpp
    ${EDITOR_DIR}/spritenormals/SpriteMedia.cpp
    ${EDITOR_DIR}/spritenormals/SpriteNormalsPaint.cpp
)
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(JASPERENGINE_NO_FMA_FLAGS "/fp:precise")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(JASPERENGINE_NO_FMA_FLAGS "/fp:precise;/clang:-ffp-contract=off")
else()
    set(JASPERENGINE_NO_FMA_FLAGS "-ffp-contract=off")
endif()
set_source_files_properties(${JASPERENGINE_SPRITENORMALS_NO_FMA_SOURCES} PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_NO_FMA_FLAGS}")

set(JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/main.cpp
    ${EDITOR_DIR}/cli/CommandLine.cpp
    ${EDITOR_DIR}/cli/SchemaArgs.cpp
    ${EDITOR_DIR}/cli/McpClient.cpp
    ${EDITOR_DIR}/cli/CallCommand.cpp
    ${EDITOR_DIR}/cli/ShippingCommands.cpp
    ${EDITOR_DIR}/cook/RuntimeFieldsEmitter.cpp
    ${EDITOR_DIR}/cook/SceneCooker.cpp
    ${EDITOR_DIR}/cook/JPakWriter.cpp
    ${EDITOR_DIR}/cook/BuildReport.cpp
    ${EDITOR_DIR}/util/ZipArchive.cpp
    ${EDITOR_DIR}/players/ModuleUsage.cpp
    ${EDITOR_DIR}/players/PlayerRuntimes.cpp
    ${EDITOR_DIR}/players/ShaderNeutralApi.cpp
    ${EDITOR_DIR}/publish/Notices.cpp
    ${EDITOR_DIR}/publish/BinaryInfo.cpp
    ${EDITOR_DIR}/publish/TreeManifest.cpp
    ${EDITOR_DIR}/publish/PayloadValidator.cpp
    ${EDITOR_DIR}/publish/Publishing.cpp
    ${EDITOR_DIR}/platform/ExternalTools.cpp
    ${EDITOR_DIR}/platform/AndroidTools.cpp
    ${EDITOR_DIR}/platform/PinnedDownload.cpp
    ${EDITOR_DIR}/platform/AndroidPackager.cpp
    ${EDITOR_DIR}/platform/WindowsResources.cpp
    ${EDITOR_DIR}/platform/MacBundle.cpp
    ${EDITOR_DIR}/platform/IosProject.cpp
    ${EDITOR_DIR}/App.cpp
    ${EDITOR_DIR}/AppCommands.cpp
    ${EDITOR_DIR}/AppSettings.cpp
    ${EDITOR_DIR}/Theme.cpp
    ${EDITOR_DIR}/Factory.cpp
    ${EDITOR_DIR}/Out.cpp
    ${EDITOR_DIR}/Platform.cpp
    ${EDITOR_DIR}/Project.cpp
    ${EDITOR_DIR}/project/ProjectPrefabEdit.cpp
    ${EDITOR_DIR}/project/ProjectPrefabImport.cpp
    ${EDITOR_DIR}/project/ProjectPrefabRuntime.cpp
    ${EDITOR_DIR}/project/ProjectPrefabInstanceEdit.cpp
    ${EDITOR_DIR}/project/ProjectPrefabMissing.cpp
    ${EDITOR_DIR}/project/ProjectPrefabReplace.cpp
    ${EDITOR_DIR}/project/ProjectPrefabOverrides.cpp
    ${EDITOR_DIR}/project/ProjectPrefabOverrideModel.cpp
    ${EDITOR_DIR}/project/ProjectPrefabPaths.cpp
    ${EDITOR_DIR}/project/ProjectAssets.cpp
    ${EDITOR_DIR}/project/ProjectShaderLists.cpp
    ${EDITOR_DIR}/project/ProjectDataSafety.cpp
    ${EDITOR_DIR}/project/ProjectPrefabVariant.cpp
    ${EDITOR_DIR}/project/ProjectPrefabRefs.cpp
    ${EDITOR_DIR}/Catalog.cpp
    ${EDITOR_DIR}/reflect/Reflect.cpp
    ${EDITOR_DIR}/reflect/TypeOps.cpp
    ${EDITOR_DIR}/reflect/Enums.cpp
    ${EDITOR_DIR}/reflect/Registry.cpp
    ${EDITOR_DIR}/reflect/Resolve.cpp
    ${EDITOR_DIR}/reflect/YamlCodec.cpp
    ${EDITOR_DIR}/reflect/Schema.cpp
    ${EDITOR_DIR}/reflect/Codegen.cpp
    ${EDITOR_DIR}/reflect/Remap.cpp
    ${EDITOR_DIR}/reflect/Properties.cpp
    ${EDITOR_DIR}/reflect/PostProcessingReflect.cpp
    ${EDITOR_DIR}/reflect/ShaderCreationReflect.cpp
    ${EDITOR_DIR}/reflect/DebugReflect.cpp
    ${EDITOR_DIR}/reflect/components/AnimationReflect.cpp
    ${EDITOR_DIR}/reflect/components/CameraAudioReflect.cpp
    ${EDITOR_DIR}/reflect/components/LightingReflect.cpp
    ${EDITOR_DIR}/reflect/components/MaterialReflect.cpp
    ${EDITOR_DIR}/reflect/components/GameplayReflect.cpp
    ${EDITOR_DIR}/reflect/components/ClipPlayersReflect.cpp
    ${EDITOR_DIR}/reflect/components/SplinesReflect.cpp
    ${EDITOR_DIR}/reflect/components/AnimatorReflect.cpp
    ${EDITOR_DIR}/reflect/components/ParticlesReflect.cpp
    ${EDITOR_DIR}/reflect/components/RenderReflect.cpp
    ${EDITOR_DIR}/reflect/components/UpstreamReflect.cpp
    ${EDITOR_DIR}/reflect/components/UpstreamMediaReflect.cpp
    ${EDITOR_DIR}/reflect/components/TransformPhysicsReflect.cpp
    ${EDITOR_DIR}/reflect/components/UIReflect.cpp
    ${EDITOR_DIR}/reflect/components/LatticeFluidReflect.cpp
    ${EDITOR_DIR}/Conector.cpp
    ${EDITOR_DIR}/EditorHost.cpp
    ${EDITOR_DIR}/Generator.cpp
    ${EDITOR_DIR}/DebugSettings.cpp
    ${EDITOR_DIR}/Stream.cpp
    ${EDITOR_DIR}/StreamComponents.cpp
    ${EDITOR_DIR}/StreamUpstream.cpp
    ${EDITOR_DIR}/StreamPrefab.cpp
    ${EDITOR_DIR}/StreamMigrations.cpp
    ${EDITOR_DIR}/stream/UpstreamParticles.cpp
    ${EDITOR_DIR}/StreamSchema.cpp
    ${EDITOR_DIR}/StreamPreserved.cpp
    ${EDITOR_DIR}/LoadReport.cpp
    ${EDITOR_DIR}/DocumentFile.cpp
    ${EDITOR_DIR}/SceneBackup.cpp
    ${EDITOR_DIR}/PrefabStage.cpp
    ${EDITOR_DIR}/Workspace.cpp
    ${EDITOR_DIR}/picking/BvhDiskCache.cpp
    ${EDITOR_DIR}/picking/IconPicker.cpp
    ${EDITOR_DIR}/picking/ScenePicker.cpp
    ${EDITOR_DIR}/sceneview/SceneViewController.cpp
    ${EDITOR_DIR}/sceneview/SceneViewMath.cpp
    ${EDITOR_DIR}/sceneview/SceneViewState.cpp
    ${EDITOR_DIR}/sceneview/SceneViewToolbar.cpp
    ${EDITOR_DIR}/sceneview/SceneViewVisibility.cpp
    ${EDITOR_DIR}/sceneview/SurfacePlacement.cpp
    ${EDITOR_DIR}/window/HierarchyVisibilityGutter.cpp
    ${EDITOR_DIR}/Exporter.cpp
    ${EDITOR_DIR}/ExporterPrebuilt.cpp
    ${EDITOR_DIR}/ShippingTypes.cpp

    ${EDITOR_DIR}/mcp/McpActivityWindow.cpp
    ${EDITOR_DIR}/window/JobsMemoryWindow.cpp
    ${EDITOR_DIR}/window/profiler/FrameDebuggerWindow.cpp
    ${EDITOR_DIR}/window/profiler/ProfilerWindow.cpp
    ${EDITOR_DIR}/mcp/McpAutoSync.cpp
    ${EDITOR_DIR}/mcp/McpCall.cpp
    ${EDITOR_DIR}/mcp/McpDialogs.cpp
    ${EDITOR_DIR}/mcp/McpContext.cpp
    ${EDITOR_DIR}/mcp/McpInstructions.cpp
    ${EDITOR_DIR}/mcp/McpJobs.cpp
    ${EDITOR_DIR}/mcp/McpRefs.cpp
    ${EDITOR_DIR}/mcp/McpRegistry.cpp
    ${EDITOR_DIR}/mcp/McpSchema.cpp
    ${EDITOR_DIR}/mcp/McpToolGroups.cpp
    ${EDITOR_DIR}/mcp/McpInstances.cpp
    ${EDITOR_DIR}/mcp/McpSocket.cpp
    ${EDITOR_DIR}/mcp/McpServer.cpp
    ${EDITOR_DIR}/mcp/McpTransportHttp.cpp
    ${EDITOR_DIR}/mcp/McpTransportStdio.cpp
    ${EDITOR_DIR}/mcp/McpValues.cpp
    ${EDITOR_DIR}/mcp/McpUndo.cpp
    ${EDITOR_DIR}/mcp/McpShaderValues.cpp
    ${EDITOR_DIR}/mcp/McpLaunch.cpp
    ${EDITOR_DIR}/mcp/EngineApiContext.cpp
    ${EDITOR_DIR}/mcp/TerrainMaps.cpp
    ${EDITOR_DIR}/mcp/tools/McpTools.cpp
    ${EDITOR_DIR}/mcp/tools/SoundTools.cpp
    ${EDITOR_DIR}/mcp/tools/CoreTools.cpp
    ${EDITOR_DIR}/mcp/tools/ProjectTools.cpp
    ${EDITOR_DIR}/mcp/tools/ShadowTools.cpp
    ${EDITOR_DIR}/mcp/tools/ProfilerTools.cpp
    ${EDITOR_DIR}/mcp/tools/MeshLodTools.cpp
    ${EDITOR_DIR}/mcp/tools/DeviceTools.cpp
    ${EDITOR_DIR}/mcp/tools/SceneTools.cpp
    ${EDITOR_DIR}/mcp/McpDataSafety.cpp
    ${EDITOR_DIR}/mcp/tools/EntityTools.cpp
    ${EDITOR_DIR}/mcp/tools/ComponentTools.cpp
    ${EDITOR_DIR}/mcp/tools/PrefabTools.cpp
    ${EDITOR_DIR}/mcp/tools/PlayTools.cpp
    ${EDITOR_DIR}/mcp/tools/ViewTools.cpp
    ${EDITOR_DIR}/mcp/tools/AtmosphereTools.cpp
    ${EDITOR_DIR}/mcp/tools/ConsoleTools.cpp
    ${EDITOR_DIR}/mcp/tools/DataTools.cpp
    ${EDITOR_DIR}/mcp/tools/AudioSessionTools.cpp
    ${EDITOR_DIR}/mcp/tools/AudioTools.cpp
    ${EDITOR_DIR}/mcp/tools/ScriptTools.cpp
    ${EDITOR_DIR}/mcp/tools/ScriptingTools.cpp
    ${EDITOR_DIR}/mcp/tools/GraphTools.cpp
    ${EDITOR_DIR}/mcp/tools/ParticleTools.cpp
    ${EDITOR_DIR}/mcp/tools/AssetTools.cpp
    ${EDITOR_DIR}/mcp/tools/MeshTools.cpp
    ${EDITOR_DIR}/mcp/tools/MaterialsLightingTools.cpp
    ${EDITOR_DIR}/mcp/tools/AssetIdentityTools.cpp
    ${EDITOR_DIR}/mcp/tools/DocsTools.cpp
    ${EDITOR_DIR}/mcp/tools/BuildTools.cpp
    ${EDITOR_DIR}/mcp/tools/PublishTools.cpp
    ${EDITOR_DIR}/mcp/tools/WorldTools.cpp
    ${EDITOR_DIR}/mcp/tools/AnimationClipTools.cpp
    ${EDITOR_DIR}/mcp/tools/AnimatorTools.cpp
    ${EDITOR_DIR}/mcp/tools/LatticeTools.cpp
    ${EDITOR_DIR}/mcp/tools/HtmlUiTools.cpp
    ${EDITOR_DIR}/mcp/tools/UIEffectTools.cpp
    ${EDITOR_DIR}/mcp/tools/SynthesisTools.cpp
    ${EDITOR_DIR}/mcp/tools/PoseTools.cpp
    ${EDITOR_DIR}/mcp/tools/PlacementTools.cpp
    ${EDITOR_DIR}/mcp/tools/EcsTools.cpp
    ${EDITOR_DIR}/mcp/tools/SceneViewTools.cpp
    ${EDITOR_DIR}/mcp/tools/GameplayTools.cpp
    ${EDITOR_DIR}/mcp/tools/SplineTools.cpp
    ${EDITOR_DIR}/mcp/tools/DiagnosticsTools.cpp
    ${EDITOR_DIR}/mcp/tools/OrganisationTools.cpp
    ${EDITOR_DIR}/mcp/tools/VatTools.cpp
    ${EDITOR_DIR}/mcp/tools/VcsTools.cpp
    ${EDITOR_DIR}/mcp/tools/SpriteNormalsTools.cpp

    ${EDITOR_DIR}/util/ShaderHeaderBuilder.cpp
    ${EDITOR_DIR}/util/BidiText.cpp
    ${EDITOR_DIR}/util/CrashHandler.cpp
    ${EDITOR_DIR}/util/EditorDiagnostics.cpp
    ${EDITOR_DIR}/util/GraphicUtils.cpp
    ${EDITOR_DIR}/util/CameraTextureLink.cpp
    ${EDITOR_DIR}/util/FluidTextureLink.cpp
    ${EDITOR_DIR}/util/VideoTextureLink.cpp
    ${EDITOR_DIR}/util/FluidEditorTools.cpp
    ${EDITOR_DIR}/util/UIProbe.cpp
    ${EDITOR_DIR}/util/Clipboard.cpp
    ${EDITOR_DIR}/util/EntityClipboard.cpp
    ${EDITOR_DIR}/util/EntityPath.cpp
    ${EDITOR_DIR}/util/ExportFeatureScan.cpp
    ${EDITOR_DIR}/util/NativeExportAndroid.cpp
    ${EDITOR_DIR}/util/CMakeText.cpp
    ${EDITOR_DIR}/util/VersionControlFiles.cpp
    ${EDITOR_DIR}/util/NativePlugins.cpp
    ${EDITOR_DIR}/util/BackgroundRelease.cpp
    ${EDITOR_DIR}/reload/HotReloadService.cpp
    ${EDITOR_DIR}/window/StatusBar.cpp
    ${EDITOR_DIR}/reload/handlers/Animations.cpp
    ${EDITOR_DIR}/reload/handlers/Assets.cpp
    ${EDITOR_DIR}/reload/handlers/Audio.cpp
    ${EDITOR_DIR}/reload/handlers/Particles.cpp
    ${EDITOR_DIR}/reload/handlers/Common.cpp
    ${EDITOR_DIR}/reload/handlers/Fonts.cpp
    ${EDITOR_DIR}/reload/handlers/Graphs.cpp
    ${EDITOR_DIR}/reload/handlers/HtmlUi.cpp
    ${EDITOR_DIR}/reload/handlers/Materials.cpp
    ${EDITOR_DIR}/reload/handlers/Models.cpp
    ${EDITOR_DIR}/reload/handlers/Scripts.cpp
    ${EDITOR_DIR}/reload/handlers/Shaders.cpp
    ${EDITOR_DIR}/reload/handlers/Textures.cpp
    ${EDITOR_DIR}/util/YamlFootprint.cpp
    ${EDITOR_DIR}/util/AssetDatabase.cpp
    ${EDITOR_DIR}/asset/AssetFileWrite.cpp
    ${EDITOR_DIR}/asset/AssetIndex.cpp
    ${EDITOR_DIR}/asset/AssetOps.cpp
    ${EDITOR_DIR}/asset/AssetRefEditor.cpp
    ${EDITOR_DIR}/asset/AssetMeta.cpp
    ${EDITOR_DIR}/asset/AssetRefSchema.cpp
    ${EDITOR_DIR}/asset/AssetTypes.cpp
    ${EDITOR_DIR}/asset/ContentHash.cpp
    ${EDITOR_DIR}/asset/DepEdge.cpp
    ${EDITOR_DIR}/asset/MissingAssetTracker.cpp
    ${EDITOR_DIR}/asset/ModelSubAssets.cpp
    ${EDITOR_DIR}/asset/TextReferences.cpp
    ${EDITOR_DIR}/util/Prefab.cpp
    ${EDITOR_DIR}/util/ProjectUtils.cpp
    ${EDITOR_DIR}/util/WaterEditorUtils.cpp
    ${EDITOR_DIR}/util/TerrainSplineBake.cpp
    ${EDITOR_DIR}/util/SplineBakeObjects.cpp
    ${EDITOR_DIR}/util/ParticleEffectEdit.cpp
    ${EDITOR_DIR}/util/ParticleMigrationReport.cpp
    ${EDITOR_DIR}/util/LatticeTrackKeys.cpp
    ${EDITOR_DIR}/util/EditorFingerprint.cpp
    ${EDITOR_DIR}/util/EditorScripts.cpp
    ${EDITOR_DIR}/util/ExternalCodeEditor.cpp
    ${EDITOR_DIR}/util/LuaApiData.cpp
    ${EDITOR_DIR}/util/NumericExpression.cpp
    ${EDITOR_DIR}/util/ProjectChecks.cpp
    ${EDITOR_DIR}/util/ShaderGlobalsJson.cpp
    ${EDITOR_DIR}/util/ShaderGlobalsUsage.cpp
    ${EDITOR_DIR}/util/ShaderGlobalsEdit.cpp
    ${EDITOR_DIR}/util/ProjectFileWrites.cpp
    ${EDITOR_DIR}/util/PhysicsMaterialAssets.cpp
    ${EDITOR_DIR}/util/RagdollWizard.cpp
    ${EDITOR_DIR}/window/dialog/RagdollWizardDialog.cpp
    ${EDITOR_DIR}/util/SafeWrite.cpp
    ${EDITOR_DIR}/util/BuiltPlayers.cpp
    ${EDITOR_DIR}/util/NormalMapTilt.cpp
    ${EDITOR_DIR}/util/HtmlUiExamples.cpp
    ${EDITOR_DIR}/util/ScriptParser.cpp
    ${EDITOR_DIR}/util/ScriptReloadService.cpp
    ${EDITOR_DIR}/util/ScriptStatus.cpp
    ${EDITOR_DIR}/util/SourceLocation.cpp
    ${EDITOR_DIR}/util/ScriptEvents.cpp
    ${EDITOR_DIR}/util/HtmlUITemplates.cpp
    ${EDITOR_DIR}/util/PngWriter.cpp
    ${EDITOR_DIR}/util/FloatImage.cpp
    ${EDITOR_DIR}/util/FbxImporter.cpp
    ${EDITOR_DIR}/util/GltfEditorDecoders.cpp
    ${EDITOR_DIR}/util/GlbBuilder.cpp
    ${JASPERENGINE_TEXSYNTH_SOURCES}
    ${EDITOR_DIR}/texsynth/SynthesisCli.cpp
    ${EDITOR_DIR}/texsynth/SynthesisSession.cpp
    ${JASPERENGINE_SPRITENORMALS_SOURCES}
    ${EDITOR_DIR}/spritenormals/SpriteNormalsSession.cpp
    ${EDITOR_DIR}/spritenormals/SpriteNormalsApply.cpp
    ${EDITOR_DIR}/spritenormals/SpriteNormalsCli.cpp
    ${EDITOR_DIR}/spritenormals/SpriteNormalsCheck.cpp
    ${EDITOR_DIR}/spritenormals/SpriteNormalsWatcher.cpp
    ${EDITOR_DIR}/physics/PlacementSettings.cpp
    ${EDITOR_DIR}/physics/PlacementSession.cpp
    ${EDITOR_DIR}/physics/PlacementCli.cpp
    ${EDITOR_DIR}/util/TerrainMapFileWriter.cpp
    ${EDITOR_DIR}/util/TerrainMapUtils.cpp
    ${EDITOR_DIR}/util/TerrainErosion.cpp
    ${EDITOR_DIR}/util/UIUtils.cpp
    ${EDITOR_DIR}/util/UiItemLocator.cpp
    ${EDITOR_DIR}/util/CommandRunner.cpp
    ${EDITOR_DIR}/util/TextEncoding.cpp
    ${EDITOR_DIR}/util/SystemFonts.cpp
    ${EDITOR_DIR}/util/ShellEnv.cpp
    ${EDITOR_DIR}/material/MaterialChange.cpp
    ${EDITOR_DIR}/pose/PoseAnimationDocument.cpp
    ${EDITOR_DIR}/pose/PoseBaker.cpp
    ${EDITOR_DIR}/pose/PoseRigProvider.cpp
    ${EDITOR_DIR}/pose/PoseEditSession.cpp
    ${EDITOR_DIR}/pose/PoseThumbnails.cpp
    ${EDITOR_DIR}/pose/PoseUiLogic.cpp
    ${EDITOR_DIR}/pose/PoseViewport.cpp
    ${EDITOR_DIR}/pose/PoseViewportLogic.cpp
    ${EDITOR_DIR}/animation/AnimationClipController.cpp
    ${EDITOR_DIR}/animation/AnimationClipDocument.cpp
    ${EDITOR_DIR}/animation/AnimatorAssetDocument.cpp
    ${EDITOR_DIR}/animation/AnimatorAssetUsers.cpp
    ${EDITOR_DIR}/animation/PreviewParticles.cpp
    ${EDITOR_DIR}/animation/ClipConverters.cpp
    ${EDITOR_DIR}/animation/ClipRecordHooks.cpp
    ${EDITOR_DIR}/animation/ClipValidation.cpp
    ${EDITOR_DIR}/animation/ClipPropertyNames.cpp
    ${EDITOR_DIR}/animation/ClipTangents.cpp
    ${EDITOR_DIR}/animation/PlayerModelLayout.cpp
    ${EDITOR_DIR}/animation/PreviewOwner.cpp
    ${EDITOR_DIR}/util/UserFiles.cpp

    ${EDITOR_DIR}/search/SearchScorer.cpp
    ${EDITOR_DIR}/search/SearchEngine.cpp
    ${EDITOR_DIR}/search/SearchPalette.cpp
    ${EDITOR_DIR}/search/AssetQuery.cpp
    ${EDITOR_DIR}/search/AssetSearchIndex.cpp
    ${EDITOR_DIR}/search/HierarchyFilter.cpp
    ${EDITOR_DIR}/shortcuts/Chord.cpp
    ${EDITOR_DIR}/shortcuts/CommandCatalog.cpp
    ${EDITOR_DIR}/shortcuts/DefaultCommands.cpp
    ${EDITOR_DIR}/shortcuts/PanelClaims.cpp
    ${EDITOR_DIR}/shortcuts/ShortcutDispatcher.cpp
    ${EDITOR_DIR}/shortcuts/ShortcutOverrides.cpp
    ${EDITOR_DIR}/shortcuts/ShortcutsWindow.cpp
    ${EDITOR_DIR}/layout/LayoutPresets.cpp
    ${EDITOR_DIR}/layout/LayoutManager.cpp
    ${EDITOR_DIR}/organisation/AssetLabels.cpp
    ${EDITOR_DIR}/organisation/Organisation.cpp
    ${EDITOR_DIR}/organisation/OrganisationCommands.cpp
    ${EDITOR_DIR}/organisation/Bookmarks.cpp
    ${EDITOR_DIR}/organisation/SavedSearchRows.cpp
    ${EDITOR_DIR}/organisation/LabelOperations.cpp
    ${EDITOR_DIR}/settings/SettingsRegistry.cpp
    ${EDITOR_DIR}/settings/SettingsRows.cpp
    ${EDITOR_DIR}/settings/SettingsSearch.cpp

    ${EDITOR_DIR}/vcs/ProcessExec.cpp
    ${EDITOR_DIR}/vcs/VcsSnapshot.cpp
    ${EDITOR_DIR}/vcs/VcsStatusPoller.cpp
    ${EDITOR_DIR}/vcs/VcsService.cpp
    ${EDITOR_DIR}/vcs/GitProvider.cpp
    ${EDITOR_DIR}/vcs/SvnProvider.cpp
    ${EDITOR_DIR}/vcs/SvnXml.cpp
    ${EDITOR_DIR}/vcs/scene/SceneYamlModel.cpp
    ${EDITOR_DIR}/vcs/scene/SceneDiff.cpp
    ${EDITOR_DIR}/vcs/scene/SceneMerge.cpp
    ${EDITOR_DIR}/vcs/scene/SceneDiffJson.cpp
    ${EDITOR_DIR}/vcs/scene/SceneCli.cpp
    ${EDITOR_DIR}/vcs/VcsEditorOps.cpp
    ${EDITOR_DIR}/vcs/SceneDiffTracker.cpp

    ${EDITOR_DIR}/htmlui/ContentSniffer.cpp
    ${EDITOR_DIR}/htmlui/GoogleFontsImporter.cpp
    ${EDITOR_DIR}/htmlui/HtmlUiAutoImport.cpp
    ${EDITOR_DIR}/htmlui/HtmlUiImportCli.cpp
    ${EDITOR_DIR}/htmlui/HtmlUiImporters.cpp
    ${EDITOR_DIR}/htmlui/HtmlUiImportMaintenance.cpp
    ${EDITOR_DIR}/htmlui/HtmlUiImportPanel.cpp
    ${EDITOR_DIR}/htmlui/HtmlUiImportQueue.cpp
    ${EDITOR_DIR}/htmlui/HttpClient.cpp
    ${EDITOR_DIR}/htmlui/ImageImporter.cpp
    ${EDITOR_DIR}/htmlui/ImportManifestWriter.cpp
    ${EDITOR_DIR}/htmlui/LottieImporter.cpp
    ${EDITOR_DIR}/htmlui/VideoImporter.cpp
    ${EDITOR_DIR}/htmlui/RemoteImporter.cpp
    ${EDITOR_DIR}/htmlui/StylesheetImporter.cpp

    ${EDITOR_DIR}/shader/ShaderBuilder.cpp
    ${EDITOR_DIR}/shader/ShaderVariantCollector.cpp
    ${EDITOR_DIR}/shader/ShaderListCache.cpp
    ${EDITOR_DIR}/shader/ShaderLibraryMatrix.cpp
    ${EDITOR_DIR}/shader/ShaderLibraryDoc.cpp
    ${EDITOR_DIR}/texture/AstcEncoder.cpp
    ${EDITOR_DIR}/texture/BasisEncoder.cpp
    ${EDITOR_DIR}/texture/BC7TransparentEndpointRepair.cpp
    ${EDITOR_DIR}/texture/BcEncoder.cpp
    ${EDITOR_DIR}/texture/TextureCookCache.cpp
    ${EDITOR_DIR}/texture/TextureCookQueue.cpp
    ${EDITOR_DIR}/texture/TextureCli.cpp
    ${EDITOR_DIR}/texture/TextureCookReport.cpp
    ${EDITOR_DIR}/texture/TextureCooker.cpp
    ${EDITOR_DIR}/texture/TextureEncoders.cpp
    ${EDITOR_DIR}/texture/TextureExport.cpp
    ${EDITOR_DIR}/texture/TextureFormatSelector.cpp
    ${EDITOR_DIR}/texture/TextureImportSettings.cpp
    ${EDITOR_DIR}/texture/TextureTypeInference.cpp
    ${EDITOR_DIR}/window/TextureImporterView.cpp
    ${EDITOR_DIR}/window/ImporterFields.cpp
    ${EDITOR_DIR}/window/ModelImporterView.cpp
    ${EDITOR_DIR}/window/MeshAuditWindow.cpp
    ${EDITOR_DIR}/window/BuildReportWindow.cpp
    ${EDITOR_DIR}/model/GltfDocument.cpp
    ${EDITOR_DIR}/model/ModelAudit.cpp
    ${EDITOR_DIR}/model/ModelCookCache.cpp
    ${EDITOR_DIR}/model/ModelCookQueue.cpp
    ${EDITOR_DIR}/model/ModelCooker.cpp
    ${EDITOR_DIR}/model/ModelExport.cpp
    ${EDITOR_DIR}/model/ModelHeader.cpp
    ${EDITOR_DIR}/model/ModelImportSettings.cpp
    ${EDITOR_DIR}/model/ModelPreviewRaster.cpp
    ${EDITOR_DIR}/model/ModelSettingsCmd.cpp
    ${EDITOR_DIR}/model/ModelSettingsEdit.cpp
    ${EDITOR_DIR}/model/ModelSourceDecode.cpp
    ${EDITOR_DIR}/model/ModelUsage.cpp

    ${EDITOR_DIR}/material/MaterialPresets.cpp
    ${EDITOR_DIR}/PostProcessingPresets.cpp

    ${EDITOR_DIR}/window/InspectorWindow.cpp
    ${EDITOR_DIR}/window/InspectorWindowClips.cpp
    ${EDITOR_DIR}/window/InspectorSplines.cpp
    ${EDITOR_DIR}/window/InputActionsWindow.cpp
    ${EDITOR_DIR}/window/InspectorWindowAnimLayers.cpp
    ${EDITOR_DIR}/window/InspectorAnimLayers.cpp
    ${EDITOR_DIR}/window/InspectorModelRig.cpp
    ${EDITOR_DIR}/window/InspectorWindowVat.cpp
    ${EDITOR_DIR}/window/InspectorPhysics2D.cpp
    ${EDITOR_DIR}/window/InspectorWindowSpriteNormals.cpp
    ${EDITOR_DIR}/window/InspectorShaderLibrary.cpp
    ${EDITOR_DIR}/window/InspectorMaterialPbr.cpp
    ${EDITOR_DIR}/window/InspectorParticles.cpp
    ${EDITOR_DIR}/window/InspectorPostProcessing.cpp
    ${EDITOR_DIR}/window/InspectorFluid.cpp
    ${EDITOR_DIR}/window/InspectorShadows.cpp
    ${EDITOR_DIR}/window/InspectorDefault.cpp
    ${EDITOR_DIR}/window/InspectorUpstream.cpp
    ${EDITOR_DIR}/window/InspectorPhysics.cpp
    ${EDITOR_DIR}/window/InspectorAnimation.cpp
    ${EDITOR_DIR}/window/InspectorRenderers.cpp
    ${EDITOR_DIR}/window/InspectorLattice.cpp
    ${EDITOR_DIR}/window/InspectorAnimator.cpp
    ${EDITOR_DIR}/window/InspectorLinesTrails.cpp
    ${EDITOR_DIR}/window/InspectorWindowCamera.cpp
    ${EDITOR_DIR}/window/InspectorAudioClip.cpp
    ${EDITOR_DIR}/window/ProjectWindowSound.cpp
    ${EDITOR_DIR}/window/InspectorVideo.cpp
    ${EDITOR_DIR}/window/InspectorPrefabBar.cpp
    ${EDITOR_DIR}/window/InspectorVcs.cpp
    ${EDITOR_DIR}/window/HierarchyPrefabRows.cpp
    ${EDITOR_DIR}/window/ConsoleWindow.cpp
    ${EDITOR_DIR}/window/ScriptStatusBadge.cpp
    ${EDITOR_DIR}/window/PlayModeChangesWindow.cpp
    ${EDITOR_DIR}/window/UndoHistoryWindow.cpp
    ${EDITOR_DIR}/window/BookmarksWindow.cpp
    ${EDITOR_DIR}/window/SceneWindow.cpp
    ${EDITOR_DIR}/window/ParticleEffectOverlay.cpp
    ${EDITOR_DIR}/window/PrefabStageBar.cpp
    ${EDITOR_DIR}/window/ExternalChangeBar.cpp
    ${EDITOR_DIR}/window/DeviceProfiles.cpp
    ${EDITOR_DIR}/window/CanvasScalerFields.cpp
    ${EDITOR_DIR}/window/GameResolutions.cpp
    ${EDITOR_DIR}/window/GameWindow.cpp
    ${EDITOR_DIR}/window/MainToolbar.cpp
    ${EDITOR_DIR}/window/PlayInput.cpp
    ${EDITOR_DIR}/window/HierarchyWindow.cpp
    ${EDITOR_DIR}/window/HierarchyPrefabMode.cpp
    ${EDITOR_DIR}/window/HierarchyVcs.cpp
    ${EDITOR_DIR}/window/ProjectWindow.cpp
    ${EDITOR_DIR}/window/ProjectWindowSearch.cpp
    ${EDITOR_DIR}/window/ProjectWindowVcs.cpp
    ${EDITOR_DIR}/window/vcs/VcsOverlay.cpp
    ${EDITOR_DIR}/window/vcs/VcsUi.cpp
    ${EDITOR_DIR}/window/vcs/VcsCommitWindow.cpp
    ${EDITOR_DIR}/window/vcs/VcsHistoryWindow.cpp
    ${EDITOR_DIR}/window/vcs/SceneDiffWindow.cpp
    ${EDITOR_DIR}/window/vcs/VcsConflictsWindow.cpp
    ${EDITOR_DIR}/window/AssetPanels.cpp
    ${EDITOR_DIR}/window/ImageViewerWindow.cpp
    ${EDITOR_DIR}/window/CodeEditor.cpp
    ${EDITOR_DIR}/window/AnimationWindow.cpp
    ${EDITOR_DIR}/window/AnimatorWindow.cpp
    ${EDITOR_DIR}/window/AvatarWindow.cpp
    ${EDITOR_DIR}/window/HumanoidConvertDialog.cpp
    ${EDITOR_DIR}/window/AnimationWindowClipSheet.cpp
    ${EDITOR_DIR}/window/AnimationWindowClipEvents.cpp
    ${EDITOR_DIR}/window/AnimationWindowClipAudio.cpp
    ${EDITOR_DIR}/window/AnimationWindowClipCurves.cpp
    ${EDITOR_DIR}/window/AnimationWindowClipRecord.cpp
    ${EDITOR_DIR}/window/AnimationWindowClipMode.cpp
    ${EDITOR_DIR}/window/AnimationWindowClipDraw.cpp
    ${EDITOR_DIR}/window/PoseAnimationWindow.cpp
    ${EDITOR_DIR}/window/AnimatorWindow.cpp
    ${EDITOR_DIR}/window/PoseAnimationWindowTemplates.cpp
    ${EDITOR_DIR}/window/TerrainEditWindow.cpp
    ${EDITOR_DIR}/lattice/LatticeEditorOps.cpp
    ${EDITOR_DIR}/lattice/LatticeBake.cpp
    ${EDITOR_DIR}/lattice/LatticeBakeGltf.cpp
    ${EDITOR_DIR}/lattice/LatticeSceneRender.cpp
    ${EDITOR_DIR}/spline/SplineEditor.cpp
    ${EDITOR_DIR}/spline/SplinePresets.cpp
    ${EDITOR_DIR}/spline/SplineSceneRender.cpp
    ${EDITOR_DIR}/render/SceneDrawModes.cpp
    ${EDITOR_DIR}/spline/SplineSceneTools.cpp
    ${EDITOR_DIR}/lattice/LatticeSceneTools.cpp
    ${EDITOR_DIR}/vat/VatAssetWriter.cpp
    ${EDITOR_DIR}/vat/VatAssetFiles.cpp
    ${EDITOR_DIR}/vat/VatDocument.cpp
    ${EDITOR_DIR}/vat/VatSceneActions.cpp
    ${EDITOR_DIR}/vat/VatCrowd.cpp
    ${EDITOR_DIR}/vat/VatReport.cpp
    ${EDITOR_DIR}/vat/VatBakeTypes.cpp
    ${EDITOR_DIR}/vat/VatBakeCompute.cpp
    ${EDITOR_DIR}/vat/VatBakeSource.cpp
    ${EDITOR_DIR}/vat/VatBakeSession.cpp
    ${EDITOR_DIR}/vat/VatFrontEnd.cpp
    ${EDITOR_DIR}/vat/VatCli.cpp
    ${EDITOR_DIR}/vat/VatContactSheet.cpp
    ${EDITOR_DIR}/vat/import/VatImport.cpp
    ${EDITOR_DIR}/vat/import/VatImportCommon.cpp
    ${EDITOR_DIR}/vat/import/VatSourceMesh.cpp
    ${EDITOR_DIR}/vat/import/HoudiniVat3.cpp
    ${EDITOR_DIR}/vat/import/OpenVat.cpp
    ${EDITOR_DIR}/vat/VatImportWrite.cpp
    # the Unity ParticleSystem importer (S20 systems)
    ${EDITOR_DIR}/unity/UnityYaml.cpp
    ${EDITOR_DIR}/unity/UnityProject.cpp
    ${EDITOR_DIR}/unity/UnityParticleImport.cpp
    ${EDITOR_DIR}/window/VertexAnimationBakerWindow.cpp
    ${EDITOR_DIR}/window/TextureSynthesisWindow.cpp
    ${EDITOR_DIR}/window/SpriteNormalsWindow.cpp
    ${EDITOR_DIR}/window/PhysicsPlacementWindow.cpp
    ${EDITOR_DIR}/window/SystemsWindow.cpp
    ${EDITOR_DIR}/window/LoadingWindow.cpp
    ${EDITOR_DIR}/window/dialog/SceneSaveDialog.cpp
    ${EDITOR_DIR}/window/dialog/DataSafetyDialogs.cpp
    ${EDITOR_DIR}/window/dialog/ComponentAddDialog.cpp
    ${EDITOR_DIR}/window/dialog/ProjectSaveDialog.cpp
    ${EDITOR_DIR}/window/dialog/NativePluginDialog.cpp
    ${EDITOR_DIR}/window/dialog/ScriptCreateDialog.cpp
    ${EDITOR_DIR}/window/dialog/ShaderForkDialog.cpp
    ${EDITOR_DIR}/window/dialog/TextureSlicerToolDialog.cpp
    ${EDITOR_DIR}/window/dialog/ExportWindow.cpp
    ${EDITOR_DIR}/window/dialog/ExportWindowShipping.cpp
    ${EDITOR_DIR}/window/dialog/EditorSettingsWindow.cpp
    ${EDITOR_DIR}/window/dialog/ProjectSettingsWindow.cpp
    ${EDITOR_DIR}/window/AudioMixerWindow.cpp
    ${EDITOR_DIR}/window/dialog/PrefabsWindow.cpp
    ${EDITOR_DIR}/window/dialog/AboutWindow.cpp
    ${EDITOR_DIR}/window/EditorFilePicker.cpp
    ${EDITOR_DIR}/window/EditorModal.cpp
    ${EDITOR_DIR}/window/dialog/ScenesWindow.cpp
    ${EDITOR_DIR}/window/widget/CustomTextEditor.cpp
    ${EDITOR_DIR}/window/widget/GraphCanvas.cpp
    ${EDITOR_DIR}/window/widget/HtmlCssLexer.cpp
    ${EDITOR_DIR}/window/widget/InputTextContextMenu.cpp
    ${EDITOR_DIR}/window/widget/SemanticSuggestions.cpp
    ${EDITOR_DIR}/window/widget/SelectableTextView.cpp

    ${EDITOR_DIR}/render/ToolsLayer.cpp
    ${EDITOR_DIR}/render/UILayer.cpp
    ${EDITOR_DIR}/render/SceneRender.cpp
    ${EDITOR_DIR}/render/SceneRender2D.cpp
    ${EDITOR_DIR}/render/SceneRender3D.cpp
    ${EDITOR_DIR}/render/gizmo/TranslateGizmo.cpp
    ${EDITOR_DIR}/render/gizmo/RotateGizmo.cpp
    ${EDITOR_DIR}/render/gizmo/ScaleGizmo.cpp
    ${EDITOR_DIR}/render/gizmo/Object2DGizmo.cpp
    ${EDITOR_DIR}/render/gizmo/AnchorGizmo.cpp
    ${EDITOR_DIR}/render/gizmo/ViewportGizmo.cpp
    ${EDITOR_DIR}/render/preview/MaterialRender.cpp
    ${EDITOR_DIR}/render/preview/MeshPreviewRender.cpp
    ${EDITOR_DIR}/render/preview/DirectionRender.cpp
    ${EDITOR_DIR}/render/preview/ModelRender.cpp
    ${EDITOR_DIR}/render/preview/PrefabRender.cpp
    ${EDITOR_DIR}/render/preview/PoseRender.cpp
    ${EDITOR_DIR}/render/preview/VatPreviewRender.cpp

    ${EDITOR_DIR}/pose/PoseImage.cpp
    ${EDITOR_DIR}/pose/PoseSheetRenderer.cpp
    ${EDITOR_DIR}/pose/PoseTemplates.cpp
    ${EDITOR_DIR}/pose/PoseTemplatesBuiltin.cpp
    ${EDITOR_DIR}/render/preview/ParticlePreviewRender.cpp

    ${EDITOR_DIR}/command/CommandHandle.cpp
    ${EDITOR_DIR}/command/CommandHistory.cpp
    ${EDITOR_DIR}/command/CommandGroup.cpp
    ${EDITOR_DIR}/command/CommandNames.cpp
    ${EDITOR_DIR}/command/UndoOrigin.cpp
    ${EDITOR_DIR}/command/UndoTimeline.cpp
    ${EDITOR_DIR}/command/PlayChangeReview.cpp
    ${EDITOR_DIR}/command/type/AddChildSceneCmd.cpp
    ${EDITOR_DIR}/command/type/AddComponentCmd.cpp
    ${EDITOR_DIR}/command/type/RemoveComponentCmd.cpp
    ${EDITOR_DIR}/command/type/RemovePreservedComponentCmd.cpp
    ${EDITOR_DIR}/command/type/CreateEntityCmd.cpp
    ${EDITOR_DIR}/command/type/CreateTrackKeyCmd.cpp
    ${EDITOR_DIR}/command/type/AddActionFrameCmd.cpp
    ${EDITOR_DIR}/command/type/DeleteEntityCmd.cpp
    ${EDITOR_DIR}/command/type/DuplicateEntityCmd.cpp
    ${EDITOR_DIR}/command/type/PasteEntitiesCmd.cpp
    ${EDITOR_DIR}/command/type/ComponentValuesCmd.cpp
    ${EDITOR_DIR}/command/type/ReflectedValueCmd.cpp
    ${EDITOR_DIR}/command/type/AtmospherePresetCmd.cpp
    ${EDITOR_DIR}/command/type/MoveEntityOrderCmd.cpp
    ${EDITOR_DIR}/command/type/ObjectTransformCmd.cpp
    ${EDITOR_DIR}/command/type/RemoveChildSceneCmd.cpp
    ${EDITOR_DIR}/command/type/SetChildSceneStartActiveCmd.cpp
    ${EDITOR_DIR}/command/type/EntityNameCmd.cpp
    ${EDITOR_DIR}/command/type/ImportPrefabCmd.cpp
    ${EDITOR_DIR}/command/type/RemoveEntityFromPrefabCmd.cpp
    ${EDITOR_DIR}/command/type/AddEntityToPrefabCmd.cpp
    ${EDITOR_DIR}/command/type/MeshChangeCmd.cpp
    ${EDITOR_DIR}/command/type/ModelLoadCmd.cpp
    ${EDITOR_DIR}/command/type/ModelClearCmd.cpp
    ${EDITOR_DIR}/command/type/SceneNameCmd.cpp
    ${EDITOR_DIR}/command/type/AssetRefCmds.cpp
    ${EDITOR_DIR}/command/type/SelectionCmd.cpp
    ${EDITOR_DIR}/command/type/SetAssetMetaCmd.cpp
    ${EDITOR_DIR}/command/type/AudioSettingsCmd.cpp
    ${EDITOR_DIR}/audio/AudioMixerDocument.cpp
    ${EDITOR_DIR}/audio/SoundEventDocument.cpp
    ${EDITOR_DIR}/audio/SoundEventRender.cpp
    ${EDITOR_DIR}/window/InspectorSoundEvent.cpp
    ${EDITOR_DIR}/window/AudioCapturesWindow.cpp
    ${EDITOR_DIR}/window/SoundBrowserWindow.cpp
    ${EDITOR_DIR}/audio/SoundDrop.cpp
    ${EDITOR_DIR}/mcp/tools/SoundEventTools.cpp
    ${EDITOR_DIR}/audio/AudioAnalysis.cpp
    ${EDITOR_DIR}/audio/AudioCompare.cpp
    ${EDITOR_DIR}/audio/AudioCredits.cpp
    ${EDITOR_DIR}/audio/AudioWaveform.cpp
    ${EDITOR_DIR}/audio/TimelineAudioPreview.cpp
    ${EDITOR_DIR}/audio/BuiltinSoundImport.cpp
    ${EDITOR_DIR}/audio/ClipDefects.cpp
    ${EDITOR_DIR}/audio/SoundEssentials.cpp
    ${EDITOR_DIR}/audio/SoundLibrary.cpp
    ${EDITOR_DIR}/audio/SoundLibraryCli.cpp
    ${EDITOR_DIR}/audio/SoundManifest.cpp
    ${EDITOR_DIR}/audio/SoundDesign.cpp
    ${EDITOR_DIR}/audio/SoundFit.cpp
    ${EDITOR_DIR}/audio/SoundSearch.cpp
    ${EDITOR_DIR}/audio/AudioBaseline.cpp
    ${EDITOR_DIR}/audio/AudioQa.cpp
    ${EDITOR_DIR}/audio/AudioQaSettings.cpp
    ${EDITOR_DIR}/audio/AudioSessionReader.cpp
    ${EDITOR_DIR}/audio/AudioTimelineImage.cpp
    ${EDITOR_DIR}/audio/SoundSlicer.cpp
    ${EDITOR_DIR}/audio/SoundSynth.cpp
    ${JASPERENGINE_ROOT}/libs/soloud/src/audiosource/sfxr/soloud_sfxr.cpp
    ${EDITOR_DIR}/audio/AudioImages.cpp
    ${EDITOR_DIR}/audio/AudioImportRegistry.cpp
    ${EDITOR_DIR}/audio/AudioImportSettings.cpp
    ${EDITOR_DIR}/audio/AudioMusicGrid.cpp
    ${EDITOR_DIR}/audio/AudioProvenance.cpp
    ${EDITOR_DIR}/audio/LoudnessMonitor.cpp
    ${EDITOR_DIR}/command/type/ShaderGlobalsCmd.cpp
    ${EDITOR_DIR}/command/type/InputActionsCmd.cpp
    ${EDITOR_DIR}/command/type/DebugSettingsCmd.cpp
    ${EDITOR_DIR}/command/type/CopyFileCmd.cpp
    ${EDITOR_DIR}/command/type/RenameFileCmd.cpp
    ${EDITOR_DIR}/command/type/CreateDirCmd.cpp
    ${EDITOR_DIR}/command/type/ThreadingSettingsCmd.cpp
    ${EDITOR_DIR}/command/type/ShaderCreationSettingsCmd.cpp
    ${EDITOR_DIR}/command/type/HtmlUiRenderSettingsCmd.cpp
    ${EDITOR_DIR}/command/type/DrawSubmissionSettingsCmd.cpp
    ${EDITOR_DIR}/command/type/DeviceSettingsCmd.cpp
    ${EDITOR_DIR}/command/type/DeleteFileCmd.cpp
    ${EDITOR_DIR}/command/type/SetMainCameraCmd.cpp
    ${EDITOR_DIR}/command/type/CreateMaterialFileCmd.cpp
    ${EDITOR_DIR}/command/type/UnlinkMaterialCmd.cpp
    ${EDITOR_DIR}/command/type/LinkMaterialCmd.cpp
    ${EDITOR_DIR}/command/type/CreatePrefabCmd.cpp
    ${EDITOR_DIR}/command/type/CreateClipCmd.cpp
    ${EDITOR_DIR}/command/type/CreateVatCmd.cpp
    ${EDITOR_DIR}/command/type/ClipSceneActions.cpp
    ${EDITOR_DIR}/command/type/PlayerLayerActions.cpp
    ${EDITOR_DIR}/command/type/CreatePrefabVariantAssetCmd.cpp
    ${EDITOR_DIR}/command/type/ComponentToPrefabSharedCmd.cpp
    ${EDITOR_DIR}/command/type/UnpackPrefabCmd.cpp
    ${EDITOR_DIR}/command/type/PrefabOverridesCmd.cpp
    ${EDITOR_DIR}/command/type/PrefabOverrideCmd.cpp
    ${EDITOR_DIR}/command/type/ReplacePrefabCmd.cpp
    ${EDITOR_DIR}/command/type/ForkShaderCmd.cpp
    ${EDITOR_DIR}/command/type/TerrainMapPatchCmd.cpp
    ${EDITOR_DIR}/command/type/WriteFilesCmd.cpp
    ${EDITOR_DIR}/command/type/RemoveFilesCmd.cpp
    ${EDITOR_DIR}/command/type/LatticePointsCmd.cpp
    ${EDITOR_DIR}/command/type/SplineEditCmd.cpp
    ${EDITOR_DIR}/command/type/LatticeBindingsCmd.cpp
    ${EDITOR_DIR}/command/type/LatticeBakeCmd.cpp
    ${EDITOR_DIR}/command/type/GraphEditCmd.cpp
    ${EDITOR_DIR}/command/type/InputActionsEditCmd.cpp
    ${EDITOR_DIR}/command/type/ParticleEffectAssetCmd.cpp
    ${EDITOR_DIR}/command/type/RevertFilesCmd.cpp
    ${EDITOR_DIR}/command/type/RevertToBaselineCmd.cpp

    ${IMGUI_SOURCES}
    ${TINYEXPR_SOURCES}
    ${MINIZ_SOURCES}
    ${BACKEND_SOURCES}
)

if(APPLE)
    # NSPasteboard image/file-URL reading for the AI chat attachment paste.
    list(APPEND JASPERENGINE_EDITOR_SOURCES ${EDITOR_DIR}/util/ClipboardMac.mm)
endif()

if(WIN32)
    list(APPEND JASPERENGINE_EDITOR_SOURCES ${EDITOR_DIR}/util/MsBuildProgress.cpp)
endif()

# navigation (S19): its own list, so the streams adding editor files do not meet here
include(${EDITOR_DIR}/navigation/NavigationEditorSources.cmake)
# the editor tools (S27): colour picker, curve and gradient fields, Project Hub, Recorder
include(${EDITOR_DIR}/EditorToolsSources.cmake)
include(${EDITOR_DIR}/procmat/ProcMatSources.cmake)

# The engine build C++ script libraries must be made for (S23 toolchain fingerprint): the
# editor version, on this one file (as on the engine's ToolchainFingerprint.cpp)
set_source_files_properties(${EDITOR_DIR}/util/EditorFingerprint.cpp PROPERTIES
    COMPILE_DEFINITIONS "JASPERENGINE_ABI_VERSION=\"${JASPERENGINE_EDITOR_VERSION}\""
    SKIP_UNITY_BUILD_INCLUSION ON)

# Unity build: these sources keep generic file-local helpers (lower, trim, Target, sceneArg, ...) in
# anonymous namespaces, one helper set per file, so they never share a unity unit
set(JASPERENGINE_EDITOR_UNITY_SKIP ${JASPERENGINE_EDITOR_SOURCES})
list(FILTER JASPERENGINE_EDITOR_UNITY_SKIP INCLUDE REGEX "(^|/)editor/(htmlui|mcp/tools|reflect|vcs)/")
jasperengine_skip_unity_build(
    REASON "the HTML UI importers, the MCP tools, the reflection tables and the version control providers: per-file helper sets with the same names"
    SOURCES ${JASPERENGINE_EDITOR_UNITY_SKIP})
set(JASPERENGINE_EDITOR_MODEL_SOURCES ${JASPERENGINE_EDITOR_SOURCES})
list(FILTER JASPERENGINE_EDITOR_MODEL_SOURCES INCLUDE REGEX "(^|/)editor/model/")
jasperengine_skip_unity_build(
    REASON "the model cook (S53): file helpers named as the texture cook's (readAll, fileTime, touch)"
    SOURCES ${JASPERENGINE_EDITOR_MODEL_SOURCES})
set(JASPERENGINE_EDITOR_SCENEVIEW_SOURCES ${JASPERENGINE_EDITOR_SOURCES})
list(FILTER JASPERENGINE_EDITOR_SCENEVIEW_SOURCES INCLUDE REGEX "(^|/)editor/(picking|sceneview)/")
jasperengine_skip_unity_build(
    REASON "the Scene view picking and tools (S26): per-file helpers (PI, now, project to view)"
    SOURCES ${JASPERENGINE_EDITOR_SCENEVIEW_SOURCES})
jasperengine_skip_unity_build(
    REASON "its own static copy of ImGui's stb_truetype, which another unit's stb_truetype.h would clash with"
    SOURCES ${EDITOR_DIR}/util/SystemFonts.cpp)

# Unity build: these sources keep file-local helpers or types (Registry, fail, lower, TempDir, ...)
# whose names another editor or editor test source defines too (docs/specs/S33-build-speed.md M8)
jasperengine_skip_unity_build(
    REASON "file-local helpers or types named as in another editor source"
    SOURCES
        ${EDITOR_DIR}/util/AssetDatabase.cpp
        ${EDITOR_DIR}/util/NativePlugins.cpp
        ${EDITOR_DIR}/window/AvatarWindow.cpp
        ${EDITOR_DIR}/Out.cpp
        ${EDITOR_DIR}/StreamSchema.cpp
        ${EDITOR_DIR}/vat/VatAssetWriter.cpp
        ${EDITOR_DIR}/vat/VatImportWrite.cpp
        ${EDITOR_DIR}/window/InspectorAnimLayers.cpp
        ${EDITOR_DIR}/window/InspectorWindowVat.cpp
        ${EDITOR_DIR}/spritenormals/SpriteNormalsApply.cpp
        ${EDITOR_DIR}/spritenormals/SpriteNormalsCheck.cpp
        ${EDITOR_DIR}/util/VideoTextureLink.cpp
        ${EDITOR_DIR}/vat/VatAssetFiles.cpp
        ${EDITOR_DIR}/vat/VatBakeSession.cpp
        ${EDITOR_DIR}/vat/VatBakeTypes.cpp
        ${EDITOR_DIR}/vat/VatCli.cpp
        ${EDITOR_DIR}/vat/VatDocument.cpp
        ${EDITOR_DIR}/vat/VatFrontEnd.cpp
        ${EDITOR_DIR}/vat/VatSceneActions.cpp
        ${EDITOR_DIR}/vat/import/OpenVat.cpp
        ${EDITOR_DIR}/window/AnimationWindowClipDraw.cpp
        ${EDITOR_DIR}/window/InspectorWindowAnimLayers.cpp
        ${EDITOR_DIR}/window/PhysicsPlacementWindow.cpp
        ${EDITOR_DIR}/window/SpriteNormalsWindow.cpp
        ${EDITOR_DIR}/window/TextureSynthesisWindow.cpp
        ${EDITOR_DIR}/window/VertexAnimationBakerWindow.cpp
        ${EDITOR_DIR}/widget/CurveEditorOps.cpp
        ${EDITOR_DIR}/widget/GradientEditorOps.cpp
        ${EDITOR_DIR}/animation/AnimationClipController.cpp
        ${EDITOR_DIR}/animation/AnimationClipDocument.cpp
        ${EDITOR_DIR}/animation/AnimatorAssetDocument.cpp
        ${EDITOR_DIR}/animation/ClipConverters.cpp
        ${EDITOR_DIR}/animation/ClipRecordHooks.cpp
        ${EDITOR_DIR}/animation/PreviewOwner.cpp
        ${EDITOR_DIR}/asset/AssetIndex.cpp
        ${EDITOR_DIR}/cook/RuntimeFieldsEmitter.cpp
        ${EDITOR_DIR}/lattice/LatticeBake.cpp
        ${EDITOR_DIR}/lattice/LatticeBakeGltf.cpp
        ${EDITOR_DIR}/lattice/LatticeSceneTools.cpp
        ${EDITOR_DIR}/layout/LayoutPresets.cpp
        ${EDITOR_DIR}/mcp/McpRefs.cpp
        ${EDITOR_DIR}/organisation/AssetLabels.cpp
        ${EDITOR_DIR}/platform/AndroidPackager.cpp
        ${EDITOR_DIR}/platform/IosProject.cpp
        ${EDITOR_DIR}/publish/PayloadValidator.cpp
        ${EDITOR_DIR}/publish/Publishing.cpp
        ${EDITOR_DIR}/search/AssetQuery.cpp
        ${EDITOR_DIR}/shortcuts/Chord.cpp
        ${EDITOR_DIR}/spline/SplineEditor.cpp
        ${EDITOR_DIR}/tests/PhysicsPlacementWorkspaceTest.cpp
        ${EDITOR_DIR}/tests/ProjectSaveTest.cpp
        ${EDITOR_DIR}/tests/animation/AnimationClipAssetsTest.cpp
        ${EDITOR_DIR}/tests/animation/AnimationClipControllerTest.cpp
        ${EDITOR_DIR}/tests/animation/AnimationPlayerProjectTest.cpp
        ${EDITOR_DIR}/tests/animation/AnimationWindowClipTest.cpp
        ${EDITOR_DIR}/tests/animation/AnimatorEditorTest.cpp
        ${EDITOR_DIR}/tests/animation/ClipConvertersTest.cpp
        ${EDITOR_DIR}/tests/animation/InspectorClipPrefabTest.cpp
        ${EDITOR_DIR}/tests/animation/PlayerModelLayoutTest.cpp
        ${EDITOR_DIR}/tests/command/CommandHistoryHookTest.cpp
        ${EDITOR_DIR}/tests/engine/action/ActionTracksTest.cpp
        ${EDITOR_DIR}/tests/engine/physics/ConvexDecompositionTest.cpp
        ${EDITOR_DIR}/tests/engine/subsystem/PhysicsSyncTest.cpp
        ${EDITOR_DIR}/tests/picking/BvhDiskCacheTest.cpp
        ${EDITOR_DIR}/tests/reflect/ReflectMigrationTest.cpp
        ${EDITOR_DIR}/tests/reflect/ReflectResolveBenchTest.cpp
        ${EDITOR_DIR}/tests/sceneview/SceneViewStateTest.cpp
        ${EDITOR_DIR}/tests/stream/StochasticTilingTest.cpp
        ${EDITOR_DIR}/tests/stream/StreamValueTest.cpp
        ${EDITOR_DIR}/tests/texsynth/TexSynthTest.cpp
        ${EDITOR_DIR}/tests/util/FbxImportTest.cpp
        ${EDITOR_DIR}/tests/util/NormalMapTiltTest.cpp
        ${EDITOR_DIR}/tests/vcs/SceneDiffTest.cpp
        ${EDITOR_DIR}/tests/vcs/SceneMergeTest.cpp
        ${EDITOR_DIR}/tests/vcs/SvnProviderTest.cpp
        ${EDITOR_DIR}/texsynth/SynthesisSession.cpp
        ${EDITOR_DIR}/util/FluidTextureLink.cpp
        ${EDITOR_DIR}/util/HtmlUiExamples.cpp
        ${EDITOR_DIR}/util/NumericExpression.cpp
        ${EDITOR_DIR}/util/ParticleEffectEdit.cpp
        ${EDITOR_DIR}/util/ProjectChecks.cpp
        ${EDITOR_DIR}/window/InspectorAnimator.cpp
        ${EDITOR_DIR}/window/InspectorFluid.cpp
        ${EDITOR_DIR}/window/PoseAnimationWindow.cpp
        ${EDITOR_DIR}/window/PoseAnimationWindowTemplates.cpp
        ${EDITOR_DIR}/window/vcs/VcsConflictsWindow.cpp
        ${EDITOR_DIR}/window/widget/HtmlCssLexer.cpp)

function(configure_jasperengine_editor_target target_name)
    add_dependencies(${target_name} generate_api_suggestions generate_sound_essentials)

    target_include_directories(${target_name} SYSTEM PRIVATE
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        engine/libs/meshoptimizer
        libs/tinyexpr
        libs/miniz
        ${OPENGL_INCLUDE_DIR}
    )

    target_include_directories(${target_name} PRIVATE
        ${GENERATED_DIR}
    )

    target_compile_definitions(${target_name} PRIVATE
        JASPERENGINE_EXPLICIT_COMPONENT_INCLUDES
        "JASPERENGINE_CXX_STANDARD_LIST=${JASPERENGINE_CXX_STANDARD_LIST}"
        "JASPERENGINE_CXX_FLOOR_GNU=\"${JASPERENGINE_CXX_FLOOR_GNU}\""
        "JASPERENGINE_CXX_FLOOR_CLANG=\"${JASPERENGINE_CXX_FLOOR_Clang}\""
        "JASPERENGINE_CXX_FLOOR_APPLECLANG=\"${JASPERENGINE_CXX_FLOOR_AppleClang}\""
        "JASPERENGINE_CXX_FLOOR_MSVC=\"${JASPERENGINE_CXX_FLOOR_MSVC}\""
    )

    target_link_libraries(
        ${target_name}

        PRIVATE

        ${BACKEND_LIBS}
        ${OPENGL_LIBRARIES}
        Threads::Threads
        jasperengine
        nfd
        yaml-cpp
        zstd
        bc7enc_rdo
        astcenc
        basisu_encoder
        ufbx
        tinyexr
        draco_decoder
        webp_decoder
    )

    if(WIN32)
        # dbghelp resolves the crash-report backtrace; ws2_32 is the MCP HTTP server's Winsock.
        target_link_libraries(${target_name} PRIVATE dbghelp ws2_32)
    else()
        # -rdynamic names the editor's own frames in the crash backtrace, at about 10 MB of
        # symbol table. A Release build stays lean and is resolved with addr2line instead.
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
            set_target_properties(${target_name} PROPERTIES ENABLE_EXPORTS ON)
        endif()
    endif()
    # Large translation units (editor/mcp/tools/WorldTools.cpp, Stream.cpp) pass the COFF
    # section limit; /utf-8: sources (tool descriptions) are UTF-8 whatever the code page
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /bigobj /utf-8)
    elseif(MINGW)
        target_compile_options(${target_name} PRIVATE -Wa,-mbig-obj)
    endif()

    if(X11_FOUND)
        target_compile_definitions(${target_name} PRIVATE JASPERENGINE_HAS_X11_CLIPBOARD)
    endif()

    jasperengine_skip_unity_build(
        REASON "Xlib.h defines None, Bool, Status, Always... as macros, which break every later source of the batch"
        SOURCES ${EDITOR_DIR}/util/Clipboard.cpp ${EDITOR_DIR}/mcp/tools/BuildTools.cpp
                ${EDITOR_DIR}/backend/BackendLinux.cpp ${JASPERENGINE_ROOT}/platform/linux/WindowLinux.cpp
                ${EDITOR_DIR}/platform/ScreenColorX11.cpp ${EDITOR_DIR}/platform/LaunchKeysX11.cpp ${JASPERENGINE_ROOT}/platform/linux/HostServicesLinux.cpp)
    jasperengine_target_warnings(${target_name})
    jasperengine_target_unity_build(${target_name})
    get_target_property(targetType ${target_name} TYPE)
    if(NOT targetType STREQUAL "OBJECT_LIBRARY")
        jasperengine_stage_asan_runtime(${target_name})
    endif()

    set_target_properties(
        ${target_name}

        PROPERTIES

        CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED ON
        OBJCXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
        OBJCXX_STANDARD_REQUIRED ON
    )

    if(APPLE)
        set_target_properties(${target_name} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "@loader_path"
        )
    elseif(UNIX)
        set_target_properties(${target_name} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN"
        )
    endif()
endfunction()

# The editor's sources but main.cpp, compiled once for every executable made of them: the editor,
# its Windows command-line twin and the editor test targets (docs/specs/S33-build-speed.md R1).
# The executables add main.cpp (the test targets their own main) and link these objects, in the
# order the sources are listed here.
set(JASPERENGINE_EDITOR_OBJECT_SOURCES ${JASPERENGINE_EDITOR_SOURCES})
list(REMOVE_ITEM JASPERENGINE_EDITOR_OBJECT_SOURCES ${EDITOR_DIR}/main.cpp)
add_library(jasperengine-editor-objects OBJECT ${JASPERENGINE_EDITOR_OBJECT_SOURCES})
configure_jasperengine_editor_target(jasperengine-editor-objects)
jasperengine_target_pch(jasperengine-editor-objects EDITOR)

add_executable(
    jasperengine-editor

    ${PLATFORM_EXEC_FLAGS}

    ${EDITOR_DIR}/main.cpp
    ${JASPERENGINE_EDITOR_ICON_OBJECTS}
)

configure_jasperengine_editor_target(jasperengine-editor)
target_link_libraries(jasperengine-editor PRIVATE jasperengine-editor-objects)

if(WIN32)
    jasperengine_add_windows_manifest(jasperengine-editor)
endif()

if(PLATFORM_PROPERTIES_FLAGS)
    set_target_properties(jasperengine-editor PROPERTIES ${PLATFORM_PROPERTIES_FLAGS})
endif()

# The dylib goes in Contents/MacOS, where @loader_path already finds it.
if(APPLE)
    set(JASPERENGINE_BUNDLE_NAME "JasperEngine")
    set(JASPERENGINE_BUNDLE_IDENTIFIER "com.jaspergameengine.editor")
    set(JASPERENGINE_BUNDLE_ICON_NAME "jasperengine.icns")

    # CFBundleShortVersionString takes digits and dots only.
    string(REPLACE "," "." JASPERENGINE_BUNDLE_VERSION "${JASPERENGINE_VERSION_NUMERIC}")
    string(REGEX REPLACE "^([0-9]+\\.[0-9]+\\.[0-9]+).*$" "\\1" JASPERENGINE_BUNDLE_VERSION "${JASPERENGINE_BUNDLE_VERSION}")

    # the deployment target set before project() (or given to the configure)
    set(JASPERENGINE_BUNDLE_MIN_OS "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    if(NOT JASPERENGINE_BUNDLE_MIN_OS)
        set(JASPERENGINE_BUNDLE_MIN_OS "12.0")
    endif()

    set(JASPERENGINE_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/${EDITOR_DIR}/resources/icons/jasperengine-editor.png")
    set(JASPERENGINE_ICONSET "${CMAKE_CURRENT_BINARY_DIR}/resources/jasperengine.iconset")
    set(JASPERENGINE_ICNS "${CMAKE_CURRENT_BINARY_DIR}/resources/${JASPERENGINE_BUNDLE_ICON_NAME}")

    add_custom_command(
        OUTPUT ${JASPERENGINE_ICNS}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${JASPERENGINE_ICONSET}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${JASPERENGINE_ICONSET}
        COMMAND sips -z 16 16     ${JASPERENGINE_ICON_PNG} --out ${JASPERENGINE_ICONSET}/icon_16x16.png
        COMMAND sips -z 32 32     ${JASPERENGINE_ICON_PNG} --out ${JASPERENGINE_ICONSET}/icon_16x16@2x.png
        COMMAND sips -z 32 32     ${JASPERENGINE_ICON_PNG} --out ${JASPERENGINE_ICONSET}/icon_32x32.png
        COMMAND sips -z 64 64     ${JASPERENGINE_ICON_PNG} --out ${JASPERENGINE_ICONSET}/icon_32x32@2x.png
        COMMAND sips -z 128 128   ${JASPERENGINE_ICON_PNG} --out ${JASPERENGINE_ICONSET}/icon_128x128.png
        COMMAND sips -z 256 256   ${JASPERENGINE_ICON_PNG} --out ${JASPERENGINE_ICONSET}/icon_128x128@2x.png
        COMMAND sips -z 256 256   ${JASPERENGINE_ICON_PNG} --out ${JASPERENGINE_ICONSET}/icon_256x256.png
        COMMAND sips -z 512 512   ${JASPERENGINE_ICON_PNG} --out ${JASPERENGINE_ICONSET}/icon_256x256@2x.png
        COMMAND sips -z 512 512   ${JASPERENGINE_ICON_PNG} --out ${JASPERENGINE_ICONSET}/icon_512x512.png
        COMMAND sips -z 1024 1024 ${JASPERENGINE_ICON_PNG} --out ${JASPERENGINE_ICONSET}/icon_512x512@2x.png
        COMMAND iconutil -c icns ${JASPERENGINE_ICONSET} -o ${JASPERENGINE_ICNS}
        DEPENDS ${JASPERENGINE_ICON_PNG}
        COMMENT "Generating ${JASPERENGINE_BUNDLE_ICON_NAME}"
        VERBATIM
    )

    target_sources(jasperengine-editor PRIVATE ${JASPERENGINE_ICNS})
    set_source_files_properties(${JASPERENGINE_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/${EDITOR_DIR}/resources/macos/Info.plist.in"
        "${CMAKE_CURRENT_BINARY_DIR}/resources/Info.plist"
        @ONLY
    )

    set_target_properties(jasperengine-editor PROPERTIES
        OUTPUT_NAME "${JASPERENGINE_BUNDLE_NAME}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/resources/Info.plist"
    )

    add_custom_command(TARGET jasperengine-editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:jasperengine> $<TARGET_FILE_DIR:jasperengine-editor>
        COMMENT "Copying the engine runtime into the bundle"
    )
endif()

# In the bundle this has to be Resources, not MacOS: codesign treats everything in
# Contents/MacOS as code. FileUtils::getEngineDir() looks in both places.
if(APPLE)
    set(JASPERENGINE_EDITOR_ENGINE_DIR $<TARGET_BUNDLE_CONTENT_DIR:jasperengine-editor>/Resources/engine)
    # Build trees from before the move still hold a copy in MacOS, which codesign rejects.
    set(JASPERENGINE_CLEAN_STALE_ENGINE COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:jasperengine-editor>/engine)
else()
    set(JASPERENGINE_EDITOR_ENGINE_DIR $<TARGET_FILE_DIR:jasperengine-editor>/engine)
    set(JASPERENGINE_CLEAN_STALE_ENGINE)
endif()

add_custom_target(copy_jasperengine_editor_engine
    ${JASPERENGINE_CLEAN_STALE_ENGINE}
    COMMAND ${CMAKE_COMMAND}
        -DENGINE_SOURCE:PATH=${CMAKE_SOURCE_DIR}/engine
        -DENGINE_DEST:PATH=${JASPERENGINE_EDITOR_ENGINE_DIR}
        # what the copy made, so a file deleted from engine/ leaves the copy too
        -DMANIFEST:FILEPATH=${CMAKE_BINARY_DIR}/engine-copy-manifest.txt
        -P ${CMAKE_SOURCE_DIR}/CopyEditorEngine.cmake
    COMMENT "Copying engine next to the editor"
    VERBATIM
)

add_dependencies(jasperengine-editor copy_jasperengine_editor_engine)

# Stage the starter library next to the editor target (a build tree runs from there), removing the
# destination first so a clip dropped from the library never lingers. It is editor data: no player
# input names it.
set(JASPERENGINE_SOUND_LIBRARY_SOURCE ${CMAKE_SOURCE_DIR}/editor/resources/sound-library/library)
if(APPLE)
    set(JASPERENGINE_EDITOR_SOUND_LIBRARY_DIR $<TARGET_BUNDLE_CONTENT_DIR:jasperengine-editor>/Resources/sound-library)
else()
    set(JASPERENGINE_EDITOR_SOUND_LIBRARY_DIR $<TARGET_FILE_DIR:jasperengine-editor>/sound-library)
endif()
set(JASPERENGINE_SOUND_LIBRARY_STAMP ${CMAKE_CURRENT_BINARY_DIR}/sound-library-$<CONFIG>.stamp)
add_custom_command(
    OUTPUT ${JASPERENGINE_SOUND_LIBRARY_STAMP}
    COMMAND ${CMAKE_COMMAND}
        -DSOUND_LIBRARY_SOURCE:PATH=${JASPERENGINE_SOUND_LIBRARY_SOURCE}
        -DSOUND_LIBRARY_DEST:PATH=${JASPERENGINE_EDITOR_SOUND_LIBRARY_DIR}
        -DSOUND_LIBRARY_STAMP:PATH=${JASPERENGINE_SOUND_LIBRARY_STAMP}
        -P ${CMAKE_SOURCE_DIR}/CopySoundLibrary.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/CopySoundLibrary.cmake ${JASPERENGINE_SOUND_LIBRARY_SOURCE}/manifest.json ${JASPERENGINE_SOUND_LIBRARY_SOURCE}/analysis.json
    COMMENT "Copying the starter library next to the editor"
    VERBATIM
)
add_custom_target(copy_jasperengine_editor_sound_library DEPENDS ${JASPERENGINE_SOUND_LIBRARY_STAMP})
add_dependencies(jasperengine-editor copy_jasperengine_editor_sound_library)

set(JASPERENGINE_EDITOR_INSTALL_TARGETS jasperengine-editor)

# jasperengine-htmlui-editor-tests: the HTML UI importers (editor/htmlui) against a local HTTP
# fixture server, built with the editor (tools/build.sh htmlui-editor-tests, Linux)
option(JASPERENGINE_HTMLUI_EDITOR_TESTS "Build jasperengine-htmlui-editor-tests (HTML UI importer tests)" OFF)
# googletest targets of the editor tree (engine/cmake/JasperTesting.cmake, docs/testing.md);
# JASPERENGINE_TESTS (engine/CMakeLists.txt) builds jasperengine-editor-tests (editor/tests)
if(JASPERENGINE_TESTS OR JASPERENGINE_HTMLUI_EDITOR_TESTS)
    include(${JASPERENGINE_ROOT}/cmake/JasperTesting.cmake)
endif()
if(JASPERENGINE_HTMLUI_EDITOR_TESTS)
    include(${EDITOR_DIR}/htmlui/tests/HtmlUiEditorTests.cmake)
endif()
# jasperengine-spritenormals-tests: the sprite normal generator core (editor/spritenormals): upstream
# parity on golden fixtures, the options, GIF/APNG decoding, recipes, the lit preview
# (-DJASPERENGINE_SPRITENORMALS_TESTS=ON and the target)
option(JASPERENGINE_SPRITENORMALS_TESTS "Build jasperengine-spritenormals-tests (sprite normal generator tests)" OFF)
if(JASPERENGINE_SPRITENORMALS_TESTS)
    include(${EDITOR_DIR}/spritenormals/tests/SpriteNormalsTests.cmake)
endif()

if(JASPERENGINE_TESTS)
    include(${EDITOR_DIR}/tests/EditorTests.cmake)
endif()

# Windows GUI-subsystem executables do not behave like normal CLI programs
# when launched from cmd.exe/PowerShell. Provide a console-subsystem twin for
# automation while keeping Unix-like platforms on a single editor binary.
#
# EXCLUDE_FROM_ALL keeps it out of the default ("all") build, so local and IDE
# builds only produce jasperengine-editor. Release builds compile it explicitly by
# name (`cmake --build --target jasperengine-editor-cmd`); the install rule below
# still installs it because they build it before installing.
if(WIN32)
    # Its own icon because both executables install to the same folder
    jasperengine_add_windows_resource(
        JASPERENGINE_EDITOR_CMD_ICON_OBJECTS
        jasperengine-editor-cmd
        "jasperengine-editor-cmd.ico"
        "jasperengine-editor-cmd.exe"
        "JasperEngine Editor (command line)"
    )

    add_executable(
        jasperengine-editor-cmd

        EXCLUDE_FROM_ALL

        ${EDITOR_DIR}/main.cpp
        ${JASPERENGINE_EDITOR_CMD_ICON_OBJECTS}
    )

    configure_jasperengine_editor_target(jasperengine-editor-cmd)
    target_link_libraries(jasperengine-editor-cmd PRIVATE jasperengine-editor-objects)
    jasperengine_add_windows_manifest(jasperengine-editor-cmd)
    add_dependencies(jasperengine-editor-cmd copy_jasperengine_editor_engine)
    list(APPEND JASPERENGINE_EDITOR_INSTALL_TARGETS jasperengine-editor-cmd)
endif()

# What `cmake --install` installs for: the release configurations, and a sanitizer tree's (Debug
# by default, tools/build.sh), whose install the MCP tests run like a normal one's
set(JASPERENGINE_INSTALL_CONFIGURATIONS Release MinSizeRel)
if(JASPERENGINE_SANITIZE)
    list(APPEND JASPERENGINE_INSTALL_CONFIGURATIONS Debug RelWithDebInfo)
endif()

# The bundle already holds the dylib and engine/.
if(APPLE)
    install(TARGETS jasperengine-editor
            CONFIGURATIONS ${JASPERENGINE_INSTALL_CONFIGURATIONS}
            BUNDLE DESTINATION bin
    )
else()
    install(TARGETS ${JASPERENGINE_EDITOR_INSTALL_TARGETS} jasperengine
            CONFIGURATIONS ${JASPERENGINE_INSTALL_CONFIGURATIONS}
            RUNTIME DESTINATION bin
            LIBRARY DESTINATION bin
            ARCHIVE DESTINATION bin
    )

    if(JASPERENGINE_MINGW_RUNTIME)
        install(FILES ${JASPERENGINE_MINGW_RUNTIME}
                CONFIGURATIONS ${JASPERENGINE_INSTALL_CONFIGURATIONS}
                DESTINATION bin
        )
    endif()

    # install(DIRECTORY) only adds and overwrites: first the files engine/ no longer has leave
    # the installed copy (an export or a script build would compile a deleted header)
    install(CODE "execute_process(COMMAND \"${CMAKE_COMMAND}\" \"-DENGINE_SOURCE=${CMAKE_SOURCE_DIR}/engine\"
                      \"-DENGINE_DEST=\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/engine\" -DPRUNE_ONLY=ON -DINSTALL_EXCLUDES=ON
                      -P \"${CMAKE_SOURCE_DIR}/CopyEditorEngine.cmake\" COMMAND_ERROR_IS_FATAL ANY)"
            CONFIGURATIONS ${JASPERENGINE_INSTALL_CONFIGURATIONS})
    # USE_SOURCE_PERMISSIONS: jasperengine.py and gradlew stay executable (install's default drops it)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/engine
            DESTINATION bin
            USE_SOURCE_PERMISSIONS
            CONFIGURATIONS ${JASPERENGINE_INSTALL_CONFIGURATIONS}
            PATTERN ".cxx" EXCLUDE
            PATTERN ".gradle" EXCLUDE
            PATTERN "build" EXCLUDE
            PATTERN ".git" EXCLUDE
            # test sources, fixtures and golden images (engine/tests only, not a vendored lib's tests)
            REGEX "/engine/tests$" EXCLUDE
            # RmlUi's own test suites: built standalone by tools/htmlui/run-rmlui-tests.sh, never by a project
            REGEX "/engine/libs/rmlui/Tests$" EXCLUDE
            # the repository's presets (build directories, tests, sanitizers): not a project's
            REGEX "/engine/CMakePresets.json$" EXCLUDE
    )
endif()

# Third-party notices next to the editor binary, so every package (the zips, the AppImage, the
# dmg's Resources) carries them: NOTICE.md, LICENSE and licenses/<library>-LICENSE for each shipped
# library, as tools/ci/check-notices.py lists them (and checks an install for: --installed)
if(APPLE)
    set(JASPERENGINE_NOTICES_DESTINATION bin/${JASPERENGINE_BUNDLE_NAME}.app/Contents/Resources)
else()
    set(JASPERENGINE_NOTICES_DESTINATION bin)
endif()
set(JASPERENGINE_CHECK_NOTICES ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/check-notices.py)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${JASPERENGINE_CHECK_NOTICES} --install-manifest
    OUTPUT_VARIABLE JASPERENGINE_LICENSE_MANIFEST
    RESULT_VARIABLE JASPERENGINE_LICENSE_MANIFEST_RESULT
)
if(NOT JASPERENGINE_LICENSE_MANIFEST_RESULT EQUAL 0)
    message(FATAL_ERROR "tools/ci/check-notices.py --install-manifest failed (${JASPERENGINE_LICENSE_MANIFEST_RESULT})")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${JASPERENGINE_CHECK_NOTICES})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.md ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
        DESTINATION ${JASPERENGINE_NOTICES_DESTINATION}
        CONFIGURATIONS ${JASPERENGINE_INSTALL_CONFIGURATIONS}
)
string(REPLACE "\n" ";" JASPERENGINE_LICENSE_MANIFEST "${JASPERENGINE_LICENSE_MANIFEST}")
foreach(entry IN LISTS JASPERENGINE_LICENSE_MANIFEST)
    if(entry MATCHES "^(.+)\\|(.+)$")
        install(FILES "${CMAKE_MATCH_1}"
                DESTINATION ${JASPERENGINE_NOTICES_DESTINATION}/licenses
                RENAME "${CMAKE_MATCH_2}"
                CONFIGURATIONS ${JASPERENGINE_INSTALL_CONFIGURATIONS}
        )
    endif()
endforeach()

# The starter library beside the editor: the editor reads it in place (FileUtils::getSoundLibraryDir),
# no player ever does. The install's folder is replaced, not merged, so a clip dropped from the
# library does not linger (sound-library verify would report it).
if(APPLE)
    set(JASPERENGINE_SOUND_LIBRARY_INSTALL_DIR bin/${JASPERENGINE_BUNDLE_NAME}.app/Contents/Resources/sound-library)
else()
    set(JASPERENGINE_SOUND_LIBRARY_INSTALL_DIR bin/sound-library)
endif()
install(CODE "
    if(CMAKE_INSTALL_CONFIG_NAME MATCHES \"^([Rr][Ee][Ll][Ee][Aa][Ss][Ee]|[Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$\")
        file(REMOVE_RECURSE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${JASPERENGINE_SOUND_LIBRARY_INSTALL_DIR}\")
    endif()
")
install(DIRECTORY ${CMAKE_SOURCE_DIR}/editor/resources/sound-library/library/
        DESTINATION ${JASPERENGINE_SOUND_LIBRARY_INSTALL_DIR}
        CONFIGURATIONS Release MinSizeRel
        OPTIONAL
        PATTERN ".gitattributes" EXCLUDE
)

# Desktop integration: a launcher entry plus the themed icons the launcher,
# taskbar and alt-tab switcher look up by the "jasperengine-editor" icon name.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/${EDITOR_DIR}/resources/linux/jasperengine-editor.desktop
            DESTINATION share/applications
            CONFIGURATIONS ${JASPERENGINE_INSTALL_CONFIGURATIONS}
    )

    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/${EDITOR_DIR}/resources/linux/com.jaspergameengine.editor.metainfo.xml
            DESTINATION share/metainfo
            CONFIGURATIONS ${JASPERENGINE_INSTALL_CONFIGURATIONS}
    )

    foreach(icon_size IN ITEMS 16 32 48 64 128 256 512)
        install(FILES ${JASPERENGINE_LINUX_ICON_DIR}/jasperengine-editor-${icon_size}.png
                DESTINATION share/icons/hicolor/${icon_size}x${icon_size}/apps
                RENAME jasperengine-editor.png
                CONFIGURATIONS ${JASPERENGINE_INSTALL_CONFIGURATIONS}
        )
    endforeach()
endif()

jasperengine_check_cxx_standards()
