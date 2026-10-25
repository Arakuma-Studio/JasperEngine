// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>

// BSD sockets / Winsock helpers shared by the MCP HTTP server (McpTransportHttp) and the HTTP
// client of `jasperengine-editor call` (cli/McpClient). Loopback only.
namespace jasperengine::editor::mcp::net {

    // A socket descriptor (SOCKET on Windows, int elsewhere); INVALID when none
    using Handle = intptr_t;
    constexpr Handle INVALID = -1;

    // WSAStartup once on Windows; true elsewhere
    bool initialize();
    void close(Handle s);
    void shutdown(Handle s);
    int lastError();
    // Child processes must not inherit it (FD_CLOEXEC / HANDLE_FLAG_INHERIT)
    void setNoInherit(Handle s);
    // send() until everything is written; false when the peer is gone (no SIGPIPE)
    bool sendAll(Handle s, const std::string& data);
    // recv() into out (appends); returns bytes read, 0 at EOF, -1 on error
    long receive(Handle s, std::string& out, size_t max = 65536);
    // Waits up to timeoutMs for data (or EOF) to read: 1 ready, 0 timeout, -1 error
    int waitReadable(Handle s, int timeoutMs);
    // A TCP connection to 127.0.0.1:port within timeoutMs; INVALID with error set
    // ("connection refused", "timed out", ...)
    Handle connectLoopback(int port, int timeoutMs, std::string& error);

}
