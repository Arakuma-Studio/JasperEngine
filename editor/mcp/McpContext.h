// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "util/SafeWrite.h"

#include <filesystem>

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

    // The write scope MCP work runs in (util/SafeWrite.h, safewrite::Scope): the open project's
    // directory ("" without a project). McpServer opens one around every tool's main-thread run,
    // its deferred continuations and resource reads; a tool's own continuations (enqueued main
    // thread tasks, worker threads that write) open one themselves. Main thread (or the worker it
    // is made on: the scope is per thread).
    // Strict (safewrite::Scope): a write outside the project is refused too, but the editor's own
    // folders (its settings, the temporary folder: registered when the server starts); a tool that
    // switches projects runs in a non-strict one.
    class McpWriteScope : public safewrite::Scope {
    public:
        McpWriteScope();
        explicit McpWriteScope(std::vector<std::filesystem::path> roots, bool strict = true) : safewrite::Scope(std::move(roots), strict) {}
        // the open project's directory ("" when none)
        static std::filesystem::path projectRoot();
        // the open project's trusted roots: its directory and its configured assets directory
        // (project.yaml assetsDir as loaded; each resolved once by the checks), empty when none
        static std::vector<std::filesystem::path> projectRoots();
    };

    // What tool handlers reach the editor through. Main thread only (except server()/jobs()).
    class McpContext {
    public:
        McpContext(App& app, McpServer& server);

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
