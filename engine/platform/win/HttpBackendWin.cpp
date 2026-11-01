// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The HTTP backend of Windows (exports and the Windows editor): WinHTTP in asynchronous mode
// (WINHTTP_FLAG_ASYNC), so Schannel, the Windows certificate store and the system proxy apply
// and no thread waits on a socket. Every operation completes in the status callback, on
// WinHTTP's own threads; one operation runs per request at a time, so the callbacks of a
// request come one after the other. Closing a request handle is the documented way to cancel
// it (pending operations end with ERROR_WINHTTP_OPERATION_CANCELLED, then
// WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING comes last, which frees the hop). Redirects are
// followed here one hop at a time (HttpTransfer::redirect decides), each hop a new request
// handle. Requests with verifyTls = false run on a session of their own, so a connection that
// skipped the certificate check is never reused by a verified request.

#if defined(_WIN32)

#include "net/HttpBackend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <iphlpapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// names of newer SDKs, for older ones
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_ALL
#define WINHTTP_DECOMPRESSION_FLAG_ALL 0x00000003
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
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

    // an adapter other than loopback and tunnels is up
    bool hasNetwork(){
        ULONG size = 16 * 1024;
        std::vector<unsigned char> buffer(size);
        ULONG result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        if (result == ERROR_BUFFER_OVERFLOW){
            buffer.resize(size);
            result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                          nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        }
        // when it cannot tell, the error stays what WinHTTP said
        if (result != NO_ERROR) return true;
        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next){
            if (a->OperStatus == IfOperStatusUp && a->IfType != IF_TYPE_SOFTWARE_LOOPBACK && a->IfType != IF_TYPE_TUNNEL &&
                    a->FirstUnicastAddress){
                return true;
            }
        }
        return false;
    }

    std::string mapError(DWORD code){
        switch (code){
            case ERROR_WINHTTP_TIMEOUT: return "timeout";
            case ERROR_WINHTTP_NAME_NOT_RESOLVED: return hasNetwork() ? "dns" : "offline";
            case ERROR_WINHTTP_CANNOT_CONNECT: return hasNetwork() ? "connect" : "offline";
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

    struct Job;

    // One request handle (one hop of a transfer): the context of its callbacks, freed at its
    // WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING
    struct Hop {
        enum class End { None, Redirect, Done, Cancel, Timeout, Error };
        std::shared_ptr<Job> job;
        HINTERNET connect = nullptr;
        HINTERNET request = nullptr;
        std::vector<uint8_t> body;          // what WinHttpSendRequest reads until it completes
        std::vector<uint8_t> buffer;        // what WinHttpReadData fills
        bool closed = false;                // the handles were closed (job->mutex)
        End end = End::None;                // why
    };

    struct Job {
        std::shared_ptr<HttpTransfer> transfer;
        std::mutex mutex;                   // the hop pointer and the hops' closed/end
        Hop* current = nullptr;
        bool cancelled = false;
        bool timedOut = false;
    };

    class WinBackend;
    std::atomic<WinBackend*> activeBackend{nullptr};

    class WinBackend : public HttpBackend {
    public:
        WinBackend(){
            verified = openSession(whyNot);
            if (verified) activeBackend = this;
        }

        ~WinBackend() override{
            shutdown();
            if (activeBackend == this) activeBackend = nullptr;
        }

        bool available(std::string& reason) override{
            if (!verified) reason = whyNot;
            return verified != nullptr;
        }

        void start(const std::shared_ptr<HttpTransfer>& transfer) override{
            if (!verified){
                transfer->finish("unsupported", whyNot);
                return;
            }
            auto job = std::make_shared<Job>();
            job->transfer = transfer;
            {
                std::lock_guard<std::mutex> lock(mutex);
                jobs[transfer->id] = job;
                if (transfer->totalTimeout() > 0.0){
                    deadlines[transfer->id] = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds((long long)std::max(1.0, transfer->remainingTimeout() * 1000.0));
                    startWatchdog();
                }
            }
            beginHop(job);
        }

        void cancel(uint64_t id) override{
            std::shared_ptr<Job> job = jobOf(id);
            if (job) stopJob(*job, false);
        }

        void shutdown() override{
            std::vector<std::shared_ptr<Job>> running;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (auto& [id, job] : jobs) running.push_back(job);
            }
            for (auto& job : running) stopJob(*job, false);
            {
                // every hop's HANDLE_CLOSING (none may come after the backend is gone)
                std::unique_lock<std::mutex> lock(mutex);
                hopsGone.wait_for(lock, std::chrono::seconds(10), [this]{ return liveHops == 0; });
                watchdogStop = true;
                watchdogWake.notify_all();
            }
            if (watchdog.joinable()) watchdog.join();
            for (HINTERNET* session : {&verified, &unverified}){
                if (*session){
                    WinHttpSetStatusCallback(*session, nullptr, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0);
                    WinHttpCloseHandle(*session);
                    *session = nullptr;
                }
            }
        }

        // ---- the status callback (WinHTTP's threads)

        static void CALLBACK callback(HINTERNET handle, DWORD_PTR context, DWORD status, LPVOID info, DWORD length){
            Hop* hop = reinterpret_cast<Hop*>(context);
            WinBackend* backend = activeBackend;
            if (!hop || !backend) return;
            if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING){
                if (handle == hop->request) backend->hopClosed(hop);
                return;
            }
            if (handle != hop->request) return;
            switch (status){
                case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
                    if (!backend->live(hop)) return;
                    if (!WinHttpReceiveResponse(hop->request, nullptr)) backend->hopFailed(hop, GetLastError());
                    break;
                case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
                    backend->headersAvailable(hop);
                    break;
                case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE: {
                    const DWORD available = (info && length >= sizeof(DWORD)) ? *static_cast<DWORD*>(info) : 0;
                    backend->dataAvailable(hop, available);
                    break;
                }
                case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
                    backend->readComplete(hop, length);
                    break;
                case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
                    const WINHTTP_ASYNC_RESULT* result = static_cast<const WINHTTP_ASYNC_RESULT*>(info);
                    backend->hopFailed(hop, result ? result->dwError : ERROR_WINHTTP_INTERNAL_ERROR);
                    break;
                }
                default:
                    break;
            }
        }

    private:
        HINTERNET verified = nullptr;     // verifyTls (the default)
        HINTERNET unverified = nullptr;   // verifyTls = false: never shares a connection with the others
        std::string whyNot;

        std::mutex mutex;
        std::unordered_map<uint64_t, std::shared_ptr<Job>> jobs;
        std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> deadlines;   // under `mutex`
        int liveHops = 0;
        std::condition_variable hopsGone;
        std::thread watchdog;
        bool watchdogRunning = false;
        bool watchdogStop = false;
        std::condition_variable watchdogWake;

        HINTERNET openSession(std::string& error){
            // no agent string: the transfer sends its own User-Agent header
            HINTERNET session = WinHttpOpen(nullptr, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                            WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
            if (!session){
                // before Windows 8.1
                session = WinHttpOpen(nullptr, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
            }
            if (!session){
                error = "WinHttpOpen failed: " + errorText(GetLastError());
                return nullptr;
            }
            // TLS 1.2 and 1.3 (Windows 7 and 8 have TLS 1.2 off by default; 1.3 from Windows 11)
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
            if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))){
                protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
                WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
            }
            if (WinHttpSetStatusCallback(session, &WinBackend::callback,
                                         WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) ==
                    WINHTTP_INVALID_STATUS_CALLBACK){
                error = "WinHttpSetStatusCallback failed: " + errorText(GetLastError());
                WinHttpCloseHandle(session);
                return nullptr;
            }
            return session;
        }

        std::shared_ptr<Job> jobOf(uint64_t id){
            std::lock_guard<std::mutex> lock(mutex);
            auto it = jobs.find(id);
            return it == jobs.end() ? nullptr : it->second;
        }

        // the transfer ends: the backend forgets it (finish() is called once by the transfer)
        void forget(const HttpTransfer& transfer){
            std::lock_guard<std::mutex> lock(mutex);
            jobs.erase(transfer.id);
            deadlines.erase(transfer.id);
        }

        void finishJob(Job& job, const std::string& code, const std::string& detail = std::string()){
            std::shared_ptr<HttpTransfer> transfer = job.transfer;
            forget(*transfer);
            transfer->finish(code, detail);
        }

        // false once the hop was closed (by a cancel, the timeout, or its end)
        bool live(Hop* hop){
            std::lock_guard<std::mutex> lock(hop->job->mutex);
            return !hop->closed;
        }

        // closes the hop's handles once; WinHTTP then cancels what is pending and sends
        // HANDLE_CLOSING. True when this call closed them.
        bool closeHop(Hop* hop, Hop::End end){
            HINTERNET request = nullptr, connect = nullptr;
            {
                std::lock_guard<std::mutex> lock(hop->job->mutex);
                if (hop->closed) return false;
                hop->closed = true;
                hop->end = end;
                request = hop->request;
                connect = hop->connect;
            }
            WinHttpCloseHandle(request);
            if (connect) WinHttpCloseHandle(connect);
            return true;
        }

        // a cancel (or the timeout): the current hop's handles close; its HANDLE_CLOSING ends the
        // transfer (a REQUEST_ERROR may come first)
        void stopJob(Job& job, bool timeout){
            Hop* hop = nullptr;
            {
                std::lock_guard<std::mutex> lock(job.mutex);
                if (timeout) job.timedOut = true;
                else job.cancelled = true;
                hop = job.current;
            }
            if (hop) closeHop(hop, timeout ? Hop::End::Timeout : Hop::End::Cancel);
        }

        std::string stopCode(Job& job){
            std::lock_guard<std::mutex> lock(job.mutex);
            if (job.cancelled || job.transfer->isCancelled()) return "cancelled";
            if (job.timedOut) return "timeout";
            return "";
        }

        void beginHop(const std::shared_ptr<Job>& job){
            HttpTransfer& t = *job->transfer;
            const std::string stopped = stopCode(*job);
            if (!stopped.empty()){
                finishJob(*job, stopped);
                return;
            }
            const double remaining = t.remainingTimeout();
            if (remaining <= 0.0){
                finishJob(*job, "timeout");
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
                finishJob(*job, "invalid_url", errorText(GetLastError()));
                return;
            }
            const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
            std::wstring target(parts.lpszUrlPath ? parts.lpszUrlPath : L"", parts.dwUrlPathLength);
            if (parts.lpszExtraInfo) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
            if (target.empty()) target = L"/";
            const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

            HINTERNET session = verified;
            if (!t.verifyTls && secure){
                std::lock_guard<std::mutex> lock(mutex);
                if (!unverified){
                    std::string error;
                    unverified = openSession(error);
                }
                session = unverified;
            }
            if (!session){
                finishJob(*job, "network", "no WinHTTP session");
                return;
            }
            HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
            if (!connect){
                const DWORD code = GetLastError();
                finishJob(*job, mapError(code), errorText(code));
                return;
            }
            const std::wstring method = wide(t.method);
            HINTERNET request = WinHttpOpenRequest(connect, method.c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
            if (!request){
                const DWORD code = GetLastError();
                WinHttpCloseHandle(connect);
                finishJob(*job, mapError(code), errorText(code));
                return;
            }

            Hop* hop = new Hop();
            hop->job = job;
            hop->connect = connect;
            hop->request = request;
            hop->buffer.resize(64 * 1024);
            if (!t.body.empty() && t.method != "GET" && t.method != "HEAD") hop->body = t.body;
            {
                std::lock_guard<std::mutex> lock(mutex);
                liveHops++;
            }
            DWORD_PTR context = reinterpret_cast<DWORD_PTR>(hop);
            WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context));

            const int remainingMs = (int)std::min(2147483647.0, std::max(1.0, remaining * 1000.0));
            const int connectMs = t.connectTimeout > 0.0 ? (int)std::min((double)remainingMs, t.connectTimeout * 1000.0) : remainingMs;
            WinHttpSetTimeouts(request, connectMs, connectMs, remainingMs, remainingMs);
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
            // gzip / deflate decoded by WinHTTP (Windows 8.1+; older ones get the body as sent)
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

            {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->current = hop;
            }
            // a cancel that came in meanwhile
            if (!stopCode(*job).empty()){
                closeHop(hop, Hop::End::Cancel);
                return;
            }

            for (const auto& [name, value] : t.headers){
                const std::wstring line = wide(name) + L": " + wide(value);
                if (!WinHttpAddRequestHeaders(request, line.c_str(), (DWORD)line.size(), WINHTTP_ADDREQ_FLAG_ADD)){
                    // never sent without the headers asked for
                    t.fail("network", "the header " + name + " cannot be sent (" + errorText(GetLastError()) + ")");
                    closeHop(hop, Hop::End::Error);
                    return;
                }
            }

            LPVOID data = hop->body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)hop->body.data();
            const DWORD bodyLength = (DWORD)hop->body.size();
            if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, data, bodyLength, bodyLength, context)){
                hopFailed(hop, GetLastError());
            }
        }

        void headersAvailable(Hop* hop){
            if (!live(hop)) return;
            HttpTransfer& t = *hop->job->transfer;
            HINTERNET request = hop->request;
            DWORD status = 0;
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);
            std::string rawHeaders;
            size = 0;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                                &size, WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && size > 0){
                std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size,
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
                        // this hop ends (its HANDLE_CLOSING frees it), the next one starts
                        if (closeHop(hop, Hop::End::Redirect)) beginHop(hop->job);
                        return;
                    }
                    if (action == HttpRedirect::Action::Fail){
                        closeHop(hop, Hop::End::Error);
                        return;
                    }
                }
            }
            if (!t.begin((int)status, std::move(headers), t.url)){
                closeHop(hop, Hop::End::Error);
                return;
            }
            if (t.head){
                closeHop(hop, Hop::End::Done);
                return;
            }
            if (!WinHttpQueryDataAvailable(request, nullptr)) hopFailed(hop, GetLastError());
        }

        void dataAvailable(Hop* hop, DWORD available){
            if (!live(hop)) return;
            if (available == 0){
                closeHop(hop, Hop::End::Done);
                return;
            }
            const DWORD want = std::min<DWORD>(available, (DWORD)hop->buffer.size());
            if (!WinHttpReadData(hop->request, hop->buffer.data(), want, nullptr)) hopFailed(hop, GetLastError());
        }

        void readComplete(Hop* hop, DWORD read){
            if (!live(hop)) return;
            if (read == 0){
                closeHop(hop, Hop::End::Done);
                return;
            }
            if (!hop->job->transfer->write(hop->buffer.data(), read)){
                closeHop(hop, Hop::End::Error);
                return;
            }
            if (!WinHttpQueryDataAvailable(hop->request, nullptr)) hopFailed(hop, GetLastError());
        }

        void hopFailed(Hop* hop, DWORD code){
            Job& job = *hop->job;
            const std::string stopped = stopCode(job);
            std::string mapped = stopped.empty() ? mapError(code) : stopped;
            if (code == ERROR_WINHTTP_OPERATION_CANCELLED && stopped.empty()) mapped = "cancelled";
            // the transfer's own reason (a refused header, too large...) wins in finish()
            job.transfer->fail(mapped, stopped.empty() ? errorText(code) : std::string());
            closeHop(hop, Hop::End::Error);
        }

        // HANDLE_CLOSING of a hop's request: the last callback it gets
        void hopClosed(Hop* hop){
            std::shared_ptr<Job> job = hop->job;
            Hop::End end = Hop::End::None;
            {
                std::lock_guard<std::mutex> lock(job->mutex);
                end = hop->end;
                if (job->current == hop) job->current = nullptr;
            }
            delete hop;
            if (end != Hop::End::Redirect){
                const std::string stopped = stopCode(*job);
                if (!stopped.empty()) finishJob(*job, stopped);
                else finishJob(*job, end == Hop::End::Done ? "" : "network");
            }
            std::lock_guard<std::mutex> lock(mutex);
            liveHops--;
            if (liveHops == 0) hopsGone.notify_all();
        }

        // with `mutex` held
        void startWatchdog(){
            if (watchdogRunning) return;
            if (watchdog.joinable()) watchdog.join();
            watchdogRunning = true;
            watchdogStop = false;
            watchdog = std::thread([this]() { watchdogLoop(); });
        }

        // whole-request timeouts; ends after 30 s without a request that has one
        void watchdogLoop(){
            auto idleSince = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lock(mutex);
            while (!watchdogStop){
                const auto now = std::chrono::steady_clock::now();
                std::vector<std::shared_ptr<Job>> expired;
                for (auto it = deadlines.begin(); it != deadlines.end();){
                    if (it->second <= now){
                        auto job = jobs.find(it->first);
                        if (job != jobs.end()) expired.push_back(job->second);
                        it = deadlines.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (!deadlines.empty() || !expired.empty()) idleSince = now;
                else if (now - idleSince > std::chrono::seconds(30)) break;
                if (!expired.empty()){
                    lock.unlock();
                    for (auto& job : expired) stopJob(*job, true);
                    lock.lock();
                    continue;
                }
                watchdogWake.wait_for(lock, std::chrono::milliseconds(20));
            }
            watchdogRunning = false;
        }
    };

}

HttpBackend* jasperengine::createPlatformHttpBackend(){
    return new WinBackend();
}

#endif
