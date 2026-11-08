# The editor tools (docs/specs/S27-editor-tools.md): the curve and gradient codec, the colour,
# curve and gradient fields and their windows, swatch and curve preset libraries, the screen
# colour readers of the eyedropper, the Project Hub with its templates and importers, the
# Recorder, and their MCP tools.

list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/stream/CurveCodec.cpp
)

list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/widget/FieldEdit.cpp
    ${EDITOR_DIR}/widget/ColorMath.cpp
    ${EDITOR_DIR}/widget/SwatchLibrary.cpp
    ${EDITOR_DIR}/widget/ColorPickerWindow.cpp
    ${EDITOR_DIR}/widget/CurveEditorOps.cpp
    ${EDITOR_DIR}/widget/GradientEditorOps.cpp
    ${EDITOR_DIR}/widget/CurveEditor.cpp
    ${EDITOR_DIR}/widget/CurvePresets.cpp
    ${EDITOR_DIR}/widget/CurveField.cpp
    ${EDITOR_DIR}/widget/GradientField.cpp
    # document rows (docs/specs/S82-title-designer.md R20-R23)
    ${EDITOR_DIR}/widget/DocRow.cpp
)

# the eyedropper's screen reads (editor/platform/ScreenColor.h)
# and the Shift key at launch (editor/platform/LaunchKeys.h)
if(APPLE)
    list(APPEND JASPERENGINE_EDITOR_SOURCES ${EDITOR_DIR}/platform/ScreenColorMac.mm ${EDITOR_DIR}/platform/LaunchKeysMac.mm)
    set_source_files_properties(${EDITOR_DIR}/platform/ScreenColorMac.mm ${EDITOR_DIR}/platform/LaunchKeysMac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
elseif(WIN32)
    list(APPEND JASPERENGINE_EDITOR_SOURCES ${EDITOR_DIR}/platform/ScreenColorWin.cpp ${EDITOR_DIR}/platform/LaunchKeysWin.cpp)
else()
    list(APPEND JASPERENGINE_EDITOR_SOURCES ${EDITOR_DIR}/platform/ScreenColorX11.cpp ${EDITOR_DIR}/platform/LaunchKeysX11.cpp)
endif()

# The Recorder (editor/recorder)
list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/recorder/RecorderSettings.cpp
    ${EDITOR_DIR}/recorder/RecorderSession.cpp
    ${EDITOR_DIR}/recorder/RecorderWindow.cpp
)

# the MCP tools (editor_tools, and the hub's core ones)
list(APPEND JASPERENGINE_EDITOR_SOURCES ${EDITOR_DIR}/mcp/tools/EditorToolsTools.cpp)

# miniz (libs/miniz) is the editor's already (CMakeLists.txt, S30): the Project Hub's zip import uses it too

# The Project Hub (editor/hub): paths, recent projects, templates, importers and its window
list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/hub/HubPaths.cpp
    ${EDITOR_DIR}/hub/RecentProjects.cpp
    ${EDITOR_DIR}/hub/StagingFolders.cpp
    ${EDITOR_DIR}/hub/TemplateCatalog.cpp
    ${EDITOR_DIR}/hub/ProjectImporter.cpp
    ${EDITOR_DIR}/hub/ProjectHubWindow.cpp
)

# The built-in project templates (editor/resources/templates/<id>/**) embedded in the editor
# (editor/hub/TemplateCatalog.cpp, the only includer): every file by its path "<id>/<relative>"
set(PROJECT_TEMPLATES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${EDITOR_DIR}/resources/templates")
file(GLOB_RECURSE PROJECT_TEMPLATE_FILES CONFIGURE_DEPENDS "${PROJECT_TEMPLATES_DIR}/*")
list(SORT PROJECT_TEMPLATE_FILES)
set(project_templates_header "// Generated from editor/resources/templates by editor/EditorToolsSources.cmake. Do not edit.\n#pragma once\n\n")
set(project_templates_entries "")
set(project_template_index 0)
foreach(project_template_file IN LISTS PROJECT_TEMPLATE_FILES)
    file(RELATIVE_PATH project_template_path "${PROJECT_TEMPLATES_DIR}" "${project_template_file}")
    file(READ "${project_template_file}" project_template_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," project_template_bytes "${project_template_hex}")
    string(APPEND project_templates_header "static const unsigned char project_template_${project_template_index}[] = {${project_template_bytes}0x00};\n")
    string(APPEND project_templates_entries "    { \"${project_template_path}\", project_template_${project_template_index}, sizeof(project_template_${project_template_index}) - 1 },\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${project_template_file}")
    math(EXPR project_template_index "${project_template_index} + 1")
endforeach()
string(APPEND project_templates_header "\nstruct ProjectTemplateEmbeddedFile { const char* path; const unsigned char* data; unsigned int size; };\n")
string(APPEND project_templates_header "static const ProjectTemplateEmbeddedFile project_template_embedded_files[] = {\n${project_templates_entries}    { nullptr, nullptr, 0 }\n};\n")
file(WRITE "${GENERATED_DIR}/project_templates.h.in" "${project_templates_header}")
configure_file("${GENERATED_DIR}/project_templates.h.in" "${GENERATED_DIR}/project_templates.h" COPYONLY)
