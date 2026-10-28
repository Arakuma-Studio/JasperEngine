// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The scripts' compile and reload state the toolbar badge shows (ScriptStatusBadge) and MCP
// get_script_status / editor_status.scripts report: one row for Lua, one for the C++ scripts and
// one for the C++ editor scripts. Set on the main thread; a build on a worker posts through
// Generator's ThreadSafeSignal and is applied by update().

#include "mcp/McpJson.h"
#include "util/Signal.h"
#include "script/ToolchainFingerprint.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct ScriptBuildEvent;

    struct ScriptFinding {
        std::string file;      // project-relative when it is in the project
        int line = 0;
        int column = 0;
        std::string message;
    };

    enum class LuaScriptState { Ok, Reloading, Error };
    enum class CppScriptState { None, Stale, Building, UpToDate, Failed, Refused };

    struct ScriptStatusRow {
        std::vector<ScriptFinding> findings;      // errors (the first ones), newest state's
        std::string message;                      // one line: "build failed — using last good build"
        float fraction = -1.0f;                   // Building: 0..1, -1 unknown
        bool softWarning = false;                 // loaded with a fingerprint warning, or old findings
        std::chrono::system_clock::time_point lastTime{};  // last build or reload
        double lastSeconds = 0.0;                 // how long it took
        bool hasLastGood = false;                 // a library from before is still loaded / kept
    };

    struct ScriptStatusState {
        LuaScriptState lua = LuaScriptState::Ok;
        ScriptStatusRow luaRow;
        CppScriptState cpp = CppScriptState::None;
        ScriptStatusRow cppRow;
        CppScriptState editorCpp = CppScriptState::None;   // the C++ editor-scripts library (EditorScripts)
        ScriptStatusRow editorCppRow;
        // the last fingerprint check of a C++ library (Play connect, reload, editor scripts)
        bool fingerprintChecked = false;
        bool fingerprintOk = true;
        std::vector<FingerprintMismatch> fingerprintMismatches;
        uint64_t revision = 0;                    // changes with every update
    };

    class ScriptStatus {
    public:
        static ScriptStatus& get();

        const ScriptStatusState& state() const { return current; }

        void setLua(LuaScriptState state, std::vector<ScriptFinding> findings = {}, std::string message = {});
        // A Lua reload finished (ok: every changed script swapped)
        void luaReloaded(double seconds);
        void setCpp(CppScriptState state, std::vector<ScriptFinding> findings = {}, std::string message = {});
        void setCppProgress(float fraction);
        void cppBuilt(bool ok, double seconds, std::vector<ScriptFinding> findings, bool keptLastGood);
        void setEditorCpp(CppScriptState state, std::vector<ScriptFinding> findings = {}, std::string message = {});
        void setFingerprint(bool ok, std::vector<FingerprintMismatch> mismatches);
        // Whether the project has C++ scripts at all (None vs the other states)
        void setHasCppScripts(bool has);
        bool hasCppScripts() const { return cppScriptsPresent; }

        // From any thread (the build's output, on a worker): applied by update()
        void postCppProgress(float fraction);
        // Follows every script build (Generator::buildEvents) from now on
        void followBuilds();

        // Main thread, every editor frame: applies what workers posted; true when it changed
        bool update();

        // The badge opens the Console when a row newly fails (OE behaviour): true once per new failure
        bool takeNewFailure();

        // Signals the state changed (main thread)
        Signal<> changed;

        // ---- names, for the badge's text and MCP
        static const char* luaStateName(LuaScriptState state);    // ok, reloading, error
        static const char* cppStateName(CppScriptState state);    // none, stale, building, up_to_date, failed, refused
        // "Lua: 2 errors · C++: build failed — using last good build"
        std::string summary() const;
        // get_script_status and editor_status.scripts: {lua, cpp, editor_cpp, fingerprint, summary}.
        // Main thread. Applies what builds posted first (update()): a build that ended after this
        // frame's update() is reported as ended, not as still building
        mcp::Json toJson();

    private:
        ScriptStatus() = default;
        void bump(bool failedNow);

        ScriptStatusState current;
        bool cppScriptsPresent = false;
        bool newFailure = false;
        std::mutex postMutex;
        std::optional<float> postedFraction;
        std::vector<ScriptBuildEvent> postedBuilds;   // (complete in the .cpp)
        Subscription buildSubscription;
        void applyBuild(const ScriptBuildEvent& event);
    };

}
