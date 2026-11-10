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

struct lua_State;

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
        std::vector<LogLevel> levels;    // empty: every level
        std::vector<std::string> categories;  // empty: every category
        int hasSource = -1;              // 1: only entries with a source, 0: only without, -1: both
        std::string contains;            // case-insensitive substring
        std::string regex;               // ECMAScript regex (case-insensitive), already validated
        size_t maxMessageChars = 4000;   // longer messages are cut (the tail is kept for build output)
    };

    struct ConsoleResult {
        Json entries = Json::array();    // [{id, type, level, category, message, time, frame, source?}]
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
    // An entry as read_console returns it
    Json consoleEntryJson(const LogEntry& entry, size_t maxChars);

    // ------------------------------------------------------------ views (ViewTools.cpp)

    // True when the PNG is one colour, within noise (a blank render); `colour`: its first pixel, "#rrggbb"
    bool isFlatPng(const std::vector<uint8_t>& png, std::string& colour);

    struct CaptureRequest {
        std::string view = "game";       // game | scene | editor
        Json scene;                      // scene reference (scene view), null: selected/playing
        int maxWidth = 1024;             // 0: full size
        std::string savePath;            // also write the PNG here (project-relative or absolute)
        bool focus = true;               // bring the view's tab to the front when it must render
        int timeoutMs = 20000;
        bool deviceOverlay = true;       // game view with a simulated device: its screen drawn over the image
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
            {"anyOf", Json::array({Json{{"type", "boolean"}}, Json{{"type", "string"}, {"enum", {"game", "scene", "editor"}}}})}
        };
    }
    inline Json maxWidthArgSchema(int maximum, int defaultWidth) {
        return Json{{"type", "integer"},
                    {"minimum", 0}, {"maximum", maximum}, {"default", defaultWidth}};
    }

    // The scene the Scene view tools act on (given, else the playing one, else the selected
    // one; open and loaded), and a run once its view is laid out (focus: brought forward)
    SceneProject* resolveSceneViewScene(Project* project, const Json& ref, std::string& error);
    void whenSceneViewShown(const CallPtr& call, uint32_t sceneId, bool focus, int timeoutMs,
                            std::function<void(bool ready, const std::string& error)> then);
    // text + data, with a Scene view image when the call asked for one (screenshot: true)
    void replyWithSceneShot(const CallPtr& call, uint32_t sceneId, const std::string& text, Json data);
    // the Scene view camera as set_scene_camera returns it
    Json sceneViewCameraJson(SceneProject* sceneProject);

    // ------------------------------------------------------------ play (PlayTools.cpp)

    // {state, scene, ...} of the play session (or "stopped")
    Json playStateJson(McpContext& ctx);
    // The scene the Game view shows: the playing scene, else the selected one (behind a prefab stage)
    SceneProject* gameViewScene(Project* project);
    // The scene play start runs by default: the start scene if open, else gameViewScene
    SceneProject* defaultPlayScene(Project* project);

    // A scene of the running game: null ref → the played (main) scene; otherwise a scene id,
    // name or path, mapped to its Play copy when the game runs it as a child scene (or a Play
    // copy id). Fails when nothing plays.
    SceneProject* resolveRuntimeScene(Project* project, const Json& ref, std::string& error);
    // A GameObject of a scene, including (while it plays) those the game created at runtime
    Entity resolveLiveEntity(Project* project, SceneProject* scene, const Json& ref, std::string& error);
    // The scene's GameObjects, plus what the running game created
    std::vector<Entity> liveEntities(Project* project, SceneProject* scene);

    // Lets an auto_paused game run (play auto_pause holds it between calls) while it lives: a capture
    // stepping the paused game with Project::step (capture_contact_sheet's scene mode, S48)
    class GameAdvance {
    public:
        explicit GameAdvance(McpContext& ctx);
        ~GameAdvance();
        GameAdvance(const GameAdvance&) = delete;
        GameAdvance& operator=(const GameAdvance&) = delete;
    private:
        McpContext* ctx = nullptr;
    };

    // The Lua value at `index` as JSON, as eval_lua returns values (tables nested maxDepth deep, at
    // most maxItems entries each); userdata with a __pairs metamethod (HTML UI data-model proxies)
    // are walked like tables. For inspect_ui models.
    Json luaToJson(Project* project, lua_State* L, int index, int maxDepth, size_t maxItems);

}
