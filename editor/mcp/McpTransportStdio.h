// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpJson.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace jasperengine::editor::mcp {

    class McpServer;
    struct McpSession;

    // MCP over newline-delimited JSON-RPC on the stdio the client launched the editor with.
    // A reader thread parses stdin; replies and notifications are written to outFd (the
    // original stdout, which `jasperengine-editor mcp` set aside before anything else could
    // print to it). stdin EOF means the client is gone.
    class McpTransportStdio {
    public:
        McpTransportStdio(McpServer& server, int inFd, int outFd);
        ~McpTransportStdio();

        void start();
        // One JSON-RPC message per line. Thread-safe.
        void send(const Json& message);
        // stdin still open (the client is connected)
        bool isOpen() const { return inputOpen.load(); }

    private:
        McpServer& server;
        int inFd;
        int outFd;
        std::atomic<bool> open{false};
        std::atomic<bool> inputOpen{false};
        std::mutex writeMutex;
        std::thread reader;
        std::shared_ptr<McpSession> session;

        void readLoop();
        void handleLine(const std::string& line);
    };

}
