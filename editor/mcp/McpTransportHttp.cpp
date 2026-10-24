// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpTransportHttp.h"
#include "thread/ThreadUtil.h"

#include "McpServer.h"

#include "AppSettings.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

using namespace jasperengine::editor::mcp;

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
const socket_t INVALID_SOCK = INVALID_SOCKET;
void closeSock(socket_t s) { closesocket(s); }
void shutdownSock(socket_t s) { shutdown(s, SD_BOTH); }
int lastSocketError() { return WSAGetLastError(); }
#else
using socket_t = int;
const socket_t INVALID_SOCK = -1;
void closeSock(socket_t s) { ::close(s); }
void shutdownSock(socket_t s) { ::shutdown(s, SHUT_RDWR); }
int lastSocketError() { return errno; }
#endif

#if defined(MSG_NOSIGNAL)
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = 0;
#endif

constexpr size_t MAX_HEADER_BYTES = 64 * 1024;
constexpr size_t MAX_BODY_BYTES = 64 * 1024 * 1024;

// Child processes (cmake, a run player, Xvfb) must not inherit the server's sockets: a
// child holding the listen socket keeps the port bound after the editor quits, and one
// holding a connection keeps it from closing
void setNoInherit(socket_t s) {
#ifdef _WIN32
    SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
#else
    const int flags = ::fcntl(s, F_GETFD);
    if (flags >= 0) ::fcntl(s, F_SETFD, flags | FD_CLOEXEC);
#endif
}

