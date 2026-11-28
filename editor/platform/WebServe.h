// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Web Build And Run (docs/specs/build-and-run-on-device.md R10, R11): a static file server on
// 127.0.0.1 (the first free port from 8600 to 8699) for a web build's output folder, with
// tools/web/serve.py's headers (COOP same-origin, COEP require-corp, application/wasm,
// Cache-Control: no-store), GET and HEAD only, no listing, every path confined to the folder.
// A development page posts its log lines to POST /__jasper/log and a requested screenshot to
// POST /__jasper/screenshot, from the server's own origin only; the answer to a log post carries
// the commands queued for the page (screenshot, quit).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine::editor::platform {

    class WebServe {
    public:
        static constexpr int FIRST_PORT = 8600;
        static constexpr int LAST_PORT = 8699;
        static constexpr size_t MAX_LOG_BYTES = 1024 * 1024;
        static constexpr size_t MAX_SCREENSHOT_BYTES = 16 * 1024 * 1024;

        struct PageLine {
            std::string level;   // log, info, warn, error, debug
            std::string text;
        };
        // what a page posted: its lines, and the S28 crash report when it crashed ("" none)
        using LogHandler = std::function<void(const std::vector<PageLine>& lines, const std::string& crash)>;
        using ScreenshotHandler = std::function<void(std::vector<uint8_t>&& png)>;

        // the editor's one server (it starts on the first Web Build And Run, stops with the project)
        static WebServe& get();
        ~WebServe();

        // serves `root` (the folder it served before is replaced); starts listening when it is not
        bool serve(const std::filesystem::path& root, std::string& error);
        void stop();
        bool running() const { return listening.load(); }
        int port() const { return listenPort.load(); }
        std::filesystem::path root() const;

        void setHandlers(LogHandler log, ScreenshotHandler screenshot);
        // returned to the page with its next log post
        void queueCommand(const std::string& command);

        // the file a request target names inside root ("" query and fragment cut, %-decoded); false
        // for a path outside it (.., a link that leads out), a folder or a missing file
        static bool resolve(const std::filesystem::path& root, const std::string& target, std::filesystem::path& file);
        static std::string contentType(const std::filesystem::path& file);

    private:
        void acceptLoop(intptr_t listenSocket);
        void serveConnection(intptr_t client);

        mutable std::mutex mutex;
        std::filesystem::path servedRoot;
        LogHandler logHandler;
        ScreenshotHandler screenshotHandler;
        std::vector<std::string> commands;
        std::atomic<bool> listening{false};
        std::atomic<int> listenPort{0};
        intptr_t listenSocket = -1;
        std::thread acceptThread;
        std::set<intptr_t> clients;
    };

    // the <script> a development page carries (R11): with ?editor-port= in its URL it posts its
    // console lines (batched, at most every 100 ms), its crash report and requested screenshots to
    // the server that serves it, and runs the commands the answers carry
    const std::string& webDevelopmentReporter();

}
