// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Scripts saved while a scene plays (S23 R1-R13): Lua scripts and modules are checked on a
// background job, then swapped into the live instances at the frame boundary
// (lua_reload::reloadScript / reloadModule); C++ scripts, when Preferences > Scripting > Reload
// C++ Scripts During Play is on, are rebuilt in the background and their library swapped
// (Project::applyCppReload). A broken file keeps the old code running. The sources of changes:
// the file watcher and every editor write (HotReloadService's script hook), the Code Editor's
// save, MCP write_file / create_script / sync_files / reload_scripts.
//
// Main thread only, except the check jobs it starts.

#include "util/ScriptStatus.h"

#include "jobs/Task.h"
#include "mcp/McpJson.h"
#include "script/ScriptProperty.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class ScriptReloadService {
    public:
        static ScriptReloadService& get();

        // Project open: installs the script hook of the hot reload (HotReloadService)
        void attach(Project* project);
        void detach();

        // Preferences > Scripting > Script Changes While Playing
        enum class Policy { ReloadAndContinue, AfterStop, StopAndReload };
        static Policy policy();
        static bool cppReloadEnabled();

        // A script file changed (absolute path). True when it is taken now: a Lua file (and,
        // when enabled, a C++ source or header of the script directories) while a scene plays
        // with Reload And Continue Playing. False: it waits for Play to stop, as before.
        // Stop Playing And Reload stops Play here (and returns false).
        bool notifyChanged(const std::filesystem::path& path);

        // Every editor frame, before the Engine runs the frame: debounced changes start their
        // check, finished checks swap the new code in
        void update();
        // Something is queued or in flight (the loop keeps drawing frames)
        bool busy() const;
        bool idleFor(const std::vector<std::filesystem::path>& paths) const;

        // MCP reload_scripts: the paths (all changed scripts when empty) now, without the debounce
        void requestReload(const std::vector<std::filesystem::path>& paths, bool lua, bool cpp);
        // Play stopped (or starts): queued work is dropped
        void playEnded();

        // One reload, as reload_scripts and external_changes report it
        struct Record {
            uint64_t serial = 0;
            std::string path;             // project-relative
            std::string language;         // lua, cpp
            bool ok = true;               // false: kept the old code
            int instances = 0;
            std::vector<std::string> added, removed, converted, reset;
            std::vector<std::string> classes;   // C++: the script classes swapped
            int droppedCallbacks = 0;
            std::vector<ScriptFinding> findings;
            double ms = 0.0;              // the swap on the main thread
            double sinceSaveMs = 0.0;     // from the first change seen to the new code running
            std::string cppState;         // C++: the status row's state after it
        };
        uint64_t lastSerial() const { return nextSerial - 1; }
        std::vector<Record> recordsSince(uint64_t serial) const;
        // {reloaded: [{path, language, instances, properties: {added, removed, converted}, dropped_callbacks, ...}],
        //  kept_old: [{path, language, findings: [{file, line, message}]}]} of the records after serial
        // (only those of `paths`, project-relative, when given)
        static mcp::Json reportJson(const std::vector<Record>& records, const std::vector<std::string>& paths = {});

        // Whether a path is a C++ script source or header under the project's script directories
        bool isCppScriptFile(const std::filesystem::path& path) const;

        // C++ reload ended (Project calls this from its frame-boundary swap or a failed build)
        void cppReloadFinished(const Record& record);

        static constexpr int LUA_DEBOUNCE_MS = 150;
        static constexpr int CPP_DEBOUNCE_MS = 1000;

    private:
        ScriptReloadService() = default;

        struct Pending {
            std::chrono::steady_clock::time_point due;
            std::chrono::steady_clock::time_point firstSeen;
        };
        // what the background check returns
        struct Checked {
            std::filesystem::path path;
            std::string code;
            bool ok = false;
            bool readFailed = false;
            std::vector<ScriptFinding> findings;
            std::vector<ScriptProperty> properties;
            std::chrono::steady_clock::time_point firstSeen;
        };

        void startLuaCheck(const std::filesystem::path& path, std::chrono::steady_clock::time_point firstSeen);
        void applyLua(Checked& checked);
        void addRecord(Record record);
        std::string relative(const std::filesystem::path& path) const;
        void updateLuaStatus();

        Project* project = nullptr;
        std::map<std::filesystem::path, Pending> pendingLua;
        std::map<std::filesystem::path, Pending> pendingCpp;
        std::unique_ptr<jobs::Task<Checked>> luaCheck;
        std::filesystem::path luaCheckPath;
        bool cppBuildRunning = false;
        std::map<std::filesystem::path, std::vector<ScriptFinding>> luaErrors;   // scripts that kept their old code
        std::deque<Record> records;
        uint64_t nextSerial = 1;
    };

}
