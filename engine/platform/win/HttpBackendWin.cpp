// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The HTTP backend of Windows (exports and the Windows editor): WinHTTP in synchronous mode on
// a pool of worker threads (made when requests come, one per running request at most, ended
// after 30 s without work), so Schannel, the Windows certificate store and the system proxy
// apply. Redirects are followed here one hop at a time (HttpTransfer::redirect decides).
// A request is cancelled, and the whole-request timeout enforced, by closing its handle from
// another thread: WinHTTP's documented way to end a synchronous call.

#if defined(_WIN32)

#include "net/HttpBackend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Windows 8.1 SDK names, for older SDKs
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_ALL
#define WINHTTP_DECOMPRESSION_FLAG_ALL 0x00000003
#endif

using namespace jasperengine;

namespace {

    std::wstring wide(const std::string& utf8){
        if (utf8.empty()) return std::wstring();
        const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
        if (size <= 0) return std::wstring();
        std::wstring out((size_t)size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &out[0], size);
        return out;
    }

    std::string narrow(const wchar_t* text, size_t length){
        if (!text || length == 0) return std::string();
        const int size = WideCharToMultiByte(CP_UTF8, 0, text, (int)length, nullptr, 0, nullptr, nullptr);
        if (size <= 0) return std::string();
        std::string out((size_t)size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, (int)length, &out[0], size, nullptr, nullptr);
        return out;
    }

    std::string errorText(DWORD code){
        wchar_t* buffer = nullptr;
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD length = FormatMessageW(flags, GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
        std::string text = length ? narrow(buffer, length) : std::string();
        if (buffer) LocalFree(buffer);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) text.pop_back();
        return text.empty() ? "error " + std::to_string(code) : text + " (" + std::to_string(code) + ")";
    }

    struct Job {
        std::shared_ptr<HttpTransfer> transfer;
        std::mutex mutex;
        HINTERNET connect = nullptr;   // open while a hop runs; null once closed
        HINTERNET request = nullptr;
        bool closedByOther = false;    // a cancel or the timeout closed the handles
        bool timedOut = false;
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();

        // the handle to call WinHTTP with now, null when it was closed
        HINTERNET current(){
            std::lock_guard<std::mutex> lock(mutex);
            return request;
        }

