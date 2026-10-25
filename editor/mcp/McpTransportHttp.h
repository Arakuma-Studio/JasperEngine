// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace jasperengine::editor::mcp {

    class McpServer;

    // MCP Streamable HTTP (http://127.0.0.1:<port>/mcp) on a minimal hand-written HTTP/1.1
    // server: BSD sockets / Winsock, one thread per connection.
    //  - binds 127.0.0.1 only; rejects an Origin or Host that is not localhost (DNS rebinding)
    //  - optional bearer token (Editor Settings / settings.yaml mcp.http_token)
    //  - POST /mcp: one JSON-RPC message. Requests answer application/json, or
    //    text/event-stream (notifications such as progress or tools/list_changed, then the
    //    response) when the client accepts it and a notification comes before the answer.
    //    Notifications/responses answer 202.
    //  - GET /mcp: 405 (no server-initiated stream). DELETE /mcp ends the session.
    //  - Mcp-Session-Id is issued at initialize and required afterwards.
    class McpTransportHttp {
    public:
        explicit McpTransportHttp(McpServer& server);
        ~McpTransportHttp();

        bool start(int port, std::string& error);
        void stop();

        bool isRunning() const { return running.load(); }
        int port() const { return listenPort; }
        std::string lastError() const;

    private:
        McpServer& server;
        std::atomic<bool> running{false};
        int listenPort = 0;
        std::string bearerToken;
        intptr_t listenSocket = -1;
        std::thread acceptThread;
        mutable std::mutex mutex;
        std::string error;
        std::set<intptr_t> clients;

        void acceptLoop(intptr_t socket);
        void serveConnection(intptr_t socket);
    };

}