bool ensureSocketsInitialized() {
#ifdef _WIN32
    static bool ok = []() {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

bool sendAll(socket_t s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = static_cast<int>(::send(s, data.data() + sent, static_cast<int>(data.size() - sent), SEND_FLAGS));
        if (n <= 0) {
#ifndef _WIN32
            if (n < 0 && errno == EINTR) continue;
#endif
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::string lowerCase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim(const std::string& value) {
    const size_t start = value.find_first_not_of(" \t");
    if (start == std::string::npos) return {};
    const size_t end = value.find_last_not_of(" \t\r");
    return value.substr(start, end - start + 1);
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;

    std::string header(const std::string& name) const {
        auto it = headers.find(name);
        return it != headers.end() ? it->second : std::string();
    }
};

// Reads one request. buffer keeps bytes past it (pipelining / keep-alive).
// Returns false on EOF, error or a malformed request (status set when worth answering).
bool readRequest(socket_t s, std::string& buffer, HttpRequest& request, int& status) {
    status = 0;
    size_t headerEnd;
    while ((headerEnd = buffer.find("\r\n\r\n")) == std::string::npos) {
        if (buffer.size() > MAX_HEADER_BYTES) {
            status = 431;
            return false;
        }
        char chunk[16384];
        const int n = static_cast<int>(::recv(s, chunk, sizeof(chunk), 0));
        if (n <= 0) return false;
        buffer.append(chunk, static_cast<size_t>(n));
    }

    const std::string head = buffer.substr(0, headerEnd);
    buffer.erase(0, headerEnd + 4);

    size_t lineEnd = head.find("\r\n");
    const std::string requestLine = head.substr(0, lineEnd);
    const size_t sp1 = requestLine.find(' ');
    const size_t sp2 = requestLine.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        status = 400;
        return false;
    }
    request.method = requestLine.substr(0, sp1);
    request.path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    request.version = requestLine.substr(sp2 + 1);
    const size_t query = request.path.find('?');
    if (query != std::string::npos) request.path.resize(query);

    size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        size_t next = head.find("\r\n", pos);
        if (next == std::string::npos) next = head.size();
        const std::string line = head.substr(pos, next - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            request.headers[lowerCase(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
        }
        pos = next + 2;
    }

    // Reads more bytes into buffer; false on EOF / error
    auto readMore = [&]() {
        char chunk[65536];
        const int n = static_cast<int>(::recv(s, chunk, sizeof(chunk), 0));
        if (n <= 0) return false;
        buffer.append(chunk, static_cast<size_t>(n));
        return true;
    };
    // curl and some HTTP clients wait for this before sending a large body
    if (lowerCase(request.header("expect")) == "100-continue") {
        if (!sendAll(s, "HTTP/1.1 100 Continue\r\n\r\n")) return false;
    }

    if (lowerCase(request.header("transfer-encoding")).find("chunked") != std::string::npos) {
        // chunk-size [;ext] CRLF data CRLF ... 0 CRLF [trailers] CRLF
        std::string body;
        while (true) {
            size_t lineEnd;
            while ((lineEnd = buffer.find("\r\n")) == std::string::npos) {
                if (buffer.size() > 1024) {
                    status = 400;
                    return false;
                }
                if (!readMore()) return false;
            }
            const std::string sizeLine = buffer.substr(0, buffer.find_first_of(";\r"));
            size_t chunkSize = 0;
            try {
                size_t used = 0;
                chunkSize = static_cast<size_t>(std::stoull(trim(sizeLine), &used, 16));
                if (used == 0) throw std::invalid_argument("chunk size");
            } catch (...) {
                status = 400;
                return false;
            }
            buffer.erase(0, lineEnd + 2);
            if (chunkSize == 0) {
                // no trailers: an empty line; else trailer lines ending with an empty line
                while (true) {
                    if (buffer.size() >= 2 && buffer.compare(0, 2, "\r\n") == 0) {
                        buffer.erase(0, 2);
                        break;
                    }
                    const size_t trailersEnd = buffer.find("\r\n\r\n");
                    if (trailersEnd != std::string::npos) {
                        buffer.erase(0, trailersEnd + 4);
                        break;
                    }
                    if (buffer.size() > MAX_HEADER_BYTES) {
                        status = 431;
                        return false;
                    }
                    if (!readMore()) return false;
                }
                break;
            }
            if (body.size() + chunkSize > MAX_BODY_BYTES) {
                status = 413;
                return false;
            }
            while (buffer.size() < chunkSize + 2) {
                if (!readMore()) return false;
            }
            body.append(buffer, 0, chunkSize);
            buffer.erase(0, chunkSize + 2);
        }
        request.body = std::move(body);
        return true;
    }

    size_t length = 0;
    const std::string lengthText = request.header("content-length");
    if (!lengthText.empty()) {
        try {
            length = static_cast<size_t>(std::stoull(lengthText));
        } catch (...) {
            status = 400;
            return false;
        }
    }
    if (length > MAX_BODY_BYTES) {
        status = 413;
        return false;
    }
    while (buffer.size() < length) {
        if (!readMore()) return false;
    }
    request.body = buffer.substr(0, length);
    buffer.erase(0, length);
    return true;
}

const char* statusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 411: return "Length Required";
        case 417: return "Expectation Failed";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
    }
    return "Error";
}

bool sendResponse(socket_t s, int status, const std::string& contentType, const std::string& body,
                  const std::vector<std::pair<std::string, std::string>>& extraHeaders, bool keepAlive) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + statusText(status) + "\r\n";
    if (!contentType.empty()) out += "Content-Type: " + contentType + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    for (const auto& header : extraHeaders) out += header.first + ": " + header.second + "\r\n";
    out += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    out += "\r\n";
    out += body;
    return sendAll(s, out);
}

// localhost, 127.0.0.1 or [::1], with an optional port
bool isLocalHostName(std::string host) {
    host = lowerCase(host);
    if (!host.empty() && host[0] == '[') {
        const size_t close = host.find(']');
        return close != std::string::npos && host.substr(0, close + 1) == "[::1]";
    }
    const size_t colon = host.find(':');
    if (colon != std::string::npos) host.resize(colon);
    return host == "localhost" || host == "127.0.0.1";
}

bool isLocalOrigin(const std::string& origin) {
    if (origin.empty() || origin == "null") return origin.empty();
    const size_t scheme = origin.find("://");
    if (scheme == std::string::npos) return false;
    std::string host = origin.substr(scheme + 3);
    const size_t slash = host.find('/');
    if (slash != std::string::npos) host.resize(slash);
    return isLocalHostName(host);
}

Json jsonError(int code, const std::string& message) {
    return McpServer::makeError(nullptr, code, message);
}

// One request's answer, handed from whatever thread completes it to the connection thread
class HttpReply : public McpReply {
public:
    // streams: the client accepts text/event-stream, so notifications can go before the answer
    explicit HttpReply(bool streams): streams(streams) {}

    bool streamsNotifications() const override { return streams; }

    void respond(const Json& message) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (responded) return;
        response = message;
        responded = true;
        cv.notify_all();
    }

    void notify(const Json& notification) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (responded || !streams) return;
        notifications.push_back(notification);
        cv.notify_all();
    }

    void cancelled() override {
        respond(McpServer::makeError(nullptr, -32800, "Request cancelled."));
    }

    // Waits for the next notification or response, setting isResponse accordingly.
    void next(Json& out, bool& isResponse) {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this]() { return responded || !notifications.empty(); });
        if (!notifications.empty()) {
            out = std::move(notifications.front());
            notifications.pop_front();
            isResponse = false;
            return;
        }
        out = response;
        isResponse = true;
    }