        // closes this hop's handles once (the worker or another thread)
        void close(bool byOther, bool timeout = false){
            HINTERNET r = nullptr, c = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex);
                r = request;
                c = connect;
                request = nullptr;
                connect = nullptr;
                if (byOther && r){
                    closedByOther = true;
                    if (timeout) timedOut = true;
                }
            }
            if (r) WinHttpCloseHandle(r);
            if (c) WinHttpCloseHandle(c);
        }
    };

    class WinBackend : public HttpBackend {
    public:
        WinBackend(){
            // no agent string: the transfer sends its own User-Agent header
            session = WinHttpOpen(nullptr, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!session){
                // before Windows 8.1
                session = WinHttpOpen(nullptr, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            }
            if (!session) whyNot = "WinHttpOpen failed: " + errorText(GetLastError());
        }

        ~WinBackend() override{
            shutdown();
            if (session) WinHttpCloseHandle(session);
        }

        bool available(std::string& reason) override{
            if (!session) reason = whyNot;
            return session != nullptr;
        }

        void start(const std::shared_ptr<HttpTransfer>& transfer) override{
            if (!session){
                transfer->finish("unsupported", whyNot);
                return;
            }
            auto job = std::make_shared<Job>();
            job->transfer = transfer;
            if (transfer->totalTimeout() > 0.0){
                job->deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds((long long)std::max(1.0, transfer->remainingTimeout() * 1000.0));
            }
            std::lock_guard<std::mutex> lock(mutex);
            stopping = false;
            jobs[transfer->id] = job;
            pending.push_back(job);
            // one worker per waiting request at most (the client keeps the concurrency limit)
            if ((int)pending.size() > idleWorkers){
                workers++;
                std::thread([this]() { workerLoop(); }).detach();
            }
            if (!watchdog.joinable()){
                watchdogRunning = true;
                watchdog = std::thread([this]() { watchdogLoop(); });
            }
            condition.notify_one();
        }

        void cancel(uint64_t id) override{
            std::shared_ptr<Job> job;
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto it = jobs.find(id);
                if (it != jobs.end()) job = it->second;
            }
            // a queued one sees isCancelled() when a worker takes it
            if (job) job->close(true);
        }

        void shutdown() override{
            std::vector<std::shared_ptr<Job>> running;
            {
                std::lock_guard<std::mutex> lock(mutex);
                stopping = true;
                for (auto& [id, job] : jobs) running.push_back(job);
                condition.notify_all();
            }
            for (auto& job : running) job->close(true);
            {
                std::unique_lock<std::mutex> lock(mutex);
                exited.wait(lock, [this]{ return workers == 0; });
                watchdogRunning = false;
            }
            if (watchdog.joinable()) watchdog.join();
        }

    private:
        HINTERNET session = nullptr;
        std::string whyNot;

        std::mutex mutex;
        std::condition_variable condition;
        std::condition_variable exited;
        std::deque<std::shared_ptr<Job>> pending;
        std::unordered_map<uint64_t, std::shared_ptr<Job>> jobs;
        int workers = 0;
        int idleWorkers = 0;
        bool stopping = false;
        std::thread watchdog;
        bool watchdogRunning = false;

        void workerLoop(){
            std::unique_lock<std::mutex> lock(mutex);
            while (true){
                idleWorkers++;
                const bool got = condition.wait_for(lock, std::chrono::seconds(30), [this]{ return !pending.empty() || stopping; });
                idleWorkers--;
                if (!got || (stopping && pending.empty())) break;
                std::shared_ptr<Job> job = std::move(pending.front());
                pending.pop_front();
                lock.unlock();
                run(*job);
                lock.lock();
                jobs.erase(job->transfer->id);
            }
            workers--;
            exited.notify_all();
        }

        void watchdogLoop(){
            while (true){
                std::vector<std::shared_ptr<Job>> expired;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    if (!watchdogRunning) return;
                    const auto now = std::chrono::steady_clock::now();
                    for (auto& [id, job] : jobs){
                        if (job->deadline <= now) expired.push_back(job);
                    }
                }
                for (auto& job : expired){
                    job->close(true, true);
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->timedOut = true;
                    job->deadline = std::chrono::steady_clock::time_point::max();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }

        static std::string mapError(DWORD code){
            switch (code){
                case ERROR_WINHTTP_TIMEOUT: return "timeout";
                case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "dns";
                case ERROR_WINHTTP_CANNOT_CONNECT: return "connect";
                case ERROR_WINHTTP_SECURE_FAILURE:
                case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
                case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
                case ERROR_WINHTTP_SECURE_INVALID_CA:
                case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
                case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
                case ERROR_WINHTTP_SECURE_INVALID_CERT:
                case ERROR_WINHTTP_SECURE_CERT_REVOKED:
                case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
                case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED: return "tls";
                case ERROR_WINHTTP_INVALID_URL:
                case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return "invalid_url";
                default: return "network";
            }
        }

        // after a failed WinHTTP call: the transfer's end
        void fail(Job& job, DWORD code){
            bool byOther = false, timedOut = false;
            {
                std::lock_guard<std::mutex> lock(job.mutex);
                byOther = job.closedByOther;
                timedOut = job.timedOut;
            }
            job.close(false);
            HttpTransfer& t = *job.transfer;
            if (t.isCancelled() || (byOther && !timedOut)) t.finish("cancelled");
            else if (timedOut) t.finish("timeout");
            else t.finish(mapError(code), errorText(code));
        }

        void run(Job& job){
            HttpTransfer& t = *job.transfer;
            if (t.isCancelled()){
                t.finish("cancelled");
                return;
            }
            while (true){
                const double remaining = t.remainingTimeout();
                if (remaining <= 0.0){
                    t.finish("timeout");
                    return;
                }
                const std::wstring url = wide(t.url);
                URL_COMPONENTS parts{};
                parts.dwStructSize = sizeof(parts);
                parts.dwSchemeLength = (DWORD)-1;
                parts.dwHostNameLength = (DWORD)-1;
                parts.dwUrlPathLength = (DWORD)-1;
                parts.dwExtraInfoLength = (DWORD)-1;
                if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &parts)){
                    t.finish("invalid_url", errorText(GetLastError()));
                    return;
                }
                const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
                std::wstring target(parts.lpszUrlPath ? parts.lpszUrlPath : L"", parts.dwUrlPathLength);
                if (parts.lpszExtraInfo) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
                if (target.empty()) target = L"/";
                const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

                HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
                if (!connect){
                    fail(job, GetLastError());
                    return;
                }
                const std::wstring method = wide(t.method);
                HINTERNET request = WinHttpOpenRequest(connect, method.c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                       WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
                if (!request){
                    const DWORD code = GetLastError();
                    WinHttpCloseHandle(connect);
                    fail(job, code);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(job.mutex);
                    job.connect = connect;
                    job.request = request;
                }
                if (t.isCancelled()){
                    job.close(false);
                    t.finish("cancelled");
                    return;
                }

                const int remainingMs = (int)std::min(2147483647.0, std::max(1.0, remaining * 1000.0));
                const int connectMs = t.connectTimeout > 0.0 ? (int)std::min((double)remainingMs, t.connectTimeout * 1000.0) : remainingMs;
                WinHttpSetTimeouts(request, connectMs, connectMs, remainingMs, remainingMs);
                DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
                WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
                // gzip / deflate, decoded by WinHTTP (Windows 8.1+; older ones get the body as sent)
                DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
                WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
                // no cookie jar on any platform
                DWORD features = WINHTTP_DISABLE_COOKIES;
                WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof(features));
                if (!t.verifyTls && secure){
                    DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                                  SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
                    WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
                }
                std::wstring headerText;
                for (const auto& [name, value] : t.headers){
                    headerText += wide(name) + L": " + wide(value) + L"\r\n";
                }
                if (!headerText.empty()){
                    WinHttpAddRequestHeaders(request, headerText.c_str(), (DWORD)headerText.size(), WINHTTP_ADDREQ_FLAG_ADD);
                }

                const bool sendsBody = !t.body.empty() && t.method != "GET" && t.method != "HEAD";
                LPVOID data = sendsBody ? (LPVOID)t.body.data() : WINHTTP_NO_REQUEST_DATA;
                const DWORD length = sendsBody ? (DWORD)t.body.size() : 0;
                HINTERNET h = job.current();
                if (!h || !WinHttpSendRequest(h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, data, length, length, 0)){
                    fail(job, h ? GetLastError() : ERROR_WINHTTP_OPERATION_CANCELLED);
                    return;
                }
                h = job.current();
                if (!h || !WinHttpReceiveResponse(h, nullptr)){
                    fail(job, h ? GetLastError() : ERROR_WINHTTP_OPERATION_CANCELLED);
                    return;
                }

                DWORD status = 0;
                DWORD size = sizeof(status);
                WinHttpQueryHeaders(h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status, &size, WINHTTP_NO_HEADER_INDEX);
                std::string rawHeaders;
                size = 0;
                WinHttpQueryHeaders(h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                                    &size, WINHTTP_NO_HEADER_INDEX);
                if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && size > 0){
                    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
                    if (WinHttpQueryHeaders(h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size,
                                            WINHTTP_NO_HEADER_INDEX)){
                        rawHeaders = narrow(buffer.data(), size / sizeof(wchar_t));
                    }
                }
                HttpHeaders headers = HttpUtil::parseHeaderBlock(rawHeaders);

                if (HttpTransfer::isRedirectStatus((int)status)){
                    std::string location;
                    for (const auto& [name, value] : headers){
                        if (name == "location") location = value;
                    }
                    if (!location.empty()){
                        const HttpRedirect::Action action = t.redirect((int)status, location);
                        if (action == HttpRedirect::Action::Follow){
                            job.close(false);
                            continue;
                        }
                        if (action == HttpRedirect::Action::Fail){
                            job.close(false);
                            t.finish("");
                            return;
                        }
                    }
                }

                if (!t.begin((int)status, std::move(headers), t.url)){
                    job.close(false);
                    t.finish("");
                    return;
                }
                if (!t.head){
                    std::vector<uint8_t> buffer(64 * 1024);
                    while (true){
                        h = job.current();
                        DWORD available = 0;
                        if (!h || !WinHttpQueryDataAvailable(h, &available)){
                            fail(job, h ? GetLastError() : ERROR_WINHTTP_OPERATION_CANCELLED);
                            return;
                        }
                        if (available == 0) break;
                        DWORD read = 0;
                        const DWORD want = std::min<DWORD>(available, (DWORD)buffer.size());
                        h = job.current();
                        if (!h || !WinHttpReadData(h, buffer.data(), want, &read)){
                            fail(job, h ? GetLastError() : ERROR_WINHTTP_OPERATION_CANCELLED);
                            return;
                        }
                        if (read == 0) break;
                        if (!t.write(buffer.data(), read)){
                            job.close(false);
                            t.finish(t.isCancelled() ? "cancelled" : "");
                            return;
                        }
                    }
                }
                bool cancelledMeanwhile = false;
                {
                    std::lock_guard<std::mutex> lock(job.mutex);
                    cancelledMeanwhile = job.closedByOther && !job.timedOut;
                }
                job.close(false);
                t.finish(cancelledMeanwhile || t.isCancelled() ? "cancelled" : "");
                return;
            }
        }
    };

}

HttpBackend* jasperengine::createPlatformHttpBackend(){
    return new WinBackend();
}

#endif
