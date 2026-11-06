// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "EditorModal.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // File pickers as MCP sees them (S69 R5). A picker is a call that returns a path, so the editor's
    // main thread waits inside it, and no tool can run there: while one is open the transports answer
    // on their own threads (McpServer::handleCallDuringFilePicker). A headless editor has no native
    // dialog to show, so its picker waits for respond_dialog's values.path; a native one cannot be
    // answered from outside, but editor_status says it is what blocks the editor. FileDialogs calls this
    // for every picker; the state is guarded, any thread may read it.
    class EditorFilePicker {
    public:
        enum class Mode {
            OpenFile,
            OpenFiles,
            OpenFolder,
            OpenFolders,
            SaveFile
        };

        struct Request {
            Mode mode = Mode::OpenFile;
            std::string defaultPath;
            std::string defaultName;
            std::vector<std::string> extensions;    // lower case; empty: any file
        };

        enum class Route {
            Native,     // the platform's dialog: a human answers it
            Agent,      // waits for respond_dialog
            Cancel      // nobody can answer (headless, no MCP session): as if cancelled
        };

        struct Host {
            std::function<Route()> route;
            std::function<void()> opened;           // on the main thread, the picker now open
            std::function<std::string()> baseDirectory;   // what a relative answer is relative to
        };

        // McpServer. An empty host: every picker is native.
        static void setHost(Host host);

        // The answer when this picker is not for the native dialog to answer (empty paths: cancelled).
        // Blocks until an agent answers or the sessions are gone. Main thread.
        static std::optional<std::vector<std::string>> ask(const Request& request);

        // Around the native dialog: reported as open, nobody can answer it
        class NativeScope {
        public:
            explicit NativeScope(const Request& request);
            ~NativeScope();
            NativeScope(const NativeScope&) = delete;
            NativeScope& operator=(const NativeScope&) = delete;
        };

        static bool isOpen();
        // As editor_status reports it; false when none is open
        static bool info(EditorDialogInfo& out);

        // respond_dialog: the paths (relative ones against the host's base directory) or a cancel.
        // False, and why, when they do not fit the request; the picker stays open then.
        static bool answer(const std::vector<std::string>& paths, std::string& error);
        static bool cancel(std::string& error);
        // The last session went away, or the server stops: the picker returns as cancelled
        static void abandon();
    };

}
