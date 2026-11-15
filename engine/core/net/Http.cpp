// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "net/Http.h"
#include "net/HttpBackend.h"
#include "net/HttpJson.h"

#include "Engine.h"
#include "Log.h"
#include "System.h"
#include "io/FileData.h"
#include "io/FileUtils.h"
#include "script/LuaJson.h"
#include "texture/TextureData.h"
#include "thread/MainThreadQueue.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <unordered_map>

#if defined(_WIN32)
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace jasperengine;

namespace fs = std::filesystem;

// HttpTransfer -> the client (defined below)
void jasperengineHttpTransferFinished(uint64_t id, jasperengine::HttpResponse&& response);
void jasperengineHttpNotifyProgress();
void jasperengineHttpResetBackend();

#ifndef JASPERENGINE_VERSION_STRING
#define JASPERENGINE_VERSION_STRING ""
#endif

// =============================================================================================
// file helpers (UTF-8 paths on every platform)

namespace {

    fs::path pathOf(const std::string& utf8){
#if defined(_WIN32)
        if (utf8.empty()) return fs::path();
        const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
        if (size <= 0) return fs::path(utf8);
        std::wstring wide((size_t)size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &wide[0], size);
        return fs::path(wide);
#else
        return fs::path(utf8);
#endif
    }

    FILE* openSystemFile(const std::string& utf8, const char* mode){
#if defined(_WIN32)
        const std::wstring wideMode(mode, mode + std::strlen(mode));
        return _wfsopen(pathOf(utf8).c_str(), wideMode.c_str(), _SH_DENYNO);
#else
        return std::fopen(utf8.c_str(), mode);
#endif
    }

    std::string errnoText(int error){
        switch (error){
            case ENOENT: return "not found";
            case EACCES:
            case EPERM: return "permission denied";
            case EISDIR: return "is a folder";
            case ENOSPC: return "no space left on the device";
            case EROFS: return "read-only file system";
            default: return std::strerror(error);
        }
    }

    bool isAbsolutePath(const std::string& path){
        if (!path.empty() && (path[0] == '/' || path[0] == '\\')) return true;
        return path.size() >= 2 && path[1] == ':' && std::isalpha((unsigned char)path[0]);
    }

    std::string genericPath(std::string path){
        std::replace(path.begin(), path.end(), '\\', '/');
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        return path;
    }

