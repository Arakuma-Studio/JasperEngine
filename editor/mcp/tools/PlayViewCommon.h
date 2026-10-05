// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// W3 helpers shared by ConsoleTools.cpp, PlayTools.cpp and ViewTools.cpp: console reads,
// view captures and the play-session state. Main thread only.

#include "mcp/McpCall.h"
#include "mcp/McpJson.h"

#include "Out.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
    struct SceneProject;
}

namespace jasperengine::editor::mcp {
    class McpContext;
}

namespace jasperengine::editor::mcp::w3 {

    // ------------------------------------------------------------ console (ConsoleTools.cpp)

    struct ConsoleQuery {
        uint64_t since = 0;              // entries with id > since (0: from the oldest kept)
        bool tail = false;               // since == 0: the newest `limit` entries instead of the oldest
        size_t limit = 100;              // 0: no limit
        bool onlyProblems = false;       // warnings, errors and build output (compiler messages)
        bool includeNoise = false;       // "Shader ... generated successfully" build lines
        std::vector<LogType> types;      // empty: every type
        std::string contains;            // case-insensitive substring
        std::string regex;               // ECMAScript regex (case-insensitive), already validated
        size_t maxMessageChars = 4000;   // longer messages are cut (the tail is kept for build output)
    };

    struct ConsoleResult {
        Json entries = Json::array();    // [{id, type, message, time}]
        uint64_t nextCursor = 0;         // pass as `since` to continue
        bool more = false;               // the limit cut the read: call again with nextCursor
        size_t skippedNoise = 0;
        size_t errors = 0;
        size_t warnings = 0;
    };

    ConsoleResult queryConsole(const ConsoleQuery& query);
    bool isNoiseLog(const LogEntry& entry);
    bool isProblemLog(const LogEntry& entry);
    // "[error] message" lines, at most maxChars (then "... N more")
    std::string formatConsoleText(const Json& entries, size_t maxChars = 6000);
    // Parses "info"/"warning"/... ; false for an unknown name
    bool parseLogType(const std::string& name, LogType& out);

    // ------------------------------------------------------------ views (ViewTools.cpp)

    struct CaptureRequest {
        std::string view = "game";       // game | scene | editor
        Json scene;                      // scene reference (scene view), null: selected/playing
        int maxWidth = 1024;             // 0: full size
        std::string savePath;            // also write the PNG here (project-relative or absolute)
        bool focus = true;               // bring the view's tab to the front when it must render
        int timeoutMs = 20000;
    };

    struct CaptureResult {
        bool ok = false;
        std::string error;
        ToolImage image;
        Json info = Json::object();      // view, width, height, source size, scene, ...
        std::string note;                // e.g. "the image is one flat colour"
    };

    // Renders (when needed) and reads the view; done runs on the main thread, exactly once.
    // The call must stay deferred until then (this uses its waitUntil / afterFrames).
    void captureView(const CallPtr& call, const CaptureRequest& request, std::function<void(CaptureResult)> done);

    // Parses the shared screenshot arguments of a tool ({screenshot: bool|"game"|"scene", max_width})
    bool parseScreenshotOption(const Json& args, CaptureRequest& request, bool& wanted, std::string& error);

    // Tool schemas of those arguments (and of max_width elsewhere)
    inline Json screenshotArgSchema() {
        return Json{
            {"description", "Also return an image (true: game)."},
            {"oneOf", Json::array({Json{{"type", "boolean"}}, Json{{"type", "string"}, {"enum", {"game", "scene", "editor"}}}})}
        };
    }
    inline Json maxWidthArgSchema(int maximum, int defaultWidth) {
        return Json{{"type", "integer"}, {"description", "0 = full size."},
                    {"minimum", 0}, {"maximum", maximum}, {"default", defaultWidth}};
    }

    // ------------------------------------------------------------ play (PlayTools.cpp)

    // {state, scene, ...} of the play session (or "stopped")
    Json playStateJson(McpContext& ctx);
    // The scene the Game view shows: the playing scene, else the selected one (behind a prefab stage)
    SceneProject* gameViewScene(Project* project);

    // A scene of the running game: null ref → the played (main) scene; otherwise a scene id,
    // name or path, mapped to its Play copy when the game runs it as a child scene (or a Play
    // copy id). Fails when nothing plays.
    SceneProject* resolveRuntimeScene(Project* project, const Json& ref, std::string& error);
    // A GameObject of a scene, including (while it plays) those the game created at runtime
    Entity resolveLiveEntity(Project* project, SceneProject* scene, const Json& ref, std::string& error);
    // The scene's GameObjects, plus what the running game created
    std::vector<Entity> liveEntities(Project* project, SceneProject* scene);

}
