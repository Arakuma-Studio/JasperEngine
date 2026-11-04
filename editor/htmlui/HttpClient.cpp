// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HttpClient.h"

#include "util/CommandRunner.h"

#include "htmlui/compat/ImportManifest.h"
#include "util/FileUtils.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor::htmlui;
namespace fs = std::filesystem;

namespace {

    struct Defaults {
        std::mutex mutex;
        std::map<std::string, std::string> hostRewrite;
        int timeoutSeconds = HttpClient::kDefaultTimeoutSeconds;
    };

    Defaults& defaults(){
        static Defaults instance;
        return instance;
    }

    // the runners of requests in flight, for cancelAll()
    struct Active {
        std::mutex mutex;
        std::set<jasperengine::editor::CommandRunner*> runners;
        std::atomic<bool> cancelled{false};
    };

    Active& active(){
        static Active instance;
        return instance;
    }


    std::string lower(std::string text){
        for (char& c : text){
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
        }
        return text;
    }

    std::string trim(const std::string& text){
        size_t begin = 0, end = text.size();
        while (begin < end && (unsigned char)text[begin] <= ' ')
            begin++;
        while (end > begin && (unsigned char)text[end - 1] <= ' ')
            end--;
        return text.substr(begin, end - begin);
    }

    // a value of a curl config file: double-quoted, backslash and quote escaped
    std::string configValue(const std::string& value){
        std::string out = "\"";
        for (char c : value){
            if (c == '\\' || c == '"')
                out += '\\';
            if (c == '\n' || c == '\r')
                continue;
            out += c;
        }
        out += '"';
        return out;
    }

    std::string mediaType(const std::string& contentType){
        std::string type = contentType;
        const size_t semicolon = type.find(';');
        if (semicolon != std::string::npos)
            type.erase(semicolon);
        return lower(trim(type));
    }

