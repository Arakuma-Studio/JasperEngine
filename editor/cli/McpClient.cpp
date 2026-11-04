// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpClient.h"

#include "EditorVersion.h"
#include "mcp/McpSocket.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include "htmlui/HttpClient.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;
namespace net = jasperengine::editor::mcp::net;

namespace jasperengine::editor::cli {

namespace {

using Clock = std::chrono::steady_clock;

// Newest first, as the server negotiates
const char* const PROTOCOL_VERSIONS[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

Clock::time_point deadlineAfter(double seconds) {
    return Clock::now() + std::chrono::milliseconds(static_cast<long long>(std::max(0.0, seconds) * 1000.0));
}

int msLeft(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return static_cast<int>(std::max<long long>(0, std::min<long long>(left, 100)));
}

std::string lowerCase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool isResponseTo(const Json& message, const Json& id) {
    return message.is_object() && message.contains("id") && message["id"] == id &&
           (message.contains("result") || message.contains("error"));
}

bool isNotification(const Json& message) {
    return message.is_object() && message.contains("method") && !message.contains("id");
}

Json parseOrThrow(const std::string& text, const std::string& where) {
    const Json message = Json::parse(text, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
        std::string shown = text.substr(0, 200);
        throw ClientError(Internal, "malformed JSON-RPC from " + where + ": " + shown);
    }
    return message;
}

} // namespace

// ---------------------------------------------------------------- McpClient

Json McpClient::initialize(double timeoutSeconds) {
    const Json id = ++nextId;
    const Json message = {
        {"jsonrpc", "2.0"}, {"id", id}, {"method", "initialize"},
        {"params", {
            {"protocolVersion", PROTOCOL_VERSIONS[0]},
            {"capabilities", Json::object()},
            {"clientInfo", {{"name", "jasperengine-editor-call"}, {"version", JASPERENGINE_EDITOR_VERSION}}}
        }}
    };
    const Json response = exchange(message, true, timeoutSeconds, {});
    if (response.is_discarded()) {
        throw ClientError(Connection, stopRequested() ? "interrupted" : "the editor did not answer initialize within " +
                          std::to_string(static_cast<int>(timeoutSeconds)) + " s");
    }
    if (!isResponseTo(response, id)) throw ClientError(Internal, "malformed initialize response: " + response.dump().substr(0, 200));
    if (response.contains("error")) {
        throw ClientError(Internal, "initialize failed: " + response["error"].value("message", response["error"].dump()));
    }
    const Json& result = response["result"];
    const std::string version = result.value("protocolVersion", std::string());
    if (std::find_if(std::begin(PROTOCOL_VERSIONS), std::end(PROTOCOL_VERSIONS),
                     [&](const char* v) { return version == v; }) == std::end(PROTOCOL_VERSIONS)) {
        throw ClientError(Internal, "the editor speaks MCP protocol \"" + version + "\", which this client does not");
    }
    info = result.value("serverInfo", Json::object());
    notify("notifications/initialized");
    return result;
}

Json McpClient::request(const std::string& method, const Json& params, double timeoutSeconds, const NotificationSink& sink) {
    const Json id = ++nextId;
    Json withMeta = params.is_object() ? params : Json::object();
    if (method == "tools/call") {
        // always: the editor reports progress only to a request that asked for it
        if (!withMeta.contains("_meta") || !withMeta["_meta"].is_object()) withMeta["_meta"] = Json::object();
        withMeta["_meta"]["progressToken"] = id;
    }
    const Json message = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", withMeta}};
    const Json response = exchange(message, true, timeoutSeconds, sink);
    if (response.is_discarded()) {
        const bool interrupt = stopRequested();
        // the editor stops the call (MCP cancellation)
        try {
            notify("notifications/cancelled", Json{{"requestId", id}, {"reason", interrupt ? "interrupted" : "client timeout"}});
        } catch (const ClientError&) {
        }
        throw ClientError(Connection, interrupt ? method + " interrupted" :
                          method + " timed out after " + std::to_string(static_cast<int>(timeoutSeconds)) + " s (--timeout)");
    }
    if (!isResponseTo(response, id)) throw ClientError(Internal, "malformed JSON-RPC response: " + response.dump().substr(0, 200));
    return response;
}

void McpClient::notify(const std::string& method, const Json& params) {
    Json message = {{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.empty()) message["params"] = params;
    exchange(message, false, 10.0, {});
}

// ---------------------------------------------------------------- HttpClient

HttpClient::HttpClient(int port, std::string path, std::string token)
    : port(port), path(path.empty() ? std::string("/mcp") : std::move(path)), token(std::move(token)) {
}

std::string HttpClient::url() const {
    return "http://127.0.0.1:" + std::to_string(port) + path;
}

Json HttpClient::exchange(const Json& message, bool isRequest, double timeoutSeconds, const NotificationSink& sink) {
    const Clock::time_point deadline = deadlineAfter(timeoutSeconds);
    std::string error;
    const net::Handle s = net::connectLoopback(port, 5000, error);
    if (s == net::INVALID) throw ClientError(Connection, "can't connect to " + url() + ": " + error);
    struct Closer {
        net::Handle s;
        ~Closer() { net::close(s); }
    } closer{s};

    const std::string body = message.dump(-1, ' ', false, Json::error_handler_t::replace);
    std::string head = "POST " + path + " HTTP/1.1\r\n"
                       "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
                       "Content-Type: application/json\r\n"
                       "Accept: application/json, text/event-stream\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n"
                       "Connection: close\r\n";
    if (!token.empty()) head += "Authorization: Bearer " + token + "\r\n";
    if (!sessionId.empty()) head += "Mcp-Session-Id: " + sessionId + "\r\n";
    if (!protocolVersion.empty()) head += "MCP-Protocol-Version: " + protocolVersion + "\r\n";
    head += "\r\n";
    if (!net::sendAll(s, head + body)) throw ClientError(Connection, "the connection to " + url() + " closed while sending");

    std::string data;
    bool eof = false;
    // more bytes, false at the deadline or on interrupt
    auto readMore = [&]() {
        while (true) {
            if (stopRequested() || Clock::now() >= deadline) return false;
            const int ready = net::waitReadable(s, msLeft(deadline));
            if (ready < 0) throw ClientError(Connection, "the connection to " + url() + " failed");
            if (ready == 0) continue;
            const long n = net::receive(s, data);
            if (n < 0) throw ClientError(Connection, "the connection to " + url() + " failed");
            if (n == 0) eof = true;
            return true;
        }
    };

    size_t headerEnd;
    while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos) {
        if (eof) throw ClientError(Connection, url() + " closed the connection without an answer");
        if (!readMore()) return Json(Json::value_t::discarded);
    }
    const std::string headers = data.substr(0, headerEnd);
    data.erase(0, headerEnd + 4);
    int status = 0;
    {
        const size_t space = headers.find(' ');
        if (space != std::string::npos) status = std::atoi(headers.c_str() + space + 1);
    }
    std::map<std::string, std::string> fields;
    {
        std::istringstream lines(headers);
        std::string line;
        std::getline(lines, line);
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string value = line.substr(colon + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            fields[lowerCase(line.substr(0, colon))] = value;
        }
    }
    if (fields.count("mcp-session-id")) sessionId = fields["mcp-session-id"];

    auto readBody = [&]() {
        const size_t length = fields.count("content-length") ? static_cast<size_t>(std::atoll(fields["content-length"].c_str()))
                                                              : std::string::npos;
        while ((length == std::string::npos || data.size() < length) && !eof) {
            if (!readMore()) return false;
        }
        if (length != std::string::npos && data.size() > length) data.resize(length);
        return true;
    };

    if (status == 401 || status == 403) {
        readBody();
        throw ClientError(Connection, url() + " refused the request (HTTP " + std::to_string(status) + "): " +
                          (status == 401 ? "missing or wrong token (--token, JASPER_MCP_TOKEN, or the mcp_http_token file next to its settings.yaml)"
                                         : data.substr(0, 200)));
    }
    if (status == 404) throw ClientError(Connection, url() + ": session or endpoint not found (HTTP 404); did the editor restart?");
    if (status == 202 || !isRequest) return Json::object();
    if (status != 200) {
        readBody();
        throw ClientError(Internal, url() + " answered HTTP " + std::to_string(status) + ": " + data.substr(0, 300));
    }

    const Json id = message["id"];
    if (fields["content-type"].rfind("text/event-stream", 0) == 0) {
        // events: "data: <json>" lines, a blank line between events
        while (true) {
            size_t end;
            while ((end = data.find("\n\n")) != std::string::npos || (end = data.find("\r\n\r\n")) != std::string::npos) {
                const bool crlf = data.compare(end, 4, "\r\n\r\n") == 0;
                const std::string event = data.substr(0, end);
                data.erase(0, end + (crlf ? 4 : 2));
                std::string payload;
                std::istringstream lines(event);
                std::string line;
                while (std::getline(lines, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.rfind("data:", 0) == 0) payload += line.substr(line.size() > 5 && line[5] == ' ' ? 6 : 5);
                }
                if (payload.empty()) continue;
                const Json event_message = parseOrThrow(payload, url());
                if (isResponseTo(event_message, id)) return event_message;
                if (isNotification(event_message) && sink) sink(event_message);
            }
            if (eof) throw ClientError(Internal, url() + ": the event stream ended without a response");
            if (!readMore()) return Json(Json::value_t::discarded);
        }
    }
    if (!readBody()) return Json(Json::value_t::discarded);
    const Json response = parseOrThrow(data, url());
    if (message.value("method", "") == "initialize" && response.contains("result")) {
        protocolVersion = response["result"].value("protocolVersion", std::string());
    }
    return response;
}

void HttpClient::close() {
    if (sessionId.empty()) return;
    std::string error;
    const net::Handle s = net::connectLoopback(port, 2000, error);
    if (s == net::INVALID) return;
    std::string requestText = "DELETE " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                              "\r\nMcp-Session-Id: " + sessionId + "\r\nContent-Length: 0\r\nConnection: close\r\n";
    if (!token.empty()) requestText += "Authorization: Bearer " + token + "\r\n";
    net::sendAll(s, requestText + "\r\n");
    std::string answer;
    if (net::waitReadable(s, 2000) > 0) net::receive(s, answer);
    net::close(s);
    sessionId.clear();
}

// ---------------------------------------------------------------- StdioClient

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

// CommandLineToArgvW quoting
std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            backslashes++;
            continue;
        }
        if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}

} // namespace

