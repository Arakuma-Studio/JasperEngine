// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpTransportStdio.h"

#include "McpServer.h"

#include <cerrno>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace jasperengine::editor::mcp;

namespace {

long readFd(int fd, char* buffer, size_t size) {
#ifdef _WIN32
    return _read(fd, buffer, static_cast<unsigned int>(size));
#else
    return static_cast<long>(::read(fd, buffer, size));
#endif
}

long writeFd(int fd, const char* data, size_t size) {
#ifdef _WIN32
    return _write(fd, data, static_cast<unsigned int>(size));
#else
    return static_cast<long>(::write(fd, data, size));
#endif
}

class StdioReply : public McpReply {
public:
    explicit StdioReply(McpTransportStdio* transport): transport(transport) {}
    void respond(const Json& message) override { transport->send(message); }
    void notify(const Json& notification) override { transport->send(notification); }

private:
    // The transport lives as long as the (never destroyed) server
    McpTransportStdio* transport;
};

} // namespace

McpTransportStdio::McpTransportStdio(McpServer& server, int inFd, int outFd)
    : server(server)
    , inFd(inFd)
    , outFd(outFd) {
}

McpTransportStdio::~McpTransportStdio() {
    // The reader may be blocked in read() until the process ends
    if (reader.joinable()) reader.detach();
}

void McpTransportStdio::start() {
    session = server.createSession("stdio");
    session->notify = [this](const Json& message) { send(message); };
    open = true;
    inputOpen = true;
    reader = std::thread([this]() { readLoop(); });
}

void McpTransportStdio::send(const Json& message) {
    if (!open.load()) return;
    const std::string line = message.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
    std::lock_guard<std::mutex> lock(writeMutex);
    size_t written = 0;
    while (written < line.size()) {
        const long n = writeFd(outFd, line.data() + written, line.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "[mcp] writing to the stdio client failed (errno %d)\n", errno);
            return;
        }
        written += static_cast<size_t>(n);
    }
}

void McpTransportStdio::readLoop() {
    std::string pending;
    char buffer[65536];
    while (true) {
        const long n = readFd(inFd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        pending.append(buffer, static_cast<size_t>(n));
        size_t start = 0;
        size_t newline;
        while ((newline = pending.find('\n', start)) != std::string::npos) {
            std::string line = pending.substr(start, newline - start);
            start = newline + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.find_first_not_of(" \t") == std::string::npos) continue;
            handleLine(line);
        }
        pending.erase(0, start);
    }
    if (!pending.empty() && pending.find_first_not_of(" \t\r\n") != std::string::npos) {
        handleLine(pending);
    }
    inputOpen = false;
    server.onStdioClosed();
}

void McpTransportStdio::handleLine(const std::string& line) {
    Json message = Json::parse(line, nullptr, false);
    auto reply = std::make_shared<StdioReply>(this);
    if (message.is_discarded()) {
        reply->respond(McpServer::makeError(nullptr, -32700, "Parse error: each line must be one JSON-RPC message."));
        return;
    }
    server.handleMessage(session, message, reply);
}