    bool sameText(const std::string& a, const std::string& b, bool caseInsensitive){
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++){
            char x = a[i], y = b[i];
            if (caseInsensitive){
                x = (char)std::tolower((unsigned char)x);
                y = (char)std::tolower((unsigned char)y);
            }
            if (x != y) return false;
        }
        return true;
    }

    // The system path a download writes to: data://..., a bare relative path (data://), or an
    // absolute path inside the user data folder
    bool resolveDownloadPath(const std::string& given, std::string& systemPath, std::string& error){
        if (given.empty()){
            error = "the download needs a file path (data://...)";
            return false;
        }
        if (isAbsolutePath(given)){
            const std::string userData = genericPath(System::instance().getUserDataPath());
            const std::string path = genericPath(given);
#if defined(_WIN32)
            const bool caseInsensitive = true;
#else
            const bool caseInsensitive = false;
#endif
            const bool inside = !userData.empty() && path.size() > userData.size() + 1 &&
                                sameText(path.substr(0, userData.size()), userData, caseInsensitive) &&
                                path[userData.size()] == '/';
            if (!inside || path.find("/../") != std::string::npos || (path.size() >= 3 && path.compare(path.size() - 3, 3, "/..") == 0)){
                error = "a download goes to data://... or a path inside System.getUserDataPath() (" + userData + "): " + given;
                return false;
            }
            systemPath = given;
            return true;
        }
        std::string normalized;
        if (!FileUtils::resolve(given, false, normalized, systemPath, error)) return false;
        if (normalized.compare(0, 7, "data://") != 0){
            error = "a download goes to data://... (asset:// and lua:// are read-only): " + given;
            return false;
        }
        if (normalized.size() <= 7){
            error = "the download path names a folder, not a file: " + given;
            return false;
        }
        return true;
    }

    int64_t fileSize(const std::string& utf8){
        std::error_code ec;
        const auto size = fs::file_size(pathOf(utf8), ec);
        return ec ? -1 : (int64_t)size;
    }

    void removeSystemFile(const std::string& utf8){
        std::error_code ec;
        fs::remove(pathOf(utf8), ec);
    }

    bool renameSystemFile(const std::string& from, const std::string& to, std::string& error){
        std::error_code ec;
        fs::rename(pathOf(from), pathOf(to), ec);
        if (!ec) return true;
#if defined(_WIN32)
        // a file in the way that rename does not replace
        if (MoveFileExW(pathOf(from).c_str(), pathOf(to).c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
#endif
        error = ec.message();
        return false;
    }

    bool makeParentFolders(const std::string& utf8, std::string& error){
        const fs::path parent = pathOf(utf8).parent_path();
        if (parent.empty()) return true;
        std::error_code ec;
        if (fs::is_directory(parent, ec)) return true;
        fs::create_directories(parent, ec);
        if (ec){
            error = ec.message();
            return false;
        }
        return true;
    }

    // <part>.meta: the validator of the response a .part holds (for If-Range)
    std::string metaPathOf(const std::string& partPath){
        return partPath + ".meta";
    }

    bool readPartValidator(const std::string& partPath, std::string& validator){
        FILE* file = openSystemFile(metaPathOf(partPath), "rb");
        if (!file) return false;
        char line[1024] = {0};
        const bool got = std::fgets(line, sizeof(line), file) != nullptr;
        std::fclose(file);
        if (!got) return false;
        validator = line;
        while (!validator.empty() && (validator.back() == '\n' || validator.back() == '\r')) validator.pop_back();
        return !validator.empty();
    }

    void writePartValidator(const std::string& partPath, const std::string& validator){
        if (validator.empty()){
            std::error_code ec;
            fs::remove(pathOf(metaPathOf(partPath)), ec);
            return;
        }
        FILE* file = openSystemFile(metaPathOf(partPath), "wb");
        if (!file) return;
        std::fputs((validator + "\n").c_str(), file);
        std::fclose(file);
    }

    const std::string* findHeader(const HttpHeaders& headers, const std::string& lowerName){
        for (const auto& [name, value] : headers){
            if (name == lowerName) return &value;
        }
        return nullptr;
    }

    bool isSuccess(int status){
        return status >= 200 && status <= 299;
    }

    int64_t parseLength(const std::string* text){
        if (!text || text->empty()) return -1;
        int64_t value = 0;
        for (char c : *text){
            if (c < '0' || c > '9') return -1;
            if (value > (INT64_MAX - 9) / 10) return -1;
            value = value * 10 + (c - '0');
        }
        return value;
    }

    // "bytes 100-199/1000": start 100, total 1000 (-1 when "*")
    bool parseContentRange(const std::string* text, int64_t& start, int64_t& total){
        start = -1;
        total = -1;
        if (!text) return false;
        std::string value = HttpUtil::lower(*text);
        if (value.compare(0, 6, "bytes ") != 0) return false;
        value = value.substr(6);
        const size_t dash = value.find('-');
        const size_t slash = value.find('/');
        if (dash == std::string::npos || slash == std::string::npos || dash > slash) return false;
        const std::string startText = value.substr(0, dash);
        start = parseLength(&startText);
        const std::string totalText = value.substr(slash + 1);
        total = totalText == "*" ? -1 : parseLength(&totalText);
        return start >= 0;
    }

    std::string originOf(const std::string& url){
        HttpUtil::Url parsed;
        if (!HttpUtil::parseUrl(url, parsed)) return url;
        return parsed.scheme + "://" + HttpUtil::lower(parsed.host) + ":" + std::to_string(parsed.port);
    }

    std::string hostOf(const std::string& url){
        HttpUtil::Url parsed;
        return HttpUtil::parseUrl(url, parsed) ? parsed.host : std::string();
    }

    // the request headers that describe a body (dropped with the body on a redirect)
    bool isBodyHeader(const std::string& lowerName){
        return lowerName == "content-type" || lowerName == "content-length" || lowerName == "content-encoding" ||
               lowerName == "content-language" || lowerName == "content-location";
    }

    // headers a redirect to another origin must not carry
    bool isCredentialHeader(const std::string& lowerName){
        return lowerName == "authorization" || lowerName == "cookie" || lowerName == "proxy-authorization";
    }

}

// =============================================================================================
// HttpResponse, HttpRequestHandle

const std::string* HttpResponse::header(const std::string& name) const{
    const std::string lowerName = HttpUtil::lower(name);
    return findHeader(headers, lowerName);
}

TextureData HttpResponse::takeTexture() const{
    if (!texture) return TextureData();
    TextureData copy = *texture;
    copy.setDataOwned(false);
    texture->setDataOwned(false);
    return copy;
}

HttpRequestHandle::HttpRequestHandle(uint64_t id, std::string method, std::string url)
    : m_id(id), m_method(std::move(method)), m_url(std::move(url)) {
}

float HttpRequestHandle::progress() const{
    if (m_state == HttpState::Done) return 1.0f;
    if (m_total <= 0) return m_total == 0 && m_state != HttpState::Queued ? 1.0f : -1.0f;
    return (float)std::min(1.0, (double)m_received / (double)m_total);
}

// =============================================================================================
// HttpTransfer

HttpTransfer::HttpTransfer(uint64_t id, HttpRequestDesc desc, const std::string& userAgent, bool allowInsecure,
                           std::string partPath, std::string finalPath)
    : id(id) {
    method = desc.method;
    url = HttpUtil::stripFragment(desc.url);
    body = std::move(desc.body);
    connectTimeout = desc.connectTimeout;
    verifyTls = desc.verifyTls;
    maxRedirects = std::max(0, desc.maxRedirects);
    this->allowInsecure = allowInsecure;
    head = method == "HEAD";

    m_type = desc.responseType;
    m_timeout = desc.timeout;
    m_maxBody = desc.maxBodySize;
    m_keepPartial = desc.keepPartial;
    m_resume = desc.resume;
    m_partPath = std::move(partPath);
    m_finalPath = std::move(finalPath);
    m_givenPath = desc.filePath;
    m_originalUrl = url;
    m_start = std::chrono::steady_clock::now();
    m_localHost = HttpUtil::isLocalHost(hostOf(url));
    // a failed download's or texture's error body: what the server says, not a whole file
    m_errorBodyLimit = (m_type == HttpResponseType::File || m_type == HttpResponseType::Texture) ? (1u << 20) : m_maxBody;

    bool hasUserAgent = false;
    for (const auto& header : desc.headers){
        if (HttpUtil::lower(header.first) == "user-agent") hasUserAgent = true;
    }
    headers = std::move(desc.headers);
    if (!hasUserAgent && !userAgent.empty()) headers.emplace_back("User-Agent", userAgent);

    if (m_type == HttpResponseType::File && m_resume){
        // a part continues only what the same file on the server started: its validator
        // (the ETag, else Last-Modified) goes in If-Range, so a changed file comes back whole
        // (200) instead of as a range of something else. A part without one starts over.
        const int64_t size = fileSize(m_partPath);
        std::string validator;
        if (size > 0 && readPartValidator(m_partPath, validator)){
            m_resumeOffset = (uint64_t)size;
            headers.emplace_back("Range", "bytes=" + std::to_string(size) + "-");
            headers.emplace_back("If-Range", validator);
            // byte offsets of the file as stored, not of a compressed copy
            headers.emplace_back("Accept-Encoding", "identity");
        }
    }
}

HttpTransfer::~HttpTransfer(){
    closeFile();
}

double HttpTransfer::remainingTimeout() const{
    if (m_timeout <= 0.0) return 1e9;
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
    return m_timeout - elapsed;
}

bool HttpTransfer::isRedirectStatus(int status){
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

HttpRedirect::Action HttpTransfer::redirect(int status, const std::string& location){
    if (maxRedirects <= 0 || location.empty()) return HttpRedirect::Action::Stop;
    if (m_redirects >= maxRedirects){
        m_failCode = "too_many_redirects";
        m_failDetail = "more than " + std::to_string(maxRedirects) + " redirect" + (maxRedirects == 1 ? "" : "s");
        return HttpRedirect::Action::Fail;
    }
    const std::string next = HttpUtil::stripFragment(HttpUtil::resolveUrl(url, location));
    std::string policyError;
    const std::string code = HttpUtil::checkPolicy(next, allowInsecure, policyError);
    if (!code.empty()){
        m_failCode = code;
        m_failDetail = "redirected to " + next + ": " + policyError;
        return HttpRedirect::Action::Fail;
    }
    // Private Network Access, as browsers do: a public host does not send a request to
    // loopback, private or link-local addresses (a router's admin page, local services)
    if (!HttpUtil::isPrivateHost(hostOf(url)) && HttpUtil::isPrivateHost(hostOf(next))){
        m_failCode = "redirect_blocked";
        m_failDetail = "a public host redirected to a private network address (" + next + ")";
        return HttpRedirect::Action::Fail;
    }
    // browser rules: 303 turns everything but HEAD into GET; 301/302 turn POST into GET;
    // 307/308 keep the method and the body
    bool dropBody = false;
    if (status == 303 && method != "HEAD" && method != "GET"){
        method = "GET";
        dropBody = true;
    } else if ((status == 301 || status == 302) && method == "POST"){
        method = "GET";
        dropBody = true;
    }
    const bool crossOrigin = originOf(next) != originOf(url);
    HttpHeaders kept;
    for (auto& header : headers){
        const std::string name = HttpUtil::lower(header.first);
        if (dropBody && isBodyHeader(name)) continue;
        if (crossOrigin && isCredentialHeader(name)) continue;
        kept.push_back(std::move(header));
    }
    headers = std::move(kept);
    if (dropBody) body.clear();
    url = next;
    m_localHost = HttpUtil::isLocalHost(hostOf(url));
    m_redirects++;
    return HttpRedirect::Action::Follow;
}

bool HttpTransfer::begin(int status, HttpHeaders responseHeaders, const std::string& finalUrl){
    if (m_begun) return true;
    m_begun = true;
    m_status = status;
    for (auto& header : responseHeaders){
        header.first = HttpUtil::lower(header.first);
    }
    m_responseHeaders = std::move(responseHeaders);
    m_finalUrl = finalUrl.empty() ? url : HttpUtil::stripFragment(finalUrl);
    if (isCancelled()) return false;
    // a stack that followed redirects itself (the browser) reached this URL without asking
    // redirect(): the same rules apply to it
    if (m_finalUrl != url){
        std::string policyError;
        const std::string code = HttpUtil::checkPolicy(m_finalUrl, allowInsecure, policyError);
        if (!code.empty() && code != "invalid_url"){
            m_failCode = code;
            m_failDetail = "redirected to " + m_finalUrl + ": " + policyError;
            return false;
        }
        if (!HttpUtil::isPrivateHost(hostOf(m_originalUrl)) && HttpUtil::isPrivateHost(hostOf(m_finalUrl))){
            m_failCode = "redirect_blocked";
            m_failDetail = "a public host redirected to a private network address (" + m_finalUrl + ")";
            return false;
        }
    }

    // the length of the body as it arrives (a compressed body's Content-Length is not)
    const std::string* encoding = findHeader(m_responseHeaders, "content-encoding");
    const bool encoded = encoding && HttpUtil::lower(*encoding) != "identity" && !encoding->empty();
    const int64_t length = encoded ? -1 : parseLength(findHeader(m_responseHeaders, "content-length"));
    int64_t total = length;

    if (m_type == HttpResponseType::File && isSuccess(status) && !head){
        uint64_t offset = 0;
        if (status == 206 && m_resumeOffset > 0){
            int64_t rangeStart = -1, rangeTotal = -1;
            if (!parseContentRange(findHeader(m_responseHeaders, "content-range"), rangeStart, rangeTotal) ||
                    (uint64_t)rangeStart != m_resumeOffset){
                m_failCode = "http";
                m_failDetail = "the server answered the resume with another range (" +
                               (findHeader(m_responseHeaders, "content-range") ? *findHeader(m_responseHeaders, "content-range") : std::string("none")) + ")";
                removePart();
                return false;
            }
            offset = m_resumeOffset;
            total = rangeTotal >= 0 ? rangeTotal : (length >= 0 ? (int64_t)offset + length : -1);
        }
        std::string error;
        if (!makeParentFolders(m_partPath, error)){
            m_failCode = "file";
            m_failDetail = "cannot make the folder of " + m_givenPath + " (" + error + ")";
            return false;
        }
        m_file = openSystemFile(m_partPath, offset > 0 ? "ab" : "wb");
        if (!m_file){
            m_failCode = "file";
            m_failDetail = "cannot write " + m_givenPath + ".part (" + errnoText(errno) + ")";
            return false;
        }
        m_toFile = true;
        m_received.store(offset, std::memory_order_release);
        if (offset == 0){
            // what a later resume sends in If-Range: a strong ETag, else Last-Modified
            const std::string* etag = findHeader(m_responseHeaders, "etag");
            const std::string* modified = findHeader(m_responseHeaders, "last-modified");
            std::string validator;
            if (etag && !etag->empty() && etag->compare(0, 2, "W/") != 0) validator = *etag;
            else if (modified) validator = *modified;
            writePartValidator(m_partPath, validator);
        }
    } else if (isSuccess(status) && !head && m_maxBody > 0 && length >= 0 && (uint64_t)length > m_maxBody &&
               m_type != HttpResponseType::File){
        m_failCode = "too_large";
        m_failDetail = "the response has " + std::to_string(length) + " bytes, more than maxBodySize (" + std::to_string(m_maxBody) + ")";
        return false;
    }
    if (m_type == HttpResponseType::File && status == 416 && m_resumeOffset > 0){
        // the .part does not fit what the server has now: the next try starts over
        m_keepPartial = false;
    }
    m_total.store(total, std::memory_order_release);
    if (!m_progressDirty.exchange(true, std::memory_order_acq_rel)){
        jasperengineHttpNotifyProgress();
    }
    return true;
}

bool HttpTransfer::write(const void* data, size_t size){
    if (isCancelled()) return false;
    if (size == 0) return true;
    if (!m_begun){
        // a backend that reports no head: take it as a 200
        if (!begin(200, {}, url)) return false;
    }
    if (head) return true;
    if (m_toFile){
        if (std::fwrite(data, 1, size, m_file) != size || std::fflush(m_file) != 0){
            m_failCode = "file";
            m_failDetail = "cannot write " + m_givenPath + ".part (" + errnoText(errno) + ")";
            return false;
        }
    } else {
        const bool success = isSuccess(m_status);
        const size_t limit = success ? m_maxBody : m_errorBodyLimit;
        if (limit > 0 && m_memory.size() + size > limit){
            if (success){
                m_failCode = "too_large";
                m_failDetail = "the response is larger than maxBodySize (" + std::to_string(m_maxBody) + " bytes)";
                return false;
            }
            // an error body: its beginning is enough
            const size_t room = limit > m_memory.size() ? limit - m_memory.size() : 0;
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            m_memory.insert(m_memory.end(), bytes, bytes + room);
        } else {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            m_memory.insert(m_memory.end(), bytes, bytes + size);
        }
    }
    m_received.fetch_add(size, std::memory_order_acq_rel);
    if (!m_progressDirty.exchange(true, std::memory_order_acq_rel)){
        // the first news since the last frame took it
        jasperengineHttpNotifyProgress();
    }
    return true;
}

void HttpTransfer::closeFile(){
    if (m_file){
        std::fclose(m_file);
        m_file = nullptr;
    }
}

void HttpTransfer::removePart(){
    closeFile();
    if (!m_partPath.empty()){
        removeSystemFile(m_partPath);
        removeSystemFile(metaPathOf(m_partPath));
    }
}

void HttpTransfer::fail(const std::string& code, const std::string& detail){
    if (m_failCode.empty()){
        m_failCode = code;
        m_failDetail = detail;
    }
}

bool HttpTransfer::requestCancel(const std::string& errorCode){
    std::lock_guard<std::mutex> lock(m_commitMutex);
    if (m_committed) return false;
    if (!m_cancelled.load(std::memory_order_acquire)){
        m_cancelCode = errorCode;
        m_cancelled.store(true, std::memory_order_release);
    }
    return true;
}

bool HttpTransfer::takeProgress(uint64_t& received, int64_t& total){
    const bool dirty = m_progressDirty.exchange(false, std::memory_order_acq_rel);
    received = m_received.load(std::memory_order_acquire);
    total = m_total.load(std::memory_order_acquire);
    return dirty;
}

std::string HttpTransfer::describeFailure(const std::string& code, const std::string& detail, double elapsed) const{
    const std::string where = ": " + url;
    const std::string more = detail.empty() ? std::string() : " (" + detail + ")";
    if (code == "timeout"){
        if (!m_begun && (m_timeout <= 0.0 || elapsed + 0.05 < m_timeout) && connectTimeout > 0.0){
            return "connect timeout after " + HttpUtil::seconds(connectTimeout) + " s" + where;
        }
        return "timeout after " + HttpUtil::seconds(m_timeout > 0.0 ? m_timeout : elapsed) + " s" + where;
    }
    if (code == "dns") return "could not resolve the host " + hostOf(url) + more + where;
    if (code == "connect") return "could not connect to " + hostOf(url) + more + where;
    if (code == "tls") return "TLS error" + more + where;
    if (code == "network") return "network error" + more + where;
    if (code == "cancelled") return "cancelled" + where;
    if (code == "offline"){
        if (detail == "simulated") return "offline (simulated: the Game view's Network is set to Offline)" + where;
        return "offline (no network connection)" + where;
    }
    if (code == "too_many_redirects") return "too many redirects" + more + where;
    if (detail.empty()) return code + where;
    return detail + where;
}

void HttpTransfer::finish(const std::string& errorCode, const std::string& detail){
    if (m_finished.exchange(true, std::memory_order_acq_rel)) return;

    bool fileError = false;
    if (m_file){
        if (std::fclose(m_file) != 0) fileError = true;
        m_file = nullptr;
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();

    HttpResponse response;
    response.method = method;
    response.url = m_begun ? m_finalUrl : url;
    response.redirected = m_redirects > 0;
    response.elapsed = elapsed;
    response.filePath = m_type == HttpResponseType::File ? m_givenPath : std::string();
    response.bytesReceived = m_received.load(std::memory_order_acquire);
    response.headers = m_responseHeaders;
    response.status = m_begun ? m_status : 0;

    std::string code;
    std::string reason = detail;
    if (!m_failCode.empty()){
        code = m_failCode;
        reason = m_failDetail;
    } else if (!errorCode.empty()){
        code = errorCode;
    } else if (!m_begun){
        code = "network";
        reason = "no response";
    } else if (fileError){
        code = "file";
        reason = "cannot write " + m_givenPath + ".part";
    }

    bool keepBody = true;
    if (code.empty()){
        response.ok = isSuccess(m_status);
        if (!response.ok){
            code = "http";
            const char* phrase = HttpUtil::reasonPhrase(m_status);
            response.error = "HTTP " + std::to_string(m_status) + (*phrase ? std::string(" ") + phrase : std::string()) + ": " + response.url;
        }
        if (response.ok && m_type == HttpResponseType::Texture){
            auto texture = std::make_shared<TextureData>();
            if (!m_memory.empty() && texture->loadTextureFromMemory(m_memory.data(), (unsigned int)m_memory.size()) &&
                    texture->getData() && texture->getWidth() > 0 && texture->getHeight() > 0){
                texture->setDataOwned(true);
                response.texture = std::move(texture);
                keepBody = false;
            } else {
                response.ok = false;
                code = "decode";
                response.error = "not an image TextureData can load (PNG, JPEG, BMP, TGA, GIF): " + response.url;
            }
        }
        if (m_type == HttpResponseType::Json && !m_memory.empty()){
            auto json = std::make_shared<HttpJson>();
            std::string parseError;
            const std::string_view text(reinterpret_cast<const char*>(m_memory.data()), m_memory.size());
            if (LuaJson::parse(text, json->value, LuaJson::DecodeOptions(), parseError)){
                response.json = std::move(json);
            } else if (response.ok){
                response.ok = false;
                code = "decode";
                response.error = "invalid JSON (" + parseError + "): " + response.url;
            }
        } else if (m_type == HttpResponseType::Json && response.ok && !head && m_status != 204){
            response.ok = false;
            code = "decode";
            response.error = "invalid JSON (the body is empty): " + response.url;
        }
    } else {
        response.error = describeFailure(code, reason, elapsed);
    }

    // a cancel that came first wins; once committed, a cancel is too late
    {
        std::lock_guard<std::mutex> lock(m_commitMutex);
        if (m_cancelled.load(std::memory_order_acquire)){
            response.ok = false;
            code = m_cancelCode.empty() ? "cancelled" : m_cancelCode;
            response.error = describeFailure(code, code == "offline" ? "simulated" : std::string(), elapsed);
            response.texture.reset();
            response.json.reset();
        } else {
            m_committed = true;
            if (response.ok && m_toFile){
                std::string error;
                if (!renameSystemFile(m_partPath, m_finalPath, error)){
                    response.ok = false;
                    code = "file";
                    response.error = "cannot replace " + m_givenPath + " (" + error + "): " + response.url;
                }
            }
        }
    }
    if (m_type == HttpResponseType::File && !response.ok && m_toFile && !m_keepPartial){
        removePart();
    } else if (m_type == HttpResponseType::File && !response.ok && !m_keepPartial && m_resumeOffset > 0){
        // a resumed .part that this try did not open (a 416, another range)
        removePart();
    } else if (m_type == HttpResponseType::File && response.ok){
        removeSystemFile(metaPathOf(m_partPath));
    }
    response.errorCode = response.ok ? std::string() : code;
    if (response.ok) response.error.clear();
    if (keepBody && !head) response.body = std::move(m_memory);
    m_memory = std::vector<uint8_t>();
    m_progressDirty.store(true, std::memory_order_release);

    jasperengineHttpTransferFinished(id, std::move(response));
}

// =============================================================================================
// the client

namespace jasperengine {

    struct HttpCore {
        struct Entry {
            uint64_t id = 0;
            std::shared_ptr<HttpRequestHandle> handle;
            HttpRequestDesc desc;              // until started
            HttpCallback onComplete;
            HttpProgress onProgress;
            AsyncOwner owner;
            bool local = false;                // the host is localhost
            bool isFile = false;               // File
            std::string filePath;              // File: as given
            std::string partPath, finalPath;   // File: system paths
            std::shared_ptr<HttpTransfer> transfer;
            bool started = false;
            bool silent = false;               // the owner went, or shutdown: no callback
            bool backendDone = false;          // `result` is there
            bool userCancelled = false;
            std::shared_ptr<HttpResponse> result;
            uint64_t lastReceived = 0;
            int64_t lastTotal = -2;
        };
        using EntryPtr = std::shared_ptr<Entry>;

        std::mutex mutex;
        std::unordered_map<uint64_t, EntryPtr> entries;
        std::deque<uint64_t> queue;           // not started, FIFO
        std::deque<uint64_t> completed;       // results to deliver, in arrival order
        std::vector<uint64_t> toCancel;       // running ones to cancel in the backend (owner gone off the main thread)
        int running = 0;                      // started in the backend, not finished there
        bool pumpPosted = false;
        uint64_t pumpEpoch = 0;
        bool ownerHooked = false;

        // settings (main thread writes; read under the lock)
        int maxConcurrent = 8;
        std::string userAgent;
        double defaultTimeout = 30.0;
        bool logFailures = false;
        bool offline = false;
        bool allowInsecure = false;

        // stats
        uint32_t finishedCount = 0, failedCount = 0, cancelledCount = 0;
        std::deque<HttpResponse> lastFailures;

        // the backend (main thread)
        HttpBackend* backend = nullptr;
        bool backendChecked = false;
        bool backendAvailable = false;
        std::string backendWhyNot;

        std::set<std::string> warnedHeaders;
        std::set<std::string> warnedTlsHosts;

        static std::atomic<uint64_t> nextId;

        static HttpCore& instance(){
            static HttpCore* core = new HttpCore();
            return *core;
        }

        // with the lock held
        void requestPump(){
            const uint64_t epoch = MainThreadQueue::epoch();
            if (pumpPosted && pumpEpoch == epoch) return;
            pumpPosted = true;
            pumpEpoch = epoch;
            MainThreadQueue::post([]() { HttpCore::instance().pump(); });
        }

        HttpBackend* ensureBackend(){
            if (!backendChecked){
                backendChecked = true;
                backend = createPlatformHttpBackend();
                if (backend){
                    backendAvailable = backend->available(backendWhyNot);
                } else {
                    backendWhyNot = "HTTP is not supported on this platform";
                }
                if (!backendAvailable){
                    Log::warn("Http: %s", backendWhyNot.c_str());
                }
            }
            return backendAvailable ? backend : nullptr;
        }

        std::string currentUserAgent(){
            if (!userAgent.empty()) return userAgent;
            const char* platform = "Linux";
            switch (Engine::getPlatform()){
                case Platform::MacOS: platform = "macOS"; break;
                case Platform::iOS: platform = "iOS"; break;
                case Platform::Web: platform = "Web"; break;
                case Platform::Android: platform = "Android"; break;
                case Platform::Linux: platform = "Linux"; break;
                case Platform::Windows: platform = "Windows"; break;
            }
            const std::string version = JASPERENGINE_VERSION_STRING;
            return std::string("JasperEngine") + (version.empty() ? "" : "/" + version) + " (" + platform + ")";
        }

        std::shared_ptr<HttpResponse> failure(const Entry& entry, const std::string& code, const std::string& error){
            auto response = std::make_shared<HttpResponse>();
            response->method = entry.handle->method();
            response->url = entry.handle->url();
            response->errorCode = code;
            response->error = error;
            response->filePath = entry.isFile ? entry.filePath : std::string();
            return response;
        }

        // with the lock held: the entry ends now with `response` (delivered in the next frame)
        void finishEarly(const EntryPtr& entry, std::shared_ptr<HttpResponse> response){
            entry->result = std::move(response);
            entry->backendDone = true;
            completed.push_back(entry->id);
            requestPump();
        }

        // the response of a request cancelled before its transfer said so
        std::shared_ptr<HttpResponse> cancelledResponse(const Entry& entry){
            auto response = failure(entry, "cancelled", "cancelled: " + entry.handle->url());
            if (entry.transfer) response->bytesReceived = entry.transfer->received();
            return response;
        }

        void ownerGone(uint64_t token){
            std::vector<EntryPtr> doomed;
            std::vector<uint64_t> cancels;
            const bool mainThread = MainThreadQueue::isMainThread();
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (auto it = entries.begin(); it != entries.end();){
                    Entry& entry = *it->second;
                    if (entry.owner.token != token || entry.silent){
                        ++it;
                        continue;
                    }
                    entry.silent = true;
                    if (entry.started && !entry.backendDone && entry.transfer && entry.transfer->requestCancel("cancelled")){
                        if (mainThread) cancels.push_back(entry.id);
                        else toCancel.push_back(entry.id);
                    }
                    if (mainThread){
                        doomed.push_back(std::move(it->second));
                        it = entries.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (!mainThread) requestPump();
            }
            if (backend){
                for (uint64_t id : cancels) backend->cancel(id);
            }
            // callbacks (Lua references) go on the main thread; off it the pump erases them
            for (const EntryPtr& entry : doomed) markEnded(*entry);
        }

        // a handle whose request ended without a delivery
        void markEnded(Entry& entry){
            if (!entry.handle->isDone()){
                entry.handle->m_state = HttpState::Cancelled;
                if (!entry.handle->m_response) entry.handle->m_response = cancelledResponse(entry);
            }
            entry.onComplete = nullptr;
            entry.onProgress = nullptr;
        }

        void transferFinished(uint64_t id, HttpResponse&& response){
            std::lock_guard<std::mutex> lock(mutex);
            running = std::max(0, running - 1);
            auto it = entries.find(id);
            if (it != entries.end() && !it->second->silent && !it->second->backendDone){
                Entry& entry = *it->second;
                // a cancel answered by the client already keeps its response object
                if (!entry.result) entry.result = std::make_shared<HttpResponse>(std::move(response));
                entry.backendDone = true;
                completed.push_back(id);
            }
            requestPump();
        }

        void notifyProgress(){
            std::lock_guard<std::mutex> lock(mutex);
            requestPump();
        }

        // with the lock held: whether the entry can start now (false: it waits)
        bool startable(const Entry& entry){
            // the web build restores the user data after it starts: a download waits for it
            if (entry.isFile && !System::instance().isUserDataLoaded()) return false;
            return true;
        }

        // main thread, no lock held
        void startEntries(){
            std::vector<EntryPtr> starting;
            bool waiting = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                std::deque<uint64_t> rest;
                while (!queue.empty()){
                    const uint64_t id = queue.front();
                    queue.pop_front();
                    auto it = entries.find(id);
                    if (it == entries.end() || it->second->silent || it->second->backendDone || it->second->started) continue;
                    const EntryPtr& entry = it->second;
                    if (running + (int)starting.size() >= maxConcurrent){
                        rest.push_back(id);
                        continue;
                    }
                    if (!startable(*entry)){
                        waiting = true;
                        rest.push_back(id);
                        continue;
                    }
                    if (offline && !entry->local){
                        finishEarly(entry, failure(*entry, "offline", "offline (simulated: the Game view's Network is set to Offline): " + entry->handle->url()));
                        continue;
                    }
                    entry->started = true;
                    entry->handle->m_state = HttpState::Running;
                    entry->transfer = std::make_shared<HttpTransfer>(entry->id, std::move(entry->desc), currentUserAgent(), allowInsecure,
                                                                     entry->partPath, entry->finalPath);
                    entry->desc = HttpRequestDesc();
                    starting.push_back(entry);
                }
                queue.swap(rest);
                running += (int)starting.size();
                if (waiting) requestPump();
            }
            HttpBackend* b = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex);
                b = ensureBackend();
            }
            for (const EntryPtr& entry : starting){
                if (b){
                    b->start(entry->transfer);
                } else {
                    entry->transfer->finish("unsupported", backendWhyNot);
                }
            }
        }

        void deliver(const EntryPtr& entry){
            HttpResponse& response = *entry->result;
            HttpRequestHandle& handle = *entry->handle;
            const bool cancelled = response.errorCode == "cancelled";
            handle.m_state = cancelled ? HttpState::Cancelled : HttpState::Done;
            if (handle.m_response != entry->result) handle.m_response = entry->result;
            handle.m_received = response.bytesReceived;
            if (entry->transfer && entry->transfer->total() >= 0) handle.m_total = entry->transfer->total();
            else if (response.ok && handle.m_total < 0) handle.m_total = (int64_t)response.bytesReceived;

            bool log = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (response.ok){
                    finishedCount++;
                } else if (cancelled){
                    cancelledCount++;
                } else {
                    failedCount++;
                    HttpResponse copy;
                    copy.ok = false;
                    copy.status = response.status;
                    copy.errorCode = response.errorCode;
                    copy.error = response.error;
                    copy.method = response.method;
                    copy.url = response.url;
                    copy.redirected = response.redirected;
                    copy.filePath = response.filePath;
                    copy.elapsed = response.elapsed;
                    copy.bytesReceived = response.bytesReceived;
                    lastFailures.push_back(std::move(copy));
                    while (lastFailures.size() > 5) lastFailures.pop_front();
                    log = logFailures;
                }
            }
            if (log){
                Log::warn("HTTP %s %s failed: %s", response.method.c_str(), handle.url().c_str(), response.error.c_str());
            }
            if (response.ok && !response.filePath.empty()){
                // the web build's IndexedDB
                System::instance().syncFileSystem();
            }
            HttpCallback callback = std::move(entry->onComplete);
            entry->onComplete = nullptr;
            entry->onProgress = nullptr;
            if (callback){
                try {
                    callback(response);
                } catch (const std::exception& e) {
                    Log::error("Http: the callback of %s %s failed: %s", response.method.c_str(), handle.url().c_str(), e.what());
                }
            }
        }

        void pump(){
            std::vector<EntryPtr> doomed;
            std::vector<uint64_t> cancels;
            {
                std::lock_guard<std::mutex> lock(mutex);
                pumpPosted = false;
                // owners that went off the main thread
                cancels.swap(toCancel);
                for (auto it = entries.begin(); it != entries.end();){
                    if (it->second->silent){
                        doomed.push_back(std::move(it->second));
                        it = entries.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (const EntryPtr& entry : doomed) markEnded(*entry);
            doomed.clear();
            if (backend){
                for (uint64_t id : cancels) backend->cancel(id);
            }

            startEntries();

            // progress of every request, then the completions in the order they came
            std::vector<EntryPtr> progressing;
            std::vector<EntryPtr> done;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& [id, entry] : entries){
                    if (entry->transfer && !entry->silent) progressing.push_back(entry);
                }
                std::sort(progressing.begin(), progressing.end(), [](const EntryPtr& a, const EntryPtr& b) { return a->id < b->id; });
                while (!completed.empty()){
                    auto it = entries.find(completed.front());
                    completed.pop_front();
                    if (it != entries.end()) done.push_back(it->second);
                }
            }
            for (const EntryPtr& entry : progressing){
                if (entry->silent) continue;
                uint64_t received = 0;
                int64_t total = -1;
                if (!entry->transfer->takeProgress(received, total)) continue;
                if (entry->userCancelled || entry->handle->isDone()) continue;
                entry->handle->m_received = received;
                entry->handle->m_total = total;
                if (entry->onProgress && (received != entry->lastReceived || total != entry->lastTotal)){
                    entry->lastReceived = received;
                    entry->lastTotal = total;
                    HttpProgress progress = entry->onProgress;
                    try {
                        progress(received, total);
                    } catch (const std::exception& e) {
                        Log::error("Http: the progress callback of %s failed: %s", entry->handle->url().c_str(), e.what());
                    }
                }
            }
            for (const EntryPtr& entry : done){
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (entry->silent) continue;
                    auto it = entries.find(entry->id);
                    if (it == entries.end()) continue;
                    entries.erase(it);
                }
                // an owner gone while its result waited (the hooks run before this)
                if (!AsyncOwners::alive(entry->owner)){
                    markEnded(*entry);
                    continue;
                }
                deliver(entry);
            }
        }

        std::shared_ptr<HttpRequestHandle> send(HttpRequestDesc desc, HttpCallback onComplete, HttpProgress onProgress, AsyncOwner owner, bool& startNow){
            startNow = false;
            const uint64_t id = nextId.fetch_add(1);
            std::string method = desc.method.empty() ? std::string("GET") : desc.method;
            const std::string upper = [&]{ std::string m = method; for (char& c : m) c = (char)std::toupper((unsigned char)c); return m; }();
            if (upper == "GET" || upper == "POST" || upper == "PUT" || upper == "PATCH" || upper == "DELETE" || upper == "HEAD" || upper == "OPTIONS") method = upper;
            desc.method = method;
            const std::string url = HttpUtil::appendQuery(HttpUtil::stripFragment(desc.url), desc.query);
            desc.url = url;
            desc.query.clear();

            auto entry = std::make_shared<Entry>();
            entry->id = id;
            entry->handle = std::make_shared<HttpRequestHandle>(id, method, url);
            entry->onComplete = std::move(onComplete);
            entry->onProgress = std::move(onProgress);
            entry->owner = owner;
            entry->isFile = desc.responseType == HttpResponseType::File;
            entry->filePath = desc.filePath;

            HttpUtil::Url parsed;
            entry->local = HttpUtil::parseUrl(url, parsed) && HttpUtil::isLocalHost(parsed.host);

            if (!AsyncOwners::alive(owner)){
                // nothing to run it for
                entry->desc = std::move(desc);
                markEnded(*entry);
                return entry->handle;
            }

            // the stacks manage these themselves
            HttpHeaders headers;
            for (auto& header : desc.headers){
                const std::string name = HttpUtil::lower(header.first);
                if (name == "host" || name == "content-length" || name == "transfer-encoding" || name == "connection" || name == "accept-encoding"){
                    if (warnedHeaders.insert(name).second){
                        Log::warn("Http: the %s header is set by the platform's HTTP stack; the one given is ignored%s", header.first.c_str(),
                                  name == "accept-encoding" ? " (gzip is accepted and decoded on every platform)" : "");
                    }
                    continue;
                }
                if (header.first.empty() || !HttpUtil::isToken(header.first) || header.second.find_first_of("\r\n") != std::string::npos){
                    if (warnedHeaders.insert("invalid:" + header.first).second){
                        Log::warn("Http: header \"%s\" is not a valid header (a token name, a value without line breaks); it is not sent", header.first.c_str());
                    }
                    continue;
                }
                headers.push_back(std::move(header));
            }
            desc.headers = std::move(headers);
            // one default on every platform for a body of unknown type (the stacks' own differ:
            // a form, nothing); Lua tables, json and form set theirs
            if (!desc.body.empty() && method != "GET" && method != "HEAD"){
                bool typed = false;
                for (const auto& header : desc.headers){
                    if (HttpUtil::lower(header.first) == "content-type") typed = true;
                }
                if (!typed) desc.headers.emplace_back("Content-Type", "application/octet-stream");
            }
            if (!desc.verifyTls && parsed.scheme == "https" && warnedTlsHosts.insert(HttpUtil::lower(parsed.host)).second){
#if defined(__EMSCRIPTEN__)
                Log::warn("Http: verifyTls = false is ignored on the web (browsers always verify certificates): %s", parsed.host.c_str());
#else
                Log::warn("Http: TLS certificate verification is off for %s (verifyTls = false): anyone on the network can read and change this traffic", parsed.host.c_str());
#endif
            }

            std::string error;
            std::string code;
            if (!HttpUtil::isToken(method)){
                code = "unsupported";
                error = "invalid HTTP method \"" + method + "\": " + url;
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (code.empty()) code = HttpUtil::checkPolicy(url, allowInsecure, error);
            if (code.empty() && offline && !entry->local){
                code = "offline";
                error = "offline (simulated: the Game view's Network is set to Offline): " + url;
            }
            if (code.empty() && desc.responseType == HttpResponseType::File){
                std::string systemPath;
                if (!resolveDownloadPath(desc.filePath, systemPath, error)){
                    code = "file";
                    error += ": " + url;
                } else {
                    entry->finalPath = systemPath;
                    entry->partPath = systemPath + ".part";
                    // one download per file: two would write the same .part
                    const std::string key = genericPath(systemPath);
                    for (const auto& [otherId, other] : entries){
                        if (other->isFile && !other->silent && !other->backendDone && genericPath(other->finalPath) == key){
                            code = "file";
                            error = desc.filePath + " is being downloaded already (request " + std::to_string(otherId) +
                                    "): wait for it or cancel it: " + url;
                            break;
                        }
                    }
                }
            }
            if (code.empty() && !backendChecked){
                // the first request loads the platform's stack (Linux: dlopen of libcurl)
                ensureBackend();
            }
            if (code.empty() && !backendAvailable){
                code = "unsupported";
                error = backendWhyNot + ": " + url;
            }
            entry->desc = std::move(desc);
            entries.emplace(id, entry);
            if (!code.empty()){
                finishEarly(entry, failure(*entry, code, error));
                return entry->handle;
            }
            queue.push_back(id);
            // starts at once when a slot is free (inside a callback too); delivers no earlier
            // than the next frame
            startNow = running < maxConcurrent;
            return entry->handle;
        }

        void cancel(uint64_t id){
            uint64_t backendCancel = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto it = entries.find(id);
                if (it == entries.end()) return;
                const EntryPtr& entry = it->second;
                if (entry->silent || entry->backendDone || entry->userCancelled) return;
                if (!entry->started){
                    queue.erase(std::remove(queue.begin(), queue.end(), id), queue.end());
                    entry->userCancelled = true;
                    auto response = cancelledResponse(*entry);
                    entry->handle->m_state = HttpState::Cancelled;
                    entry->handle->m_response = response;
                    finishEarly(entry, response);
                    return;
                }
                if (!entry->transfer->requestCancel("cancelled")) return;   // the response is there
                entry->userCancelled = true;
                auto response = cancelledResponse(*entry);
                entry->handle->m_state = HttpState::Cancelled;
                entry->handle->m_response = response;
                entry->result = response;
                backendCancel = id;
            }
            if (backendCancel && backend) backend->cancel(backendCancel);
        }

        void cancelAll(){
            std::vector<uint64_t> ids;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& [id, entry] : entries) ids.push_back(id);
            }
            std::sort(ids.begin(), ids.end());
            for (uint64_t id : ids) cancel(id);
        }

        void shutdown(){
            std::vector<EntryPtr> doomed;
            std::vector<uint64_t> cancels;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (auto& [id, entry] : entries){
                    entry->silent = true;
                    if (entry->started && !entry->backendDone && entry->transfer && entry->transfer->requestCancel("cancelled")){
                        cancels.push_back(id);
                    }
                    doomed.push_back(std::move(entry));
                }
                entries.clear();
                queue.clear();
                completed.clear();
                toCancel.clear();
                finishedCount = failedCount = cancelledCount = 0;
                // what scripts set lasts for their run (Play, the game): the defaults again
                maxConcurrent = 8;
                userAgent.clear();
                defaultTimeout = 30.0;
                logFailures = false;
                lastFailures.clear();
                pumpPosted = false;
            }
            if (backend){
                for (uint64_t id : cancels) backend->cancel(id);
            }
            for (const EntryPtr& entry : doomed) markEnded(*entry);
        }

        void setOffline(bool value){
            std::vector<uint64_t> cancels;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (offline == value) return;
                offline = value;
                if (!value) return;
                for (const auto& [id, entry] : entries){
                    if (entry->started && !entry->backendDone && !entry->silent && entry->transfer &&
                            !entry->transfer->isLocalHost() && entry->transfer->requestCancel("offline")){
                        cancels.push_back(id);
                    }
                }
            }
            if (backend){
                for (uint64_t id : cancels) backend->cancel(id);
            }
        }
    };

    std::atomic<uint64_t> HttpCore::nextId{1};

}

