// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WebServe.h"

#include "mcp/McpSocket.h"
#include "thread/ThreadUtil.h"
#include "util/FileUtils.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::platform {

namespace fs = std::filesystem;
namespace net = jasperengine::editor::mcp::net;

namespace {

#ifdef _WIN32
    using WebSocketFd = SOCKET;
    const WebSocketFd WEB_INVALID_SOCKET = INVALID_SOCKET;
#else
    using WebSocketFd = int;
    const WebSocketFd WEB_INVALID_SOCKET = -1;
#endif

    constexpr size_t WEB_MAX_HEADER_BYTES = 64 * 1024;

    struct WebRequest {
        std::string method, target;
        std::map<std::string, std::string> headers;   // lower-case names
        std::string body;
    };

    std::string webLower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::string webTrim(const std::string& text) {
        size_t start = 0, end = text.size();
        while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) end--;
        return text.substr(start, end - start);
    }

    std::string webStatusText(int status) {
        switch (status) {
            case 200: return "OK";
            case 204: return "No Content";
            case 400: return "Bad Request";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 413: return "Payload Too Large";
            default: return "Error";
        }
    }

    // the headers of tools/web/serve.py on every answer
    std::string webResponse(int status, const std::string& contentType, size_t length, const std::string& extra = "") {
        std::string head = "HTTP/1.1 " + std::to_string(status) + " " + webStatusText(status) + "\r\n";
        head += "Cross-Origin-Opener-Policy: same-origin\r\nCross-Origin-Embedder-Policy: require-corp\r\nCache-Control: no-store\r\n";
        if (!contentType.empty()) head += "Content-Type: " + contentType + "\r\n";
        head += "Content-Length: " + std::to_string(length) + "\r\n";
        head += extra;
        head += "Connection: close\r\n\r\n";
        return head;
    }

    // reads the request line, the headers and up to maxBody bytes of body; false when the peer goes
    // or the request is malformed; tooLarge when the body is over the limit
    bool webReadRequest(net::Handle client, WebRequest& request, size_t maxBody, bool& tooLarge) {
        tooLarge = false;
        std::string data;
        size_t headerEnd = std::string::npos;
        while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos) {
            if (data.size() > WEB_MAX_HEADER_BYTES) return false;
            if (net::waitReadable(client, 10000) <= 0) return false;
            if (net::receive(client, data) <= 0) return false;
        }
        const std::string head = data.substr(0, headerEnd);
        std::string body = data.substr(headerEnd + 4);
        size_t lineEnd = head.find("\r\n");
        const std::string requestLine = head.substr(0, lineEnd);
        const size_t firstSpace = requestLine.find(' ');
        const size_t secondSpace = firstSpace == std::string::npos ? std::string::npos : requestLine.find(' ', firstSpace + 1);
        if (secondSpace == std::string::npos) return false;
        request.method = requestLine.substr(0, firstSpace);
        request.target = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        while (lineEnd != std::string::npos) {
            const size_t start = lineEnd + 2;
            lineEnd = head.find("\r\n", start);
            const std::string line = head.substr(start, lineEnd == std::string::npos ? std::string::npos : lineEnd - start);
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            request.headers[webLower(webTrim(line.substr(0, colon)))] = webTrim(line.substr(colon + 1));
        }
        size_t length = 0;
        const auto it = request.headers.find("content-length");
        if (it != request.headers.end()) {
            char* end = nullptr;
            const unsigned long long value = std::strtoull(it->second.c_str(), &end, 10);
            if (!end || *end != '\0') return false;
            if (value > maxBody) {
                tooLarge = true;
                return true;
            }
            length = static_cast<size_t>(value);
        }
        while (body.size() < length) {
            if (net::waitReadable(client, 10000) <= 0) return false;
            if (net::receive(client, body, std::min<size_t>(65536, length - body.size())) <= 0) return false;
        }
        body.resize(length);
        request.body = std::move(body);
        return true;
    }

    std::string webPercentDecode(const std::string& text, bool& ok) {
        ok = true;
        std::string out;
        for (size_t i = 0; i < text.size(); i++) {
            if (text[i] != '%') {
                out += text[i];
                continue;
            }
            if (i + 2 >= text.size() || !std::isxdigit(static_cast<unsigned char>(text[i + 1])) ||
                !std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
                ok = false;
                return out;
            }
            out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
            i += 2;
        }
        return out;
    }

}