private:
    const bool streams;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Json> notifications;
    Json response;
    bool responded = false;
};

} // namespace

McpTransportHttp::McpTransportHttp(McpServer& server)
    : server(server) {
}

McpTransportHttp::~McpTransportHttp() {
    stop();
}

std::string McpTransportHttp::lastError() const {
    std::lock_guard<std::mutex> lock(mutex);
    return error;
}

bool McpTransportHttp::start(int port, std::string& errorOut) {
    stop();
    if (port <= 0 || port > 65535) {
        errorOut = "invalid port " + std::to_string(port);
        std::lock_guard<std::mutex> lock(mutex);
        error = errorOut;
        return false;
    }
    if (!ensureSocketsInitialized()) {
        errorOut = "could not initialize sockets";
        std::lock_guard<std::mutex> lock(mutex);
        error = errorOut;
        return false;
    }

#ifdef _WIN32
    socket_t s = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
#elif defined(SOCK_CLOEXEC)
    socket_t s = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
#else
    socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
    if (s != INVALID_SOCK) setNoInherit(s);
    if (s == INVALID_SOCK) {
        errorOut = "socket() failed (" + std::to_string(lastSocketError()) + ")";
        std::lock_guard<std::mutex> lock(mutex);
        error = errorOut;
        return false;
    }
#ifdef _WIN32
    // No other process may bind the same port while the editor listens on it
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int code = lastSocketError();
        closeSock(s);
        errorOut = "port " + std::to_string(port) + " is not available (bind error " + std::to_string(code) + ")";
        std::lock_guard<std::mutex> lock(mutex);
        error = errorOut;
        return false;
    }
    if (::listen(s, 16) != 0) {
        closeSock(s);
        errorOut = "listen() failed";
        std::lock_guard<std::mutex> lock(mutex);
        error = errorOut;
        return false;
    }

    bearerToken = AppSettings::getMcpHttpToken();
    listenPort = port;
    listenSocket = static_cast<intptr_t>(s);
    {
        std::lock_guard<std::mutex> lock(mutex);
        error.clear();
    }
    running = true;
    acceptThread = jasperengine::thread::spawnNamed("je-mcp-accept", [this, s]() { acceptLoop(static_cast<intptr_t>(s)); });
    return true;
}

void McpTransportHttp::stop() {
    if (!running.exchange(false)) {
        if (acceptThread.joinable()) acceptThread.join();
        return;
    }
    const socket_t s = static_cast<socket_t>(listenSocket);
    listenSocket = -1;
    // Wakes accept() so the thread can exit
    shutdownSock(s);
    closeSock(s);
    if (acceptThread.joinable()) acceptThread.join();

    // Open connections end too; their threads are detached
    std::lock_guard<std::mutex> lock(mutex);
    for (intptr_t client : clients) shutdownSock(static_cast<socket_t>(client));
}