void jasperengineHttpTransferFinished(uint64_t id, HttpResponse&& response){
    HttpCore::instance().transferFinished(id, std::move(response));
}

void jasperengineHttpNotifyProgress(){
    HttpCore::instance().notifyProgress();
}

void HttpRequestHandle::cancel(){
    HttpCore::instance().cancel(m_id);
}

// =============================================================================================
// Http

std::shared_ptr<HttpRequestHandle> Http::send(HttpRequestDesc desc, HttpCallback onComplete, HttpProgress onProgress, AsyncOwner owner){
    HttpCore& core = HttpCore::instance();
    if (!core.ownerHooked){
        core.ownerHooked = true;
        AsyncOwners::onOwnerGone().add("Http", [](uint64_t token) { HttpCore::instance().ownerGone(token); });
    }
    if (!desc.reportProgress && onProgress) desc.reportProgress = true;
    if (!desc.reportProgress) onProgress = nullptr;
    bool startNow = false;
    std::shared_ptr<HttpRequestHandle> handle = core.send(std::move(desc), std::move(onComplete), std::move(onProgress), owner, startNow);
    if (startNow) core.startEntries();
    return handle;
}

bool Http::isAvailable(){
    HttpCore& core = HttpCore::instance();
    std::lock_guard<std::mutex> lock(core.mutex);
    core.ensureBackend();
    return core.backendAvailable;
}