StdioClient::StdioClient(const fs::path& executable, const std::vector<std::string>& args, const fs::path& workdir)
    : stderrLog(workdir / "editor.stderr.log") {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childIn = nullptr, parentIn = nullptr, parentOut = nullptr, childOut = nullptr;
    if (!CreatePipe(&childIn, &parentIn, &inherit, 0) || !CreatePipe(&parentOut, &childOut, &inherit, 0)) {
        throw ClientError(Connection, "can't create the pipes to start the editor");
    }
    SetHandleInformation(parentIn, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentOut, HANDLE_FLAG_INHERIT, 0);
    HANDLE errorFile = CreateFileW(stderrLog.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);

    std::wstring commandLine = quoteArg(executable.wstring());
    for (const std::string& arg : args) commandLine += L" " + quoteArg(widen(arg));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childIn;
    startup.hStdOutput = childOut;
    startup.hStdError = errorFile != INVALID_HANDLE_VALUE ? errorFile : childOut;
    PROCESS_INFORMATION created{};
    const BOOL ok = CreateProcessW(executable.wstring().c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                   nullptr, workdir.wstring().c_str(), &startup, &created);
    CloseHandle(childIn);
    CloseHandle(childOut);
    if (errorFile != INVALID_HANDLE_VALUE) CloseHandle(errorFile);
    if (!ok) {
        CloseHandle(parentIn);
        CloseHandle(parentOut);
        throw ClientError(Connection, "can't start " + executable.string() + " (error " + std::to_string(GetLastError()) + ")");
    }
    CloseHandle(created.hThread);
    process = created.hProcess;
    childPid = static_cast<long>(created.dwProcessId);
    toChild = reinterpret_cast<intptr_t>(parentIn);
    fromChild = reinterpret_cast<intptr_t>(parentOut);
}

