# Game UI localization's editor sources (docs/specs/game-ui-localization.md D1), included by the root
# CMakeLists.txt into the editor's sources: editor/localization/ (String Tables, their files, ops,
# undo, interchange, cooking), the Localization Tables window and the localization MCP tools.
# Globbed, so later work (dialogue-graphs.md) adds files without editing CMake.

set(jasperengineLocalizationEditorDir ${CMAKE_CURRENT_LIST_DIR}/..)
file(GLOB JASPERENGINE_EDITOR_LOCALIZATION_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp
    ${jasperengineLocalizationEditorDir}/window/LocalizationTables*.cpp
    ${jasperengineLocalizationEditorDir}/window/InspectorLocalization.cpp
    ${jasperengineLocalizationEditorDir}/mcp/tools/LocalizationTools*.cpp)
list(SORT JASPERENGINE_EDITOR_LOCALIZATION_SOURCES)
list(APPEND JASPERENGINE_EDITOR_SOURCES ${JASPERENGINE_EDITOR_LOCALIZATION_SOURCES})
