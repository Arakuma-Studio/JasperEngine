// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TestHttpServer.h"
#include "io/File.h"

#include "json.hpp"
#include "stb_image_write.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

using namespace jaspertest;

namespace {

    struct Request {
        std::string method;
        std::string target;
        std::string path;
        std::string query;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;

        std::string header(const std::string& lowerName) const{
            for (const auto& [name, value] : headers){
                if (name == lowerName) return value;
            }
            return "";
        }
    };

    std::string lower(std::string text){
        for (char& c : text) c = (char)std::tolower((unsigned char)c);
        return text;
    }

    std::string trim(const std::string& text){
        size_t a = 0, b = text.size();
        while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
        while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r')) b--;
        return text.substr(a, b - a);
    }

    bool sendAll(int socket, const std::string& data){
        size_t sent = 0;
        while (sent < data.size()){
            const ssize_t n = ::send(socket, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += (size_t)n;
        }
        return true;
    }

    // reads one request; false when the peer closed or sent garbage
    bool readRequest(int socket, Request& request){
        std::string buffer;
        char chunk[16384];
        size_t headerEnd = std::string::npos;
        while ((headerEnd = buffer.find("\r\n\r\n")) == std::string::npos){
            const ssize_t n = ::recv(socket, chunk, sizeof(chunk), 0);
            if (n <= 0) return false;
            buffer.append(chunk, (size_t)n);
            if (buffer.size() > (1u << 20)) return false;
        }
        std::istringstream head(buffer.substr(0, headerEnd));
        std::string line;
        if (!std::getline(head, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream requestLine(line);
        std::string version;
        requestLine >> request.method >> request.target >> version;
        const size_t q = request.target.find('?');
        request.path = request.target.substr(0, q);
        request.query = q == std::string::npos ? "" : request.target.substr(q + 1);
        while (std::getline(head, line)){
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            request.headers.emplace_back(lower(trim(line.substr(0, colon))), trim(line.substr(colon + 1)));
        }
        request.body = buffer.substr(headerEnd + 4);
        const std::string length = request.header("content-length");
        const size_t want = length.empty() ? 0 : (size_t)std::strtoull(length.c_str(), nullptr, 10);
        while (request.body.size() < want){
            const ssize_t n = ::recv(socket, chunk, sizeof(chunk), 0);
            if (n <= 0) return false;
            request.body.append(chunk, (size_t)n);
        }
        return true;
    }

    const char* reason(int status){
        switch (status){
            case 200: return "OK";
            case 204: return "No Content";
            case 206: return "Partial Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 304: return "Not Modified";
            case 303: return "See Other";
            case 307: return "Temporary Redirect";
            case 308: return "Permanent Redirect";
            case 404: return "Not Found";
            case 416: return "Range Not Satisfiable";
            case 500: return "Internal Server Error";
            default: return "Status";
        }
    }

    std::string response(int status, const std::string& body, const std::string& contentType = "text/plain",
                         const std::vector<std::string>& extra = {}){
        std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n";
        out += "Content-Type: " + contentType + "\r\n";
        out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        for (const std::string& header : extra) out += header + "\r\n";
        out += "Connection: close\r\n\r\n";
        out += body;
        return out;
    }

    uint32_t crc32(const std::string& data){
        static uint32_t table[256];
        static bool ready = false;
        if (!ready){
            for (uint32_t i = 0; i < 256; i++){
                uint32_t c = i;
                for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            ready = true;
        }
        uint32_t crc = 0xFFFFFFFFu;
        for (unsigned char b : data) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }

    // gzip with stored (uncompressed) deflate blocks: valid for every decoder
    std::string gzip(const std::string& data){
        std::string out = std::string("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
        size_t pos = 0;
        do {
            const size_t n = std::min<size_t>(65535, data.size() - pos);
            const bool last = pos + n >= data.size();
            out += (char)(last ? 1 : 0);
            out += (char)(n & 0xFF);
            out += (char)((n >> 8) & 0xFF);
            out += (char)(~n & 0xFF);
            out += (char)((~n >> 8) & 0xFF);
            out.append(data, pos, n);
            pos += n;
        } while (pos < data.size());
        const uint32_t crc = crc32(data);
        const uint32_t size = (uint32_t)data.size();
        for (int i = 0; i < 4; i++) out += (char)((crc >> (8 * i)) & 0xFF);
        for (int i = 0; i < 4; i++) out += (char)((size >> (8 * i)) & 0xFF);
        return out;
    }

    std::string pngBytes(){
        // 4x2: red, green, blue, white / black, transparent, grey, yellow
        const unsigned char pixels[4 * 2 * 4] = {
            255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 255,   255, 255, 255, 255,
            0, 0, 0, 255,     0, 0, 0, 0,       128, 128, 128, 255, 255, 255, 0, 255,
        };
        std::string out;
        stbi_write_png_to_func([](void* context, void* data, int size) {
            static_cast<std::string*>(context)->append(static_cast<const char*>(data), (size_t)size);
        }, &out, 4, 2, 4, pixels, 16);
        return out;
    }

    std::string hex(const std::string& bytes){
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (unsigned char c : bytes){
            out += digits[c >> 4];
            out += digits[c & 15];
        }
        return out;
    }

    std::string queryValue(const std::string& query, const std::string& name){
        size_t pos = 0;
        while (pos <= query.size()){
            size_t end = query.find('&', pos);
            if (end == std::string::npos) end = query.size();
            const std::string pair = query.substr(pos, end - pos);
            const size_t eq = pair.find('=');
            if (pair.substr(0, eq) == name){
                std::string value = eq == std::string::npos ? "" : pair.substr(eq + 1);
                std::string decoded;
                for (size_t i = 0; i < value.size(); i++){
                    if (value[i] == '%' && i + 2 < value.size()){
                        decoded += (char)std::strtol(value.substr(i + 1, 2).c_str(), nullptr, 16);
                        i += 2;
                    } else {
                        decoded += value[i];
                    }
                }
                return decoded;
            }
            pos = end + 1;
        }
        return "";
    }

    bool startsWith(const std::string& text, const std::string& prefix){
        return text.compare(0, prefix.size(), prefix) == 0;
    }

}

TestHttpServer::TestHttpServer(){
    listenSocket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket < 0) return;
    int yes = 1;
    ::setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listenSocket, 64) != 0){
        ::close(listenSocket);
        listenSocket = -1;
        return;
    }
    socklen_t length = sizeof(address);
    ::getsockname(listenSocket, reinterpret_cast<sockaddr*>(&address), &length);
    boundPort = ntohs(address.sin_port);
    acceptThread = std::thread([this]() { acceptLoop(); });
}

TestHttpServer::~TestHttpServer(){
    stopping = true;
    if (acceptThread.joinable()) acceptThread.join();
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(mutex);
        threads.swap(connections);
    }
    for (std::thread& thread : threads){
        if (thread.joinable()) thread.join();
    }
    if (listenSocket >= 0) ::close(listenSocket);
}

void TestHttpServer::addFile(const std::string& name, std::string bytes, std::string etag){
    std::lock_guard<std::mutex> lock(countMutex);
    files[name] = File{std::move(bytes), std::move(etag)};
}

void TestHttpServer::cutNext(const std::string& name, size_t bytes){
    std::lock_guard<std::mutex> lock(countMutex);
    files[name].cutNext = bytes;
}

std::string TestHttpServer::url(const std::string& path) const{
    return "http://127.0.0.1:" + std::to_string(boundPort) + path;
}

int TestHttpServer::requests(const std::string& path) const{
    std::lock_guard<std::mutex> lock(countMutex);
    auto it = counts.find(path);
    return it == counts.end() ? 0 : it->second;
}

std::string TestHttpServer::lastHeader(const std::string& path, const std::string& name) const{
    std::lock_guard<std::mutex> lock(countMutex);
    auto it = lastHeaders.find(path);
    if (it == lastHeaders.end()) return "";
    for (const auto& [n, v] : it->second){
        if (n == name) return v;
    }
    return "";
}

int TestHttpServer::totalRequests() const{
    std::lock_guard<std::mutex> lock(countMutex);
    int total = 0;
    for (const auto& [path, count] : counts) total += count;
    return total;
}

void TestHttpServer::resetCounters(){
    std::lock_guard<std::mutex> lock(countMutex);
    counts.clear();
    maxActive = 0;
}

std::string TestHttpServer::pattern(size_t n, size_t offset){
    std::string out(n, '\0');
    for (size_t i = 0; i < n; i++) out[i] = (char)((i + offset) % 251);
    return out;
}

void TestHttpServer::acceptLoop(){
    while (!stopping){
        pollfd fd{listenSocket, POLLIN, 0};
        if (::poll(&fd, 1, 50) <= 0) continue;
        const int client = ::accept(listenSocket, nullptr, nullptr);
        if (client < 0) continue;
        int yes = 1;
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        std::lock_guard<std::mutex> lock(mutex);
        connections.emplace_back([this, client]() { serve(client); });
    }
}

void TestHttpServer::serve(int socket){
    Request request;
    if (!readRequest(socket, request)){
        ::close(socket);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(countMutex);
        counts[request.path]++;
        lastHeaders[request.path] = request.headers;
    }
    // waits in small steps so the server can stop
    auto wait = [this](int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!stopping && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    const std::string& path = request.path;
    if (path == "/text"){
        sendAll(socket, response(200, "hello http", "text/plain; charset=utf-8", {"X-Test: one", "X-Test: two"}));
    } else if (path == "/json"){
        sendAll(socket, response(200, R"({"name":"jasper","n":3,"list":[1,2],"nested":{"ok":true}})", "application/json"));
    } else if (path == "/badjson"){
        sendAll(socket, response(200, "{not json", "application/json"));
    } else if (startsWith(path, "/status/")){
        const int code = std::atoi(path.c_str() + 8);
        sendAll(socket, response(code, "status " + std::to_string(code)));
    } else if (startsWith(path, "/redirect/")){
        const int n = std::atoi(path.c_str() + 10);
        if (n <= 0) sendAll(socket, response(200, "redirected"));
        else sendAll(socket, response(302, "moving", "text/plain", {"Location: /redirect/" + std::to_string(n - 1)}));
    } else if (startsWith(path, "/redirect-code/")){
        const int code = std::atoi(path.c_str() + 15);
        const std::string to = queryValue(request.query, "to");
        sendAll(socket, response(code, "moving", "text/plain", {"Location: " + (to.empty() ? std::string("/echo") : to)}));
    } else if (startsWith(path, "/slow/")){
        wait(std::atoi(path.c_str() + 6));
        if (!stopping) sendAll(socket, response(200, "slow"));
    } else if (startsWith(path, "/bytes/")){
        const size_t n = (size_t)std::strtoull(path.c_str() + 7, nullptr, 10);
        std::string etag = queryValue(request.query, "etag");
        etag = "\"" + (etag.empty() ? std::string("v1") : etag) + "\"";
        const std::string range = request.header("range");
        const std::string ifRange = request.header("if-range");
        if (startsWith(range, "bytes=") && (ifRange.empty() || ifRange == etag)){
            const size_t from = (size_t)std::strtoull(range.c_str() + 6, nullptr, 10);
            if (from >= n){
                sendAll(socket, response(416, "", "text/plain", {"Content-Range: bytes */" + std::to_string(n)}));
            } else {
                sendAll(socket, response(206, pattern(n - from, from), "application/octet-stream",
                                         {"Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(n - 1) + "/" + std::to_string(n),
                                          "ETag: " + etag}));
            }
        } else {
            sendAll(socket, response(200, pattern(n), "application/octet-stream", {"Accept-Ranges: bytes", "ETag: " + etag}));
        }
    } else if (startsWith(path, "/chunked/")){
        const size_t n = (size_t)std::strtoull(path.c_str() + 9, nullptr, 10);
        const std::string data = pattern(n);
        std::string out = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
        for (size_t pos = 0; pos < data.size(); pos += 4096){
            const size_t len = std::min<size_t>(4096, data.size() - pos);
            char size[32];
            std::snprintf(size, sizeof(size), "%zx\r\n", len);
            out += size;
            out.append(data, pos, len);
            out += "\r\n";
        }
        out += "0\r\n\r\n";
        sendAll(socket, out);
    } else if (startsWith(path, "/trickle/")){
        size_t n = 0;
        int ms = 0;
        std::sscanf(path.c_str() + 9, "%zu/%d", &n, &ms);
        const std::string data = pattern(n);
        std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(n) +
                           "\r\nETag: \"t1\"\r\nConnection: close\r\n\r\n";
        bool ok = sendAll(socket, head);
        for (size_t pos = 0; ok && pos < data.size() && !stopping; pos += 16384){
            ok = sendAll(socket, data.substr(pos, 16384));
            wait(ms);
        }
    } else if (path == "/echo"){
        nlohmann::json headers = nlohmann::json::object();
        for (const auto& [name, value] : request.headers){
            if (headers.contains(name)) headers[name] = headers[name].get<std::string>() + ", " + value;
            else headers[name] = value;
        }
        nlohmann::json echo = {
            {"method", request.method}, {"path", request.path}, {"query", request.query},
            {"headers", headers}, {"body_hex", hex(request.body)}, {"body_size", request.body.size()}
        };
        sendAll(socket, response(200, echo.dump(), "application/json"));
    } else if (path == "/gzip"){
        std::string text;
        for (int i = 0; i < 100; i++) text += "gzip body ";
        sendAll(socket, response(200, gzip(text), "text/plain", {"Content-Encoding: gzip"}));
    } else if (path == "/png"){
        sendAll(socket, response(200, pngBytes(), "image/png"));
    } else if (path == "/cookies"){
        sendAll(socket, response(200, "cookies", "text/plain", {"Set-Cookie: a=1; Path=/", "Set-Cookie: b=2, x; Path=/"}));
    } else if (startsWith(path, "/count/")){
        const int now = ++active;
        int seen = maxActive.load();
        while (now > seen && !maxActive.compare_exchange_weak(seen, now)){}
        wait(std::atoi(path.c_str() + 7));
        --active;
        sendAll(socket, response(200, "counted"));
    } else if (startsWith(path, "/file/")){
        File file;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(countMutex);
            auto it = files.find(path.substr(6));
            if (it != files.end()){
                file = it->second;
                it->second.cutNext = 0;
                found = true;
            }
        }
        const std::string modified = "Last-Modified: Wed, 21 Oct 2026 07:28:00 GMT";
        if (!found){
            sendAll(socket, response(404, "no file " + path));
        } else if (!file.etag.empty() && request.header("if-none-match") == file.etag){
            sendAll(socket, "HTTP/1.1 304 Not Modified\r\nETag: " + file.etag + "\r\n" + modified + "\r\nConnection: close\r\n\r\n");
        } else {
            size_t from = 0;
            const std::string range = request.header("range");
            if (startsWith(range, "bytes="))
                from = (size_t)std::strtoull(range.c_str() + 6, nullptr, 10);
            if (from >= file.bytes.size() && from > 0){
                sendAll(socket, response(416, "", "text/plain", {"Content-Range: bytes */" + std::to_string(file.bytes.size())}));
            } else {
                const std::string body = file.bytes.substr(from);
                const int status = from > 0 ? 206 : 200;
                std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\nContent-Type: video/mp4\r\nContent-Length: " +
                                   std::to_string(body.size()) + "\r\nAccept-Ranges: bytes\r\n" + modified + "\r\n";
                if (!file.etag.empty())
                    head += "ETag: " + file.etag + "\r\n";
                if (from > 0)
                    head += "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(file.bytes.size() - 1) + "/" +
                            std::to_string(file.bytes.size()) + "\r\n";
                head += "Connection: close\r\n\r\n";
                const int trickle = std::atoi(queryValue(request.query, "trickle").c_str());
                const std::string cutText = queryValue(request.query, "cut");
                size_t cut = cutText.empty() ? body.size() : std::min(body.size(), (size_t)std::strtoull(cutText.c_str(), nullptr, 10));
                if (file.cutNext > 0)
                    cut = std::min(cut, file.cutNext);
                bool ok = sendAll(socket, head);
                for (size_t pos = 0; ok && pos < cut && !stopping; pos += 4096){
                    ok = sendAll(socket, body.substr(pos, std::min<size_t>(4096, cut - pos)));
                    if (trickle > 0)
                        wait(trickle);
                }
                if (cut < body.size()){
                    // a broken transfer: the connection goes without the rest
                    ::shutdown(socket, SHUT_RDWR);
                }
            }
        }
    } else if (path == "/drop"){
        // nothing: the connection closes
    } else {
        sendAll(socket, response(404, "no route " + path));
    }
    ::shutdown(socket, SHUT_WR);
    // the client reads to the end before the socket goes
    char drain[1024];
    pollfd fd{socket, POLLIN, 0};
    for (int i = 0; i < 20 && ::poll(&fd, 1, 50) > 0; i++){
        if (::recv(socket, drain, sizeof(drain), 0) <= 0) break;
    }
    ::close(socket);
}