std::string Http::unavailableReason(){
    HttpCore& core = HttpCore::instance();
    std::lock_guard<std::mutex> lock(core.mutex);
    core.ensureBackend();
    return core.backendAvailable ? std::string() : core.backendWhyNot;
}

void Http::cancelAll(){
    HttpCore::instance().cancelAll();
}

void Http::shutdown(){
    HttpCore::instance().shutdown();
}

void Http::terminate(){
    HttpCore::instance().shutdown();
    // the platform stack's threads end here (joined); the next request starts them again
    jasperengineHttpResetBackend();
}

int Http::getMaxConcurrentRequests(){
    HttpCore& core = HttpCore::instance();
    std::lock_guard<std::mutex> lock(core.mutex);
    return core.maxConcurrent;
}

void Http::setMaxConcurrentRequests(int n){
    HttpCore& core = HttpCore::instance();
    {
        std::lock_guard<std::mutex> lock(core.mutex);
        core.maxConcurrent = std::clamp(n, 1, 64);
        if (!core.queue.empty()) core.requestPump();
    }
}

std::string Http::getUserAgent(){
    HttpCore& core = HttpCore::instance();
    std::lock_guard<std::mutex> lock(core.mutex);
    return core.currentUserAgent();
}

void Http::setUserAgent(const std::string& userAgent){
    HttpCore& core = HttpCore::instance();
    std::lock_guard<std::mutex> lock(core.mutex);
    core.userAgent = userAgent.find_first_of("\r\n") == std::string::npos ? userAgent : std::string();
}

