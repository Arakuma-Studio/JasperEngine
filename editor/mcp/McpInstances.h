// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpJson.h"

#include <filesystem>
#include <string>
#include <vector>

// The per-user registry of running editors that serve MCP over HTTP, so that
// `jasperengine-editor call` finds them (S31 R13): one file per editor,
// <user-config-dir>/mcp/instances/<pid>.json, written atomically with mode 0600 when the HTTP
// transport starts, rewritten when the open project changes, removed when HTTP stops or the
// editor quits. A reader drops the files of processes that are gone. The bearer token is never
// stored here: a client reads it from settings_path (mcp.http_token) or its own options.
namespace jasperengine::editor::mcp {

    struct InstanceInfo {
        int pid = 0;
        std::string url;            // http://127.0.0.1:<port>/mcp
        int port = 0;
        std::string project;        // the open project's directory (absolute)
        std::string cwd;            // the editor's working directory
        std::string settingsPath;   // its settings.yaml (holds mcp.http_token when set)
        bool tokenRequired = false;
        std::string version;        // JASPERENGINE_EDITOR_VERSION
        std::string started;        // UTC, ISO 8601

        Json toJson() const;
        static bool fromJson(const Json& json, InstanceInfo& out);
    };

    // <user-config-dir>/mcp/instances (AppSettings::getUserConfigDirectory)
    std::filesystem::path instancesDirectory();

    // Writes (or replaces) this process's file; false with error set when it can't
    bool writeInstance(const InstanceInfo& info, std::string& error);
    void removeInstance(int pid);

    // The editors whose files name a running process, by pid; the other files are deleted
    std::vector<InstanceInfo> liveInstances();

    // Paths compare as canonical directories (symlinks, "..", a trailing separator, case on Windows)
    bool sameProjectPath(const std::string& a, const std::string& b);

    // Keeps this editor's instance file current (McpServer: HTTP started/stopped, every frame)
    class InstanceRecord {
    public:
        // HTTP serves on port: write the file
        void httpStarted(int port, bool tokenRequired, const std::string& project);
        // the open project may have changed: rewrite the file when it did
        void projectChanged(const std::string& project);
        // HTTP stopped or the editor quits: remove the file
        void httpStopped();
        bool active() const { return info.port > 0; }

    private:
        InstanceInfo info;
        void write();
    };

}
