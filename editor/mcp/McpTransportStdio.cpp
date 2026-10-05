// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpTransportStdio.h"

#include "McpServer.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>

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
    // The reader may be blocked in read() and the writer in write() until the process ends
    if (reader.joinable()) reader.detach();
    if (writer.joinable()) writer.detach();
}

void McpTransportStdio::start() {
    session = server.createSession("stdio");
    session->notify = [this](const Json& message) { send(message); };
    open = true;
    inputOpen = true;
    writer = std::thread([this]() { writeLoop(); });
    reader = std::thread([this]() { readLoop(); });
}

void McpTransportStdio::send(const Json& message) {
    if (!open.load()) return;
    std::string line = message.dump(-1, ' ', false, Json::error_handler_t::replace);
    line += "\n";
    {
        std::lock_guard<std::mutex> lock(writeMutex);
        writeQueue.push_back(std::move(line));
    }
    writeReady.notify_all();
}

void McpTransportStdio::writeLoop() {
    while (true) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(writeMutex);
            writeReady.wait(lock, [this]() { return !writeQueue.empty(); });
            // (flush() waits on the same condition variable for the queue to drain)
            line = std::move(writeQueue.front());
            writeQueue.pop_front();
            writing = true;
        }
        size_t written = 0;
        while (written < line.size()) {
            const long n = writeFd(outFd, line.data() + written, line.size() - written);
            if (n < 0) {
                if (errno == EINTR) continue;
                // The client closed its end: nothing more can be delivered
                std::fprintf(stderr, "[mcp] writing to the stdio client failed (errno %d)\n", errno);
                open = false;
                std::lock_guard<std::mutex> lock(writeMutex);
                writeQueue.clear();
                break;
            }
            written += static_cast<size_t>(n);
        }
        McpServer::trace("wrote", line.substr(0, 80));
        {
            std::lock_guard<std::mutex> lock(writeMutex);
            writing = false;
        }
        writeReady.notify_all();
    }
}

void McpTransportStdio::flush(int timeoutMs) {
    std::unique_lock<std::mutex> lock(writeMutex);
    writeReady.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]() {
        return (writeQueue.empty() && !writing) || !open.load();
    });
}

void McpTransportStdio::readLoop() {
    // One JSON-RPC message per line. A line may arrive in many reads (a large batch) and a
    // read may hold many lines; only the bytes not searched yet are scanned for '\n', so a
    // long line costs O(n), not O(n^2).
    constexpr size_t MAX_LINE_BYTES = 64 * 1024 * 1024;
    std::string pending;
    size_t scanned = 0;        // bytes at the front of pending known to hold no '\n'
    bool discarding = false;   // dropping the rest of an over-long line
    std::vector<char> buffer(65536);
    while (true) {
        const long n = readFd(inFd, buffer.data(), buffer.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        pending.append(buffer.data(), static_cast<size_t>(n));
        size_t start = 0;
        size_t newline;
        while ((newline = pending.find('\n', std::max(start, scanned))) != std::string::npos) {
            const size_t lineStart = start;
            start = newline + 1;
            if (discarding) {
                discarding = false;
                continue;
            }
            std::string line = pending.substr(lineStart, newline - lineStart);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.find_first_not_of(" \t") == std::string::npos) continue;
            handleLine(line);
        }
        pending.erase(0, start);
        scanned = pending.size();
        if (discarding) {
            pending.clear();
            scanned = 0;
        } else if (pending.size() > MAX_LINE_BYTES) {
            std::fprintf(stderr, "[mcp] dropping a stdin line longer than %zu bytes\n", MAX_LINE_BYTES);
            auto reply = std::make_shared<StdioReply>(this);
            reply->respond(McpServer::makeError(nullptr, -32700, "Parse error: message longer than " +
                                                std::to_string(MAX_LINE_BYTES / (1024 * 1024)) + " MB."));
            pending.clear();
            scanned = 0;
            discarding = true;
        }
    }
    if (!discarding && !pending.empty() && pending.find_first_not_of(" \t\r\n") != std::string::npos) {
        handleLine(pending);
    }
    inputOpen = false;
    server.onStdioClosed();
}

void McpTransportStdio::handleLine(const std::string& line) {
    McpServer::trace("read", line.substr(0, 120));
    Json message = McpServer::parseMessage(line);
    auto reply = std::make_shared<StdioReply>(this);
    if (message.is_discarded()) {
        reply->respond(McpServer::makeError(nullptr, -32700, "Parse error: each line must be one JSON-RPC message."));
        return;
    }
    try {
        server.handleMessage(session, message, reply);
    } catch (const std::exception& e) {
        // Never let one bad message end the reader (the client would see a dead server)
        std::fprintf(stderr, "[mcp] stdio message failed: %s\n", e.what());
    }
}