double Http::getDefaultTimeout(){
    HttpCore& core = HttpCore::instance();
    std::lock_guard<std::mutex> lock(core.mutex);
    return core.defaultTimeout;
}

void Http::setDefaultTimeout(double seconds){
    HttpCore& core = HttpCore::instance();
    std::lock_guard<std::mutex> lock(core.mutex);
    core.defaultTimeout = seconds > 0.0 ? seconds : 0.0;
}

bool Http::getLogFailures(){
    HttpCore& core = HttpCore::instance();
    std::lock_guard<std::mutex> lock(core.mutex);
    return core.logFailures;
}

void Http::setLogFailures(bool log){
    HttpCore& core = HttpCore::instance();
    std::lock_guard<std::mutex> lock(core.mutex);
    core.logFailures = log;
}

bool Http::isOfflineSimulated(){
    HttpCore& core = HttpCore::instance();
    std::lock_guard<std::mutex> lock(core.mutex);
    return core.offline;
}

void Http::setOfflineSimulated(bool offline){
    HttpCore::instance().setOffline(offline);
}

bool Http::isInsecureHttpAllowed(){
    HttpCore& core = HttpCore::instance();
    std::lock_guard<std::mutex> lock(core.mutex);
    return core.allowInsecure;
}

void Http::setInsecureHttpAllowed(bool allowed){
    HttpCore& core = HttpCore::instance();
    std::lock_guard<std::mutex> lock(core.mutex);
    core.allowInsecure = allowed;
}