WebServe& WebServe::get() {
    // never destroyed: its threads may still answer while the editor exits
    static WebServe* instance = new WebServe();
    return *instance;
}

WebServe::~WebServe() {
    stop();
}

fs::path WebServe::root() const {
    std::lock_guard<std::mutex> lock(mutex);
    return servedRoot;
}

void WebServe::setHandlers(LogHandler log, ScreenshotHandler screenshot) {
    std::lock_guard<std::mutex> lock(mutex);
    logHandler = std::move(log);
    screenshotHandler = std::move(screenshot);
}

void WebServe::queueCommand(const std::string& command) {
    std::lock_guard<std::mutex> lock(mutex);
    commands.push_back(command);
}

bool WebServe::resolve(const fs::path& root, const std::string& target, fs::path& file) {
    std::string path = target.substr(0, target.find_first_of("?#"));
    bool ok = false;
    path = webPercentDecode(path, ok);
    if (!ok || path.empty() || path[0] != '/' || path.find('\0') != std::string::npos || path.find('\\') != std::string::npos) return false;
    if (path.back() == '/') path += "index.html";
    // no segment may climb: "/../project.yaml" is refused before any file is looked at
    fs::path relative;
    size_t start = 1;
    while (start <= path.size()) {
        const size_t end = std::min(path.find('/', start), path.size());
        const std::string segment = path.substr(start, end - start);
        start = end + 1;
        if (segment.empty() || segment == ".") continue;
        if (segment == "..") return false;
        relative /= FileUtils::pathFromUtf8(segment);
        if (end == path.size()) break;
    }
    if (relative.empty()) return false;
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(root, ec);
    if (ec) return false;
    const fs::path candidate = fs::weakly_canonical(base / relative, ec);
    if (ec) return false;
    // where links lead must still be inside the folder
    const fs::path inside = candidate.lexically_relative(base);
    if (inside.empty() || *inside.begin() == "..") return false;
    if (!fs::is_regular_file(candidate, ec)) return false;
    file = candidate;
    return true;
}

std::string WebServe::contentType(const fs::path& file) {
    static const std::map<std::string, std::string> types = {
        {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"}, {".js", "text/javascript"},
        {".mjs", "text/javascript"}, {".wasm", "application/wasm"}, {".json", "application/json"}, {".css", "text/css"},
        {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".svg", "image/svg+xml"},
        {".ico", "image/x-icon"}, {".txt", "text/plain; charset=utf-8"}, {".data", "application/octet-stream"},
        {".jpak", "application/octet-stream"}, {".mp3", "audio/mpeg"}, {".ogg", "audio/ogg"}, {".wav", "audio/wav"},
        {".mp4", "video/mp4"}, {".webm", "video/webm"}, {".ttf", "font/ttf"}, {".otf", "font/otf"}, {".woff2", "font/woff2"}};
    const auto it = types.find(webLower(file.extension().string()));
    return it == types.end() ? "application/octet-stream" : it->second;
}

bool WebServe::serve(const fs::path& folder, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        servedRoot = folder;
        commands.clear();
    }
    if (listening.load()) return true;
    if (!net::initialize()) {
        error = "could not initialize sockets";
        return false;
    }
    for (int port = FIRST_PORT; port <= LAST_PORT; port++) {
#ifdef _WIN32
        WebSocketFd s = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
#elif defined(SOCK_CLOEXEC)
        WebSocketFd s = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
#else
        WebSocketFd s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
        if (s == WEB_INVALID_SOCKET) {
            error = "socket() failed (" + std::to_string(net::lastError()) + ")";
            return false;
        }
        net::setNoInherit(static_cast<net::Handle>(s));
#ifdef _WIN32
        BOOL exclusive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        // loopback only: the build is never served to other machines
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(s, 16) != 0) {
            net::close(static_cast<net::Handle>(s));
            continue;
        }
        listenSocket = static_cast<intptr_t>(s);
        listenPort = port;
        listening = true;
        acceptThread = jasperengine::thread::spawnNamed("je-web-serve", [this, s]() { acceptLoop(static_cast<intptr_t>(s)); });
        return true;
    }
    error = "no free port from " + std::to_string(FIRST_PORT) + " to " + std::to_string(LAST_PORT) + " on 127.0.0.1: close what uses them";
    return false;
}

