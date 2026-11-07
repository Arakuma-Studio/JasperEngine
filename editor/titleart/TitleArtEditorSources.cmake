# Title Art's editor sources (docs/specs/S80-title-art-core.md §6.15), included by the root
# CMakeLists.txt into the editor's sources. Globbed, so each title spec adds files without editing
# CMake: editor/titleart/ and its folders (S80 root; S81 runtime/, S82 designer/ and fonts/, S83
# fx/, S84 library/, S85 anim/, S86 three_d/, S87 localisation/ and interchange/), the program's
# windows, MCP tools, preview renderer and reload handler, and S80's image diff.

set(jasperengineTitleArtEditorDir ${CMAKE_CURRENT_LIST_DIR}/..)
file(GLOB_RECURSE JASPERENGINE_EDITOR_TITLE_ART_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp)
file(GLOB jasperengineTitleArtEditorFiles CONFIGURE_DEPENDS
    ${jasperengineTitleArtEditorDir}/window/TitleDesigner*.cpp
    ${jasperengineTitleArtEditorDir}/window/*TitleArt*.cpp
    ${jasperengineTitleArtEditorDir}/mcp/tools/TitleArtTools*.cpp
    ${jasperengineTitleArtEditorDir}/mcp/tools/ImageDiffTools.cpp
    ${jasperengineTitleArtEditorDir}/util/ImageDiff.cpp
    ${jasperengineTitleArtEditorDir}/render/preview/TitleArt*.cpp
    ${jasperengineTitleArtEditorDir}/reload/handlers/TitleArt*.cpp)
list(APPEND JASPERENGINE_EDITOR_TITLE_ART_SOURCES ${jasperengineTitleArtEditorFiles})
list(SORT JASPERENGINE_EDITOR_TITLE_ART_SOURCES)
list(APPEND JASPERENGINE_EDITOR_SOURCES ${JASPERENGINE_EDITOR_TITLE_ART_SOURCES})

# Unity build: several streams' files keep per-file helpers with the same names
if(JASPERENGINE_EDITOR_TITLE_ART_SOURCES)
    jasperengine_skip_unity_build(
        REASON "the Title Art editor sources: per-file helper sets with the same names"
        SOURCES ${JASPERENGINE_EDITOR_TITLE_ART_SOURCES})
endif()

# S81's built-in data, embedded in the editor (editor/titleart/runtime/TitleRuntimeResources.cpp, the
# only includer): the Joyo Kanji preset of a title's Characters (R53) and the OFL text of the font
# credits (R57)
set(jasperengineTitleRuntimeHeader "// Generated from editor/resources/titleart by TitleArtEditorSources.cmake. Do not edit.\n#pragma once\n\n")
foreach(jasperengineTitleRuntimeEntry IN ITEMS "joyo_kanji|charsets/joyo-kanji.txt" "ofl_1_1|OFL-1.1.txt")
    string(REPLACE "|" ";" jasperengineTitleRuntimeEntry "${jasperengineTitleRuntimeEntry}")
    list(GET jasperengineTitleRuntimeEntry 0 jasperengineTitleRuntimeName)
    list(GET jasperengineTitleRuntimeEntry 1 jasperengineTitleRuntimeFile)
    set(jasperengineTitleRuntimeFile "${jasperengineTitleArtEditorDir}/resources/titleart/${jasperengineTitleRuntimeFile}")
    file(READ "${jasperengineTitleRuntimeFile}" jasperengineTitleRuntimeHex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," jasperengineTitleRuntimeBytes "${jasperengineTitleRuntimeHex}")
    string(APPEND jasperengineTitleRuntimeHeader "static const unsigned char title_runtime_${jasperengineTitleRuntimeName}[] = {${jasperengineTitleRuntimeBytes}0x00};\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${jasperengineTitleRuntimeFile}")
endforeach()
file(WRITE "${GENERATED_DIR}/titleart_runtime_resources.h.in" "${jasperengineTitleRuntimeHeader}")
configure_file("${GENERATED_DIR}/titleart_runtime_resources.h.in" "${GENERATED_DIR}/titleart_runtime_resources.h" COPYONLY)
