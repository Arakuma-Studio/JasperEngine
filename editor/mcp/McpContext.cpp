// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpContext.h"

#include "McpServer.h"
#include "legacy/EditorActionExecutor.h"

#include "App.h"

using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

McpContext::McpContext(App& app, McpServer& server)
    : appRef(app)
    , serverRef(server) {
}

McpContext::~McpContext() = default;

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
PlayInput* McpContext::playInput() const { return appRef.getPlayInput(); }

bool McpContext::isPlaying() const {
    return appRef.getProject()->isAnyScenePlaying();
}

legacy::EditorActionExecutor& McpContext::legacyExecutor() {
    if (!legacy) {
        legacy = std::make_unique<legacy::EditorActionExecutor>(appRef.getProject(), appRef.getProjectWindow());
    }
    return *legacy;
}