void McpTransportHttp::acceptLoop(intptr_t listenFd) {
    const socket_t ls = static_cast<socket_t>(listenFd);
    while (running.load()) {
        sockaddr_in peer{};
#ifdef _WIN32
        int peerLen = sizeof(peer);
#else
        socklen_t peerLen = sizeof(peer);
#endif
        socket_t client = ::accept(ls, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (client == INVALID_SOCK) {
            if (!running.load()) break;
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        setNoInherit(client);
        int noDelay = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        {
            std::lock_guard<std::mutex> lock(mutex);
            clients.insert(static_cast<intptr_t>(client));
        }
        // one long-lived service thread per client (not job-system work)
        jasperengine::thread::spawnNamed("je-mcp-client", [this, client]() {
            serveConnection(static_cast<intptr_t>(client));
            {
                std::lock_guard<std::mutex> lock(mutex);
                clients.erase(static_cast<intptr_t>(client));
            }
            closeSock(client);
        }).detach();
    }
}

void McpTransportHttp::serveConnection(intptr_t clientFd) {
    const socket_t s = static_cast<socket_t>(clientFd);
    std::string buffer;
    while (running.load()) {
        HttpRequest request;
        int status = 0;
        if (!readRequest(s, buffer, request, status)) {
            if (status) sendResponse(s, status, "text/plain", statusText(status), {}, false);
            return;
        }
        const bool keepAlive = lowerCase(request.header("connection")) != "close" && request.version != "HTTP/1.0";

        // DNS rebinding: a browser page may only reach a localhost server through a local Origin/Host
        if (!isLocalOrigin(request.header("origin")) ||
            (!request.header("host").empty() && !isLocalHostName(request.header("host")))) {
            sendResponse(s, 403, "application/json", jsonError(-32600, "Forbidden: non-local Origin or Host.").dump(), {}, false);
            return;
        }
        if (request.path != "/mcp" && request.path != "/mcp/") {
            if (!sendResponse(s, 404, "text/plain", "Not found. The MCP endpoint is /mcp.", {}, keepAlive) || !keepAlive) return;
            continue;
        }
        if (!bearerToken.empty() && request.header("authorization") != "Bearer " + bearerToken) {
            if (!sendResponse(s, 401, "application/json", jsonError(-32600, "Unauthorized: missing or wrong bearer token.").dump(),
                              {{"WWW-Authenticate", "Bearer"}}, keepAlive) || !keepAlive) return;
            continue;
        }

        const std::string sessionHeader = request.header("mcp-session-id");

        if (request.method == "GET") {
            if (!sendResponse(s, 405, "text/plain", "This server does not offer a GET stream.",
                              {{"Allow", "POST, DELETE"}}, keepAlive) || !keepAlive) return;
            continue;
        }
        if (request.method == "DELETE") {
            int code = 400;
            if (!sessionHeader.empty()) {
                code = server.findSession(sessionHeader) ? 200 : 404;
                if (code == 200) server.closeSession(sessionHeader);
            }
            if (!sendResponse(s, code, "", "", {}, keepAlive) || !keepAlive) return;
            continue;
        }
        if (request.method != "POST") {
            if (!sendResponse(s, 405, "text/plain", "Use POST.", {{"Allow", "POST, DELETE"}}, keepAlive) || !keepAlive) return;
            continue;
        }

        Json message = McpServer::parseMessage(request.body);
        if (message.is_discarded()) {
            if (!sendResponse(s, 400, "application/json", jsonError(-32700, "Parse error: the body must be one JSON-RPC message.").dump(),
                              {}, keepAlive) || !keepAlive) return;
            continue;
        }
        if (!message.is_object()) {
            if (!sendResponse(s, 400, "application/json", jsonError(-32600, "Invalid Request: send one JSON-RPC message per POST (batches are not supported).").dump(),
                              {}, keepAlive) || !keepAlive) return;
            continue;
        }

        const bool isRequest = message.contains("method") && message.contains("id") && !message["id"].is_null();
        const bool isInitialize = isRequest && message["method"] == "initialize";

        SessionPtr session;
        if (isInitialize) {
            session = server.createSession("http");
        } else if (sessionHeader.empty()) {
            if (!sendResponse(s, 400, "application/json", jsonError(-32600, "Bad Request: missing Mcp-Session-Id header (call initialize first).").dump(),
                              {}, keepAlive) || !keepAlive) return;
            continue;
        } else {
            session = server.findSession(sessionHeader);
            if (!session) {
                if (!sendResponse(s, 404, "application/json", jsonError(-32001, "Session not found: initialize a new session.").dump(),
                                  {}, keepAlive) || !keepAlive) return;
                continue;
            }
        }
        const std::vector<std::pair<std::string, std::string>> sessionHeaders = {{"Mcp-Session-Id", session->id}};

        if (!isRequest) {
            server.handleMessage(session, message, nullptr);
            if (!sendResponse(s, 202, "", "", sessionHeaders, keepAlive) || !keepAlive) return;
            continue;
        }

        // SSE only pays off when notifications (progress, tools/list_changed) come before the
        // answer: the stream starts with the first one, else the answer goes as plain JSON
        const bool acceptsSse = request.header("accept").find("text/event-stream") != std::string::npos;
        auto reply = std::make_shared<HttpReply>(acceptsSse);
        server.handleMessage(session, message, reply);

        Json out;
        bool isResponse = false;
        reply->next(out, isResponse);
        if (isResponse) {
            if (!sendResponse(s, 200, "application/json", out.dump(-1, ' ', false, Json::error_handler_t::replace),
                              sessionHeaders, keepAlive) || !keepAlive) return;
            continue;
        }

        // text/event-stream: notifications, then the response, then the stream closes
        std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                           "Mcp-Session-Id: " + session->id + "\r\nConnection: close\r\n\r\n";
        if (!sendAll(s, head)) return;
        while (true) {
            const std::string event = "event: message\ndata: " + out.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n\n";
            if (!sendAll(s, event)) return;
            if (isResponse) break;
            reply->next(out, isResponse);
        }
        return;
    }
}
