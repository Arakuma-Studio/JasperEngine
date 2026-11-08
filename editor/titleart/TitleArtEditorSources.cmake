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

# S83's effect and generator presets (editor/resources/titleart/fx-presets/<type>.yaml), embedded as
# title_fx_presets.h (editor/titleart/fx/FxPresets.cpp parses them)
file(GLOB jasperengineTitleFxPresetFiles CONFIGURE_DEPENDS ${jasperengineTitleArtEditorDir}/resources/titleart/fx-presets/*.yaml)
list(SORT jasperengineTitleFxPresetFiles)
set(jasperengineTitleFxPresetsHeader "// Generated from editor/resources/titleart/fx-presets by editor/titleart/TitleArtEditorSources.cmake. Do not edit.\n#pragma once\n\n")
set(jasperengineTitleFxPresetEntries "")
set(jasperengineTitleFxPresetIndex 0)
foreach(presetFile IN LISTS jasperengineTitleFxPresetFiles)
    get_filename_component(presetName "${presetFile}" NAME)
    file(READ "${presetFile}" presetHex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," presetBytes "${presetHex}")
    string(APPEND jasperengineTitleFxPresetsHeader "static const unsigned char title_fx_preset_${jasperengineTitleFxPresetIndex}[] = {${presetBytes}0x00};\n")
    string(APPEND jasperengineTitleFxPresetEntries "    { \"${presetName}\", title_fx_preset_${jasperengineTitleFxPresetIndex}, sizeof(title_fx_preset_${jasperengineTitleFxPresetIndex}) - 1 },\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${presetFile}")
    math(EXPR jasperengineTitleFxPresetIndex "${jasperengineTitleFxPresetIndex} + 1")
endforeach()
string(APPEND jasperengineTitleFxPresetsHeader "\nstruct TitleFxPresetEmbeddedFile { const char* name; const unsigned char* data; unsigned int size; };\n")
string(APPEND jasperengineTitleFxPresetsHeader "static const TitleFxPresetEmbeddedFile title_fx_preset_embedded_files[] = {\n${jasperengineTitleFxPresetEntries}    { nullptr, nullptr, 0 }\n};\n")
file(WRITE "${GENERATED_DIR}/title_fx_presets.h.in" "${jasperengineTitleFxPresetsHeader}")
configure_file("${GENERATED_DIR}/title_fx_presets.h.in" "${GENERATED_DIR}/title_fx_presets.h" COPYONLY)

# the Pachislop effect id map (S83 §6.9), embedded as title_pachislop_fx_map.h
set(jasperengineTitleFxMapFile ${jasperengineTitleArtEditorDir}/resources/titleart/pachislop-fx-map.yaml)
file(READ "${jasperengineTitleFxMapFile}" jasperengineTitleFxMapHex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," jasperengineTitleFxMapBytes "${jasperengineTitleFxMapHex}")
file(WRITE "${GENERATED_DIR}/title_pachislop_fx_map.h.in"
    "// Generated from editor/resources/titleart/pachislop-fx-map.yaml by editor/titleart/TitleArtEditorSources.cmake. Do not edit.\n#pragma once\n\nstatic const unsigned char title_pachislop_fx_map[] = {${jasperengineTitleFxMapBytes}0x00};\n")
configure_file("${GENERATED_DIR}/title_pachislop_fx_map.h.in" "${GENERATED_DIR}/title_pachislop_fx_map.h" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${jasperengineTitleFxMapFile}")

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