    // what a mirror file is served as, from the extension of the URL's path
    std::string mirrorContentType(const std::string& url){
        std::string path = url;
        const size_t query = path.find_first_of("?#");
        if (query != std::string::npos)
            path.erase(query);
        const size_t dot = path.find_last_of('.');
        const size_t slash = path.find_last_of('/');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            return "";
        const std::string extension = lower(path.substr(dot + 1));
        static const std::map<std::string, std::string> types = {
            {"css", "text/css"}, {"json", "application/json"}, {"lottie", "application/zip"},
            {"svg", "image/svg+xml"}, {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
            {"gif", "image/gif"}, {"webp", "image/webp"}, {"ttf", "font/ttf"}, {"otf", "font/otf"},
            {"woff", "font/woff"}, {"woff2", "font/woff2"}, {"txt", "text/plain"}, {"html", "text/html"},
        };
        const auto it = types.find(extension);
        return it != types.end() ? it->second : std::string();
    }

    // "curl: (28) Operation timed out ..." -> "Operation timed out ..."
    std::string curlMessage(const std::string& line){
        const size_t close = line.find(") ");
        if (line.compare(0, 7, "curl: (") == 0 && close != std::string::npos)
            return line.substr(close + 2);
        if (line.compare(0, 6, "curl: ") == 0)
            return line.substr(6);
        return line;
    }

    // what a browser does with characters a URL cannot hold as they are (curl refuses them)
    std::string encodeForRequest(const std::string& url){
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : url){
            if (c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' || c == '`' || c == '{' ||
                c == '|' || c == '}'){
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 15];
            }else{
                out += (char)c;
            }
        }
        return out;
    }

    bool isWriteOutLine(const std::string& line){
        return line.size() >= 4 && std::isdigit((unsigned char)line[0]) && std::isdigit((unsigned char)line[1]) &&
               std::isdigit((unsigned char)line[2]) && line[3] == ' ';
    }

}

HttpClient::HttpClient(): runner(std::make_unique<CommandRunner>()){
    std::lock_guard<std::mutex> lock(defaults().mutex);
    hostRewrite = defaults().hostRewrite;
    timeoutSeconds = defaults().timeoutSeconds;
}

HttpClient::~HttpClient(){
    if (!tempDir.empty()){
        std::error_code ec;
        fs::remove_all(tempDir, ec);
    }
}

bool HttpClient::available(){
    static std::mutex mutex;
    static std::map<std::string, bool> byPath;

    const char* pathValue = std::getenv("PATH");
    const std::string key = pathValue ? pathValue : "";
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = byPath.find(key);
    if (it != byPath.end())
        return it->second;

    bool found = false;
    CommandRunner runner;
    runner.run({"curl", "--version"}, fs::path(), [&found](const std::string& line){
        if (line.compare(0, 5, "curl ") == 0)
            found = true;
    });
    byPath[key] = found;
    return found;
}

void HttpClient::setHostRewrite(std::map<std::string, std::string> rewrites){
    hostRewrite = std::move(rewrites);
}

void HttpClient::setMaxFileSize(long long bytes){
    maxFileSize = bytes > 0 ? bytes : kMaxFileSize;
}

void HttpClient::setTimeoutSeconds(int seconds){
    timeoutSeconds = seconds > 0 ? seconds : kDefaultTimeoutSeconds;
}

void HttpClient::setDefaultHostRewrite(std::map<std::string, std::string> rewrites){
    std::lock_guard<std::mutex> lock(defaults().mutex);
    defaults().hostRewrite = std::move(rewrites);
}

std::map<std::string, std::string> HttpClient::getDefaultHostRewrite(){
    std::lock_guard<std::mutex> lock(defaults().mutex);
    return defaults().hostRewrite;
}

void HttpClient::setDefaultTimeoutSeconds(int seconds){
    std::lock_guard<std::mutex> lock(defaults().mutex);
    defaults().timeoutSeconds = seconds > 0 ? seconds : kDefaultTimeoutSeconds;
}

void HttpClient::cancelAll(){
    Active& state = active();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.cancelled = true;
    for (CommandRunner* running : state.runners)
        running->cancel();
}

bool HttpClient::cancelled(){
    return active().cancelled;
}

std::string HttpClient::mirrorDirectory(){
    const char* value = std::getenv("JASPERENGINE_IMPORT_MIRROR");
    return value ? std::string(value) : std::string();
}

std::string HttpClient::urlQuote(const std::string& text){
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text){
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' || c == '~'){
            out += (char)c;
        }else{
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string HttpClient::mirrorPath(const std::string& url){
    const std::string normalized = jasperengine::htmlui::compat::ImportManifest::normalizeUrl(url);
    if (normalized.compare(0, 8, "https://") != 0)
        return "";
    const std::string rest = normalized.substr(8);
    const size_t slash = rest.find_first_of("/?");
    std::string host = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string pathAndQuery = slash == std::string::npos ? "/" : rest.substr(slash);
    if (!pathAndQuery.empty() && pathAndQuery[0] == '?')
        pathAndQuery.insert(0, "/");
    // a port is not part of the directory name (':' is not allowed in Windows names)
    const size_t colon = host.find(':');
    if (colon != std::string::npos)
        host.erase(colon);
    if (host.empty())
        return "";
    return host + "/" + urlQuote(pathAndQuery);
}

bool HttpClient::readBody(const HttpResponse& response, std::string& body){
    body.clear();
    if (response.bodyFile.empty())
        return false;
    std::ifstream file(response.bodyFile, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream stream;
    stream << file.rdbuf();
    body = stream.str();
    return true;
}

std::string HttpClient::rewrite(const std::string& url) const{
    // the longest matching prefix that ends at a URL boundary
    const std::pair<const std::string, std::string>* best = nullptr;
    for (const auto& pair : hostRewrite){
        const std::string& from = pair.first;
        if (url.compare(0, from.size(), from) != 0)
            continue;
        if (url.size() > from.size() && from.back() != '/' && url[from.size()] != '/' && url[from.size()] != '?')
            continue;
        if (!best || from.size() > best->first.size())
            best = &pair;
    }
    if (!best)
        return url;
    return best->second + url.substr(best->first.size());
}

std::string HttpClient::unrewrite(const std::string& url) const{
    const std::pair<const std::string, std::string>* best = nullptr;
    for (const auto& pair : hostRewrite){
        const std::string& to = pair.second;
        if (url.compare(0, to.size(), to) != 0)
            continue;
        if (url.size() > to.size() && to.back() != '/' && url[to.size()] != '/' && url[to.size()] != '?')
            continue;
        if (!best || to.size() > best->second.size())
            best = &pair;
    }
    if (!best)
        return url;
    return best->first + url.substr(best->second.size());
}

fs::path HttpClient::newTempFile(const char* suffix){
    if (tempDir.empty()){
        std::random_device device;
        std::ostringstream name;
        name << "jasperengine-http-" << std::hex << device() << device();
        std::error_code ec;
        tempDir = fs::temp_directory_path(ec) / name.str();
        fs::create_directories(tempDir, ec);
    }
    return tempDir / (std::to_string(++counter) + suffix);
}

HttpResponse HttpClient::getFromMirror(const std::string& mirror, const std::string& url){
    HttpResponse response;
    response.finalUrl = url;
    const std::string relative = mirrorPath(url);
    if (relative.empty()){
        response.error = "not found: " + url + " (the import mirror serves http(s) URLs only)";
        return response;
    }
    const fs::path file = fs::path(mirror) / editor::FileUtils::pathFromUtf8(relative);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)){
        response.status = 404;
        response.error = "not found: " + url + " (no mirror file " + file.generic_string() + ")";
        return response;
    }
    response.status = 200;
    response.contentType = mirrorContentType(url);
    response.bodyFile = file;
    return response;
}

void HttpClient::cancel(){
    cancelRequested.store(true);
    runner->cancel();
}

unsigned long long HttpClient::receivedBytes() const{
    std::lock_guard<std::mutex> lock(bodyMutex);
    std::error_code ec;
    if (currentBody.empty()) return 0;
    const uintmax_t size = fs::file_size(currentBody, ec);
    return ec ? 0 : static_cast<unsigned long long>(size);
}

HttpResponse HttpClient::get(const std::string& url){
    const std::string mirror = mirrorDirectory();
    if (!mirror.empty())
        return getFromMirror(mirror, url);
    return transfer(url, newTempFile(".body"), false);
}

HttpResponse HttpClient::getResuming(const std::string& url, const fs::path& partial){
    const std::string mirror = mirrorDirectory();
    if (mirror.empty())
        return transfer(url, partial, true);
    HttpResponse response = getFromMirror(mirror, url);
    if (response.ok()){
        std::error_code ec;
        if (!editor::FileUtils::copyFileAtomic(response.bodyFile, partial, ec))
            response.error = "cannot write " + partial.string() + ": " + ec.message();
        response.bodyFile = partial;
    }
    return response;
}

HttpResponse HttpClient::transfer(const std::string& url, const fs::path& body, bool resume){
    HttpResponse response;
    response.finalUrl = url;

    const std::string requestUrl = encodeForRequest(rewrite(url));
    const fs::path config = newTempFile(".curlrc");
    {
        std::lock_guard<std::mutex> lock(bodyMutex);
        currentBody = body;
    }
    {
        std::ofstream out(config, std::ios::binary);
        out << "url = " << configValue(requestUrl) << "\n"
            << "output = " << configValue(body.string()) << "\n"
            << "user-agent = " << configValue(kUserAgent) << "\n"
            // http(s) only, also after a redirect: remote content (a Lottie image ref, a Google
            // Fonts src) can't make curl read file:// or another scheme
            << "proto = \"=https,http\"\n"
            << "proto-redir = \"=https,http\"\n"
            << "location\n"
            << "max-redirs = 5\n"
            << "max-time = " << timeoutSeconds << "\n"
            << "max-filesize = " << maxFileSize << "\n"
            // (resuming: an HTTP error's body would land after the bytes kept so far)
            << (resume ? "continue-at = \"-\"\nfail\nconnect-timeout = 30\nspeed-limit = 1024\nspeed-time = 60\n" : "fail-with-body\n")
            << "write-out = \"%{http_code} %{content_type} %{url_effective}\\n\"\n"
            << "silent\n"
            << "show-error\n";
        if (!out){
            response.error = "cannot write a temporary file in " + config.parent_path().string();
            return response;
        }
    }

    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(active().mutex);
        if (active().cancelled || cancelRequested.load()){
            response.error = "cancelled";
            return response;
        }
        active().runners.insert(runner.get());
    }
    // argv, no shell: the config file path is one literal argument (its contents — URL, headers —
    // are curl's own config syntax, written by newCurlConfig). curl's non-zero exit is expected on a
    // network error and is read from `lines`, not the return code.
    runner->run({"curl", "-K", config.string()}, fs::path(), [&lines](const std::string& line){
        lines.push_back(line);
    });
    {
        std::lock_guard<std::mutex> lock(active().mutex);
        active().runners.erase(runner.get());
    }
    {
        std::lock_guard<std::mutex> lock(bodyMutex);
        currentBody.clear();
    }
    std::error_code ec;
    fs::remove(config, ec);
    if (active().cancelled || cancelRequested.load()){
        response.error = "cancelled";
        return response;
    }

    std::string writeOut;
    std::string curlError;
    std::string otherOutput;
    for (const std::string& line : lines){
        if (isWriteOutLine(line))
            writeOut = line;
        else if (line.compare(0, 5, "curl:") == 0)
            curlError = curlMessage(trim(line));
        else if (otherOutput.empty())
            otherOutput = trim(line);
    }

    if (!writeOut.empty()){
        // "<code> <content type, may hold spaces or be empty> <url>"
        response.status = std::atoi(writeOut.substr(0, 3).c_str());
        const size_t lastSpace = writeOut.find_last_of(' ');
        if (lastSpace > 3){
            response.contentType = mediaType(writeOut.substr(4, lastSpace - 4));
            response.finalUrl = unrewrite(trim(writeOut.substr(lastSpace + 1)));
        }
    }
    if (fs::exists(body, ec))
        response.bodyFile = body;

    if (response.status >= 400){
        response.error = "HTTP " + std::to_string(response.status);
    }else if (!curlError.empty()){
        response.error = curlError;
    }else if (writeOut.empty()){
        response.error = otherOutput.empty() ? std::string("curl gave no response") : otherOutput;
    }else if (response.status < 200 || response.status >= 300){
        response.error = response.status == 0 ? std::string("no response") : "HTTP " + std::to_string(response.status);
    }
    return response;
}
