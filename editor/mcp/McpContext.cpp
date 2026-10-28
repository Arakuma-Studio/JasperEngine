// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpContext.h"

#include "Backend.h"

#include "McpServer.h"

#include "App.h"

using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

McpContext::McpContext(App& app, McpServer& server)
    : appRef(app)
    , serverRef(server) {
}

std::filesystem::path McpWriteScope::projectRoot() {
    const App& app = Backend::getApp();
    Project* project = const_cast<App&>(app).getProject();
    return project ? project->getProjectPath() : std::filesystem::path();
}

std::vector<std::filesystem::path> McpWriteScope::projectRoots() {
    const App& app = Backend::getApp();
    Project* project = const_cast<App&>(app).getProject();
    if (!project || project->getProjectPath().empty()) return {};
    std::vector<std::filesystem::path> roots = {project->getProjectPath()};
    const std::filesystem::path assets = project->getAssetsPath();
    if (!assets.empty() && assets.lexically_normal() != project->getProjectPath().lexically_normal()) roots.push_back(assets);
    return roots;
}

McpWriteScope::McpWriteScope() : safewrite::Scope(projectRoots(), true) {}

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