bool StdioClient::exited(int& exitCode) {
    if (finished) {
        exitCode = finishedCode;
        return true;
    }
    DWORD code = 0;
    if (!process || !GetExitCodeProcess(static_cast<HANDLE>(process), &code) || code == STILL_ACTIVE) return false;
    finished = true;
    finishedCode = exitCode = static_cast<int>(code);
    return true;
}

bool StdioClient::readLine(std::string& line, int timeoutMs) {
    const Clock::time_point until = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        const size_t newline = buffer.find('\n');
        if (newline != std::string::npos) {
            line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            return true;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(reinterpret_cast<HANDLE>(fromChild), nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available > 0) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!ReadFile(reinterpret_cast<HANDLE>(fromChild), chunk.data(), available, &read, nullptr)) return false;
            buffer.append(chunk.data(), read);
            continue;
        }
        if (Clock::now() >= until) return false;
        Sleep(10);
    }
}

void StdioClient::writeLine(const std::string& text) {
    const std::string line = text + "\n";
    DWORD written = 0;
    if (!WriteFile(reinterpret_cast<HANDLE>(toChild), line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) {
        throw ClientError(Connection, "the editor closed its input");
    }
}

void StdioClient::close() {
    if (toChild != -1) {
        CloseHandle(reinterpret_cast<HANDLE>(toChild));
        toChild = -1;
    }
    if (process) {
        // closing stdin quits the editor; only this process, and only if it is still there in 30 s
        if (WaitForSingleObject(static_cast<HANDLE>(process), 30000) == WAIT_TIMEOUT) {
            TerminateProcess(static_cast<HANDLE>(process), 1);
            WaitForSingleObject(static_cast<HANDLE>(process), 5000);
        }
        int code = 0;
        exited(code);
        CloseHandle(static_cast<HANDLE>(process));
        process = nullptr;
    }
    if (fromChild != -1) {
        CloseHandle(reinterpret_cast<HANDLE>(fromChild));
        fromChild = -1;
    }
}

#else

StdioClient::StdioClient(const fs::path& executable, const std::vector<std::string>& args, const fs::path& workdir)
    : stderrLog(workdir / "editor.stderr.log") {
    // writing to an editor that is gone must be an error, not a signal
    std::signal(SIGPIPE, SIG_IGN);
    int input[2];
    int output[2];
    if (::pipe(input) != 0 || ::pipe(output) != 0) throw ClientError(Connection, "can't create the pipes to start the editor");
    for (int fd : {input[1], output[0]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderrLog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_addclose(&actions, input[0]);
    posix_spawn_file_actions_addclose(&actions, output[1]);
    posix_spawn_file_actions_addchdir_np(&actions, workdir.c_str());

    // headless: the editor starts its own Xvfb instead of opening a window on the user's display
    std::vector<std::string> envStrings;
    for (char** item = environ; item && *item; item++) {
        const std::string entry = *item;
#ifdef __linux__
        if (entry.rfind("DISPLAY=", 0) == 0 || entry.rfind("WAYLAND_DISPLAY=", 0) == 0) continue;
#endif
        envStrings.push_back(entry);
    }
    std::vector<char*> envp;
    for (std::string& entry : envStrings) envp.push_back(entry.data());
    envp.push_back(nullptr);
    std::vector<std::string> argStrings = {executable.string()};
    argStrings.insert(argStrings.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& arg : argStrings) argv.push_back(arg.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int status = posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(input[0]);
    ::close(output[1]);
    if (status != 0) {
        ::close(input[1]);
        ::close(output[0]);
        throw ClientError(Connection, "can't start " + executable.string() + ": " + std::strerror(status));
    }
    childPid = static_cast<long>(pid);
    toChild = input[1];
    fromChild = output[0];
}

bool StdioClient::exited(int& exitCode) {
    if (finished) {
        exitCode = finishedCode;
        return true;
    }
    if (childPid <= 0) return false;
    int status = 0;
    if (::waitpid(static_cast<pid_t>(childPid), &status, WNOHANG) != static_cast<pid_t>(childPid)) return false;
    finished = true;
    finishedCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    exitCode = finishedCode;
    return true;
}

bool StdioClient::readLine(std::string& line, int timeoutMs) {
    const Clock::time_point until = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        const size_t newline = buffer.find('\n');
        if (newline != std::string::npos) {
            line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            return true;
        }
        const int left = static_cast<int>(std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count()));
        pollfd pfd{static_cast<int>(fromChild), POLLIN, 0};
        const int ready = ::poll(&pfd, 1, left);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return false;
        char chunk[65536];
        const ssize_t n = ::read(static_cast<int>(fromChild), chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            // EOF: the editor closed its stdout (it is exiting)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return false;
        }
        buffer.append(chunk, static_cast<size_t>(n));
    }
}

void StdioClient::writeLine(const std::string& text) {
    const std::string line = text + "\n";
    size_t written = 0;
    while (written < line.size()) {
        const ssize_t n = ::write(static_cast<int>(toChild), line.data() + written, line.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw ClientError(Connection, "the editor closed its input");
        written += static_cast<size_t>(n);
    }
}

void StdioClient::close() {
    if (toChild != -1) {
        ::close(static_cast<int>(toChild));
        toChild = -1;
    }
    if (childPid > 0) {
        // closing stdin quits the editor; only this process, and only if it is still there in 30 s
        int code = 0;
        const Clock::time_point until = Clock::now() + std::chrono::seconds(30);
        while (!exited(code) && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!exited(code)) {
            ::kill(static_cast<pid_t>(childPid), SIGKILL);
            int status = 0;
            ::waitpid(static_cast<pid_t>(childPid), &status, 0);
            finished = true;
            finishedCode = 128 + SIGKILL;
        }
    }
    if (fromChild != -1) {
        ::close(static_cast<int>(fromChild));
        fromChild = -1;
    }
}

#endif

StdioClient::~StdioClient() {
    close();
}

std::string StdioClient::stderrTail(size_t lines) const {
    std::ifstream file(stderrLog, std::ios::binary);
    std::deque<std::string> tail;
    std::string line;
    while (std::getline(file, line)) {
        tail.push_back(line);
        if (tail.size() > lines) tail.pop_front();
    }
    std::string out;
    for (const std::string& item : tail) out += item + "\n";
    return out;
}

Json StdioClient::exchange(const Json& message, bool isRequest, double timeoutSeconds, const NotificationSink& sink) {
    int code = 0;
    if (exited(code)) {
        throw ClientError(Connection, "the editor (pid " + std::to_string(childPid) + ") exited with code " +
                          std::to_string(code) + "\n" + stderrTail());
    }
    writeLine(message.dump(-1, ' ', false, Json::error_handler_t::replace));
    if (!isRequest) return Json::object();
    const Json id = message["id"];
    const Clock::time_point deadline = deadlineAfter(timeoutSeconds);
    while (true) {
        if (stopRequested() || Clock::now() >= deadline) return Json(Json::value_t::discarded);
        std::string line;
        if (!readLine(line, 100)) {
            if (exited(code)) {
                throw ClientError(Connection, "the editor (pid " + std::to_string(childPid) + ") exited with code " +
                                  std::to_string(code) + " before answering\n" + stderrTail());
            }
            continue;
        }
        if (line.empty() || line == "\r") continue;
        const Json received = parseOrThrow(line, "the editor's stdout");
        if (isResponseTo(received, id)) return received;
        if (isNotification(received) && sink) sink(received);
    }
}

}
