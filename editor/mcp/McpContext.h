// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>

namespace jasperengine::editor {
    class App;
    class Project;
    class ProjectWindow;
    class SceneWindow;
    class ConsoleWindow;
    class CodeEditor;
    class HierarchyWindow;
    class InspectorWindow;
    class GameWindow;
    class AnimationWindow;
    class TerrainEditWindow;
    class PlayInput;
}

namespace jasperengine::editor::mcp {

    class McpServer;
    class McpJobs;

    // What tool handlers reach the editor through. Main thread only (except server()/jobs()).
    class McpContext {
    public:
        McpContext(App& app, McpServer& server);
        ~McpContext();

        App& app() const { return appRef; }
        Project* project() const;
        McpServer& server() const { return serverRef; }
        McpJobs& jobs() const;

        ProjectWindow* projectWindow() const;
        SceneWindow* sceneWindow() const;
        ConsoleWindow* consoleWindow() const;
        CodeEditor* codeEditor() const;
        HierarchyWindow* hierarchyWindow() const;
        InspectorWindow* inspectorWindow() const;
        GameWindow* gameWindow() const;
        AnimationWindow* animationWindow() const;
        TerrainEditWindow* terrainEditWindow() const;
        PlayInput* playInput() const;

        // A scene is playing, paused, loading or stopping
        bool isPlaying() const;

    private:
        App& appRef;
        McpServer& serverRef;
    };

}
