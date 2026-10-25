// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpSocket.h"

#include <cerrno>
#include <cstring>

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
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace jasperengine::editor::mcp::net {

namespace {

#ifdef _WIN32
using native_t = SOCKET;
#else
using native_t = int;
#endif

#if defined(MSG_NOSIGNAL)
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = 0;
#endif

native_t native(Handle s) { return static_cast<native_t>(s); }

} // namespace

bool initialize() {
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

void close(Handle s) {
    if (s == INVALID) return;
#ifdef _WIN32
    closesocket(native(s));
#else
    ::close(native(s));
#endif
}

void shutdown(Handle s) {
    if (s == INVALID) return;
#ifdef _WIN32
    ::shutdown(native(s), SD_BOTH);
#else
    ::shutdown(native(s), SHUT_RDWR);
#endif
}

int lastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

void setNoInherit(Handle s) {
#ifdef _WIN32
    SetHandleInformation(reinterpret_cast<HANDLE>(native(s)), HANDLE_FLAG_INHERIT, 0);
#else
    const int flags = ::fcntl(native(s), F_GETFD);
    if (flags >= 0) ::fcntl(native(s), F_SETFD, flags | FD_CLOEXEC);
#endif
}

bool sendAll(Handle s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = static_cast<int>(::send(native(s), data.data() + sent, static_cast<int>(data.size() - sent), SEND_FLAGS));
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

long receive(Handle s, std::string& out, size_t max) {
    std::string chunk(max, '\0');
    while (true) {
        const int n = static_cast<int>(::recv(native(s), chunk.data(), static_cast<int>(max), 0));
#ifndef _WIN32
        if (n < 0 && errno == EINTR) continue;
#endif
        if (n > 0) out.append(chunk.data(), static_cast<size_t>(n));
        return n;
    }
}

int waitReadable(Handle s, int timeoutMs) {
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = native(s);
    pfd.events = POLLRDNORM;
    const int ready = WSAPoll(&pfd, 1, timeoutMs);
#else
    pollfd pfd{native(s), POLLIN, 0};
    int ready = ::poll(&pfd, 1, timeoutMs);
    if (ready < 0 && errno == EINTR) ready = 0;
#endif
    return ready < 0 ? -1 : (ready > 0 ? 1 : 0);
}

Handle connectLoopback(int port, int timeoutMs, std::string& error) {
    if (!initialize()) {
        error = "could not initialize sockets";
        return INVALID;
    }
#ifdef _WIN32
    native_t s = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) {
#else
    native_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
#endif
        error = "socket() failed (" + std::to_string(lastError()) + ")";
        return INVALID;
    }
    const Handle handle = static_cast<Handle>(s);
    setNoInherit(handle);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // non-blocking connect, so a filtered port can't hang the caller
#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
#else
    const int flags = ::fcntl(s, F_GETFL);
    ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
    int result = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (result != 0) {
#ifdef _WIN32
        const bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
        const bool pending = errno == EINPROGRESS;
#endif
        if (!pending) {
            error = "connection refused";
            close(handle);
            return INVALID;
        }
#ifdef _WIN32
        WSAPOLLFD pfd{};
        pfd.fd = s;
        pfd.events = POLLWRNORM;
        const int ready = WSAPoll(&pfd, 1, timeoutMs);
#else
        pollfd pfd{s, POLLOUT, 0};
        const int ready = ::poll(&pfd, 1, timeoutMs);
#endif
        if (ready <= 0) {
            error = ready == 0 ? "connection timed out" : "connection failed";
            close(handle);
            return INVALID;
        }
        int code = 0;
#ifdef _WIN32
        int length = sizeof(code);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &length);
#else
        socklen_t length = sizeof(code);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &code, &length);
#endif
        if (code != 0) {
            error = "connection refused";
            close(handle);
            return INVALID;
        }
    }
#ifdef _WIN32
    nonBlocking = 0;
    ioctlsocket(s, FIONBIO, &nonBlocking);
#else
    ::fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
#endif
    int noDelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    return handle;
}

}