Http::Stats Http::getStats(){
    HttpCore& core = HttpCore::instance();
    std::lock_guard<std::mutex> lock(core.mutex);
    Stats stats;
    stats.active = (uint32_t)std::max(0, core.running);
    uint32_t queued = 0;
    for (uint64_t id : core.queue){
        auto it = core.entries.find(id);
        if (it != core.entries.end() && !it->second->silent) queued++;
    }
    stats.queued = queued;
    stats.finished = core.finishedCount;
    stats.failed = core.failedCount;
    stats.cancelled = core.cancelledCount;
    stats.lastFailures.assign(core.lastFailures.begin(), core.lastFailures.end());
    return stats;
}

std::string Http::encodeUrl(const std::string& text){
    return HttpUtil::encode(text);
}

std::string Http::decodeUrl(const std::string& text){
    return HttpUtil::decode(text, true);
}

// =============================================================================================
// test seams

void HttpTesting::deliver(){
    HttpCore::instance().pump();
}

int HttpTesting::runningTransfers(){
    HttpCore& core = HttpCore::instance();
    std::lock_guard<std::mutex> lock(core.mutex);
    return core.running;
}

void jasperengineHttpResetBackend(){
    HttpCore& core = HttpCore::instance();
    HttpBackend* old = nullptr;
    {
        std::lock_guard<std::mutex> lock(core.mutex);
        old = core.backend;
        core.backend = nullptr;
        core.backendChecked = false;
        core.backendAvailable = false;
        core.backendWhyNot.clear();
    }
    // joins the worker, which may still report to the client
    if (old){
        old->shutdown();
        delete old;
    }
}