void WebServe::stop() {
    if (!listening.exchange(false)) {
        if (acceptThread.joinable()) acceptThread.join();
        return;
    }
    const net::Handle s = static_cast<net::Handle>(listenSocket);
    listenSocket = -1;
    net::shutdown(s);
    net::close(s);
    if (acceptThread.joinable()) acceptThread.join();
    std::lock_guard<std::mutex> lock(mutex);
    for (intptr_t client : clients) net::shutdown(static_cast<net::Handle>(client));
    servedRoot.clear();
    logHandler = nullptr;
    screenshotHandler = nullptr;
    listenPort = 0;
}

void WebServe::acceptLoop(intptr_t listenFd) {
    const WebSocketFd ls = static_cast<WebSocketFd>(listenFd);
    while (listening.load()) {
        sockaddr_in peer{};
#ifdef _WIN32
        int peerLen = sizeof(peer);
#else
        socklen_t peerLen = sizeof(peer);
#endif
        WebSocketFd client = ::accept(ls, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (client == WEB_INVALID_SOCKET) {
            if (!listening.load()) break;
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        net::setNoInherit(static_cast<net::Handle>(client));
        {
            std::lock_guard<std::mutex> lock(mutex);
            clients.insert(static_cast<intptr_t>(client));
        }
        jasperengine::thread::spawnNamed("je-web-client", [this, client]() {
            serveConnection(static_cast<intptr_t>(client));
            {
                std::lock_guard<std::mutex> lock(mutex);
                clients.erase(static_cast<intptr_t>(client));
            }
            net::close(static_cast<net::Handle>(client));
        }).detach();
    }
}

void WebServe::serveConnection(intptr_t clientFd) {
    const net::Handle client = static_cast<net::Handle>(clientFd);
    WebRequest request;
    bool tooLarge = false;
    // the body limit by endpoint is known once the target is read; the larger one bounds the read
    if (!webReadRequest(client, request, MAX_SCREENSHOT_BYTES, tooLarge)) return;
    const std::string path = request.target.substr(0, request.target.find_first_of("?#"));
    auto answer = [&](int status, const std::string& type, const std::string& body) {
        net::sendAll(client, webResponse(status, type, body.size()) + (request.method == "HEAD" ? std::string() : body));
    };

    if (request.method == "POST") {
        // the page this server serves, and nothing else
        const std::string origin = request.headers.count("origin") ? request.headers["origin"] : "";
        const std::string own = "http://127.0.0.1:" + std::to_string(listenPort.load());
        const std::string ownName = "http://localhost:" + std::to_string(listenPort.load());
        if (origin != own && origin != ownName) {
            answer(403, "text/plain", "only the page this editor serves may post here\n");
            return;
        }
        if (path == "/__jasper/log") {
            if (tooLarge || request.body.size() > MAX_LOG_BYTES) {
                answer(413, "text/plain", "a log post holds at most 1 MB\n");
                return;
            }
            const nlohmann::json body = nlohmann::json::parse(request.body, nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
                answer(400, "text/plain", "expected {\"lines\": [{\"level\", \"text\"}]}\n");
                return;
            }
            std::vector<PageLine> lines;
            if (body.contains("lines") && body["lines"].is_array()) {
                for (const nlohmann::json& line : body["lines"]) {
                    if (!line.is_object() || !line.contains("text") || !line["text"].is_string()) continue;
                    lines.push_back({line.value("level", "log"), line["text"].get<std::string>()});
                }
            }
            std::string crash;
            if (body.contains("crash") && body["crash"].is_object()) crash = body["crash"].value("message", "unknown error");
            LogHandler handler;
            std::vector<std::string> pending;
            {
                std::lock_guard<std::mutex> lock(mutex);
                handler = logHandler;
                pending.swap(commands);
            }
            if (handler && (!lines.empty() || !crash.empty())) handler(lines, crash);
            answer(200, "application/json", nlohmann::json({{"commands", pending}}).dump());
            return;
        }
        if (path == "/__jasper/screenshot") {
            if (tooLarge) {
                answer(413, "text/plain", "a screenshot holds at most 16 MB\n");
                return;
            }
            ScreenshotHandler handler;
            {
                std::lock_guard<std::mutex> lock(mutex);
                handler = screenshotHandler;
            }
            if (handler) handler(std::vector<uint8_t>(request.body.begin(), request.body.end()));
            answer(200, "text/plain", "ok\n");
            return;
        }
        answer(404, "text/plain", "not found\n");
        return;
    }
    if (request.method != "GET" && request.method != "HEAD") {
        const std::string body = "GET and HEAD only\n";
        net::sendAll(client, webResponse(405, "text/plain", body.size(), "Allow: GET, HEAD\r\n") + body);
        return;
    }
    fs::path file;
    if (!resolve(root(), request.target, file)) {
        // browsers ask for one; a 404 would be an error line in the page's console
        if (path == "/favicon.ico") {
            answer(204, "", "");
            return;
        }
        answer(404, "text/plain", "not found\n");
        return;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        answer(404, "text/plain", "not found\n");
        return;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    answer(200, contentType(file), bytes);
}

const std::string& webDevelopmentReporter() {
    static const std::string script = R"JS(<script>
// Build And Run's log channel (docs/specs/build-and-run-on-device.md R11): a development page served
// by the editor posts its console lines, its crash report and requested screenshots back to it
(function () {
  var port = new URLSearchParams(location.search).get("editor-port");
  if (!port || location.port !== port || !window.fetch) return;
  var queue = [], crash = null, sending = false, last = 0, stopped = false;
  ["log", "info", "warn", "error", "debug"].forEach(function (level) {
    if (!console[level]) return;
    var original = console[level].bind(console);
    console[level] = function () {
      var text = Array.prototype.map.call(arguments, String).join(" ");
      queue.push({ level: level, text: text.length > 2000 ? text.slice(0, 2000) : text });
      original.apply(null, arguments);
    };
  });
  var api = window.jasperengine = window.jasperengine || {};
  var previousCrash = api.onCrash;
  api.onCrash = function (report) {
    crash = report;
    flush();
    if (typeof previousCrash == "function") previousCrash(report);
  };
  function run(command) {
    if (command === "screenshot") {
      var canvas = document.getElementById("canvas") || document.querySelector("canvas");
      if (!canvas) return;
      requestAnimationFrame(function () {
        canvas.toBlob(function (blob) {
          if (blob) fetch("/__jasper/screenshot", { method: "POST", headers: { "Content-Type": "image/png" }, body: blob });
        }, "image/png");
      });
    } else if (command === "quit") {
      stopped = true;
      queue.push({ level: "info", text: "stopped by the editor" });
      flush();
      setTimeout(function () { location.replace("about:blank"); }, 200);
    }
  }
  function flush() {
    if (sending) return;
    var body = { lines: queue.splice(0, 200) };
    if (crash) { body.crash = crash; crash = null; }
    sending = true;
    last = Date.now();
    fetch("/__jasper/log", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) })
      .then(function (answer) { return answer.json(); })
      .then(function (answer) { (answer.commands || []).forEach(run); })
      .catch(function () {})
      .then(function () { sending = false; });
  }
  setInterval(function () {
    if (!stopped && (queue.length || crash || Date.now() - last >= 500)) flush();
  }, 100);
})();
</script>)JS";
    return script;
}

}  // namespace jasperengine::editor::platform
