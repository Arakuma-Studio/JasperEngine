# Dialogue graphs' editor sources (docs/specs/dialogue-graphs.md D6), included by the root
# CMakeLists.txt into the editor's sources: editor/dialogue/ (the edit hooks and lint context, the
# walker shared with run_dialogue, the Dialogue window and its preview). Globbed.

file(GLOB JASPERENGINE_EDITOR_DIALOGUE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*.cpp)
list(SORT JASPERENGINE_EDITOR_DIALOGUE_SOURCES)
list(APPEND JASPERENGINE_EDITOR_SOURCES ${JASPERENGINE_EDITOR_DIALOGUE_SOURCES})
