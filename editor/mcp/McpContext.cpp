// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpContext.h"

#include "McpServer.h"

#include "App.h"

using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

McpContext::McpContext(App& app, McpServer& server)
    : appRef(app)
    , serverRef(server) {
}

Project* McpContext::project() const {
    return appRef.getProject();
}

McpJobs& McpContext::jobs() const {
    return serverRef.jobs();
}

ProjectWindow* McpContext::projectWindow() const { return appRef.getProjectWindow(); }
SceneWindow* McpContext::sceneWindow() const { return appRef.getSceneWindow(); }
ConsoleWindow* McpContext::consoleWindow() const { return appRef.getConsoleWindow(); }
CodeEditor* McpContext::codeEditor() const { return appRef.getCodeEditor(); }
HierarchyWindow* McpContext::hierarchyWindow() const { return appRef.getHierarchyWindow(); }
InspectorWindow* McpContext::inspectorWindow() const { return appRef.getInspectorWindow(); }
GameWindow* McpContext::gameWindow() const { return appRef.getGameWindow(); }
AnimationWindow* McpContext::animationWindow() const { return appRef.getAnimationWindow(); }
TerrainEditWindow* McpContext::terrainEditWindow() const { return appRef.getTerrainEditWindow(); }
SystemsWindow* McpContext::systemsWindow() const { return appRef.getSystemsWindow(); }
PlayInput* McpContext::playInput() const { return appRef.getPlayInput(); }

bool McpContext::isPlaying() const {
    return appRef.getProject()->isAnyScenePlaying();
}
