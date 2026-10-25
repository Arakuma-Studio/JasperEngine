// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpJson.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace jasperengine::editor::mcp {

    class McpServer;

    // MCP Streamable HTTP (http://127.0.0.1:<port>/mcp) on a minimal hand-written HTTP/1.1
    // server: BSD sockets / Winsock, one thread per connection.
    //  - binds 127.0.0.1 only; rejects an Origin or Host that is not localhost (DNS rebinding)
    //  - requires "Authorization: Bearer <token>" (AppSettings: the owner-only mcp_http_token
    //    file, made the first time HTTP is enabled; the MCP window copies it), compared in
    //    constant time
    //  - POST /mcp: one JSON-RPC message. Requests answer application/json, or
    //    text/event-stream (notifications such as progress or tools/list_changed, then the response) when the client
    //    accepts it and sent a progressToken. Notifications/responses answer 202.
    //  - GET /mcp: 405 (no server-initiated stream). DELETE /mcp ends a legacy session.
    //  - Up to 2025-11-25: Mcp-Session-Id is issued at initialize and required afterwards.
    //  - 2026-07-28 (a request whose MCP-Protocol-Version header or _meta names it): no
    //    session; the MCP-Protocol-Version / Mcp-Method / Mcp-Name headers must mirror the
    //    body, and closing the connection before the answer cancels the request.
    class McpTransportHttp {
    public:
        explicit McpTransportHttp(McpServer& server);
        ~McpTransportHttp();

        bool start(int port, std::string& error);
        void stop();

        bool isRunning() const { return running.load(); }
        int port() const { return listenPort; }
        std::string lastError() const;

        // The bearer token every request must carry; applies to the next request. Any thread.
        void setToken(const std::string& token);

        // --- helpers (any thread; public for the unit tests) ---
        // 32 random bytes as 64 hex digits
        static std::string newToken();
        static bool constantTimeEquals(const std::string& a, const std::string& b);
        // An Authorization header value carries this token (scheme "Bearer", any case). An
        // empty token matches nothing.
        static bool bearerMatches(const std::string& authorization, const std::string& token);
        // Header values that are not plain ASCII arrive as =?base64?<UTF-8 as base64>?=
        static std::string decodeHeaderValue(const std::string& value);
        // 2026-07-28: the MCP-Protocol-Version, Mcp-Method and Mcp-Name headers (lower-case
        // names) must mirror the request body. Null when they do, else the JSON-RPC error
        // to answer with HTTP 400.
        static Json checkModernHeaders(const std::map<std::string, std::string>& headers, const Json& message);

    private:
        McpServer& server;
        std::atomic<bool> running{false};
        int listenPort = 0;
        mutable std::mutex tokenMutex;
        std::string bearerToken;
        intptr_t listenSocket = -1;
        std::thread acceptThread;
        mutable std::mutex mutex;
        std::string error;
        std::set<intptr_t> clients;

        std::string token() const;
        void acceptLoop(intptr_t socket);
        void serveConnection(intptr_t socket);
    };

}
