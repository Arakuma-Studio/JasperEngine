# Title Art's editor sources (docs/specs/S80-title-art-core.md §6.15), included by the root
# CMakeLists.txt into the editor's sources. Globbed, so each title spec adds files without editing
# CMake: editor/titleart/ and its folders (S80 root; S81 runtime/, S82 designer/ and fonts/, S83
# fx/, S84 library/, S85 anim/, S86 three_d/, S87 localisation/ and interchange/), the program's
# windows, MCP tools, preview renderer and reload handler, S80's image diff and S82's RampField
# widget (editor/widget/RampField.cpp).

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
    ${jasperengineTitleArtEditorDir}/reload/handlers/TitleArt*.cpp
    ${jasperengineTitleArtEditorDir}/widget/RampField.cpp)
list(APPEND JASPERENGINE_EDITOR_TITLE_ART_SOURCES ${jasperengineTitleArtEditorFiles})
list(SORT JASPERENGINE_EDITOR_TITLE_ART_SOURCES)
list(APPEND JASPERENGINE_EDITOR_SOURCES ${JASPERENGINE_EDITOR_TITLE_ART_SOURCES})

# Unity build: several streams' files keep per-file helpers with the same names
if(JASPERENGINE_EDITOR_TITLE_ART_SOURCES)
    jasperengine_skip_unity_build(
        REASON "the Title Art editor sources: per-file helper sets with the same names"
        SOURCES ${JASPERENGINE_EDITOR_TITLE_ART_SOURCES})
endif()

# S82's search terms (editor/resources/titleart/search-terms.json, R85) embedded in the editor:
# title_search_terms.h, included only by editor/titleart/designer/TitleSearchTerms.cpp
set(jasperengineTitleSearchTermsFile ${jasperengineTitleArtEditorDir}/resources/titleart/search-terms.json)
file(READ "${jasperengineTitleSearchTermsFile}" jasperengineTitleSearchTermsHex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," jasperengineTitleSearchTermsBytes "${jasperengineTitleSearchTermsHex}")
file(WRITE "${GENERATED_DIR}/title_search_terms.h.in"
    "// Generated from editor/resources/titleart/search-terms.json by editor/titleart/TitleArtEditorSources.cmake. Do not edit.\n#pragma once\n\n"
    "static const unsigned char title_search_terms_json[] = {${jasperengineTitleSearchTermsBytes}0x00};\n"
    "static const unsigned int title_search_terms_json_size = sizeof(title_search_terms_json) - 1;\n")
configure_file("${GENERATED_DIR}/title_search_terms.h.in" "${GENERATED_DIR}/title_search_terms.h" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${jasperengineTitleSearchTermsFile}")
