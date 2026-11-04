# The editor's navigation (docs/specs/S19-navigation.md, docs/navigation.md): the components'
# scene codecs and generated code, the project settings, the Inspector, the Navigation window,
# the Scene view overlay, gizmos and nav-grid brush, the asset files and the MCP tools.

list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/StreamNavigation.cpp
    ${EDITOR_DIR}/FactoryNavigation.cpp
    ${EDITOR_DIR}/reflect/components/NavigationReflect.cpp
    ${EDITOR_DIR}/navigation/NavigationProjectSettings.cpp
    ${EDITOR_DIR}/navigation/NavigationSettingsCmd.cpp
    ${EDITOR_DIR}/navigation/NavigationEditor.cpp
    ${EDITOR_DIR}/navigation/NavGridCellsCmd.cpp
    ${EDITOR_DIR}/navigation/NavGridBrush.cpp
    ${EDITOR_DIR}/navigation/NavigationWindow.cpp
    ${EDITOR_DIR}/navigation/NavigationOverlay.cpp
    ${EDITOR_DIR}/navigation/NavPathTester.cpp
    ${EDITOR_DIR}/navigation/NavWalkabilityProbe.cpp
    ${EDITOR_DIR}/navigation/NavigationFindings.cpp
    ${EDITOR_DIR}/mcp/tools/NavigationTools.cpp
    ${EDITOR_DIR}/window/InspectorNavigation.cpp
)
