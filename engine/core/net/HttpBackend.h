// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_HTTPBACKEND_H
#define JASPERENGINE_HTTPBACKEND_H

// Internal: the interface between the HTTP client (Http.cpp, the same on every platform) and
// a platform's stack (HttpBackendCurl.cpp, HttpBackendWin.cpp, HttpBackendApple.mm,
// HttpBackendAndroid.cpp, HttpBackendWeb.cpp). A backend moves bytes: it sends one request
// (one hop) at a time and hands the response to the HttpTransfer, which does the rest the same
// way everywhere (redirect rules, the plain-HTTP policy of every hop, body limits, files and
// resume, texture and JSON decoding, error texts, progress).

#include "Export.h"
#include "net/Http.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine {

    using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

    struct HttpRedirect {
        enum class Action {
            Stop,       // deliver this 3xx as the response (redirects off)
            Follow,     // send the next hop: the transfer's method, url, headers and body are updated
            Fail        // abort; finish() reports the reason
        };
        Action action = Action::Stop;
    };

    // One request, from start to finish. The backend calls the "backend" functions from one
    // thread at a time (its worker, its delegate queue, the browser's event loop), in this
    // order per hop: redirect() for a 3xx that has a Location, else begin() once and write()
    // for each body chunk; then finish() exactly once for the whole request. A false return
    // of begin()/write() or a Fail redirect means: abort the transfer, then call finish()
    // (the transfer keeps the reason; the backend's own error is ignored then).
    class JASPERENGINE_API HttpTransfer {
    public:
        // ---- the request of the current hop (read by the backend; redirect() changes them)
        const uint64_t id;
        std::string method;
        std::string url;               // without the fragment
        HttpHeaders headers;           // what to send (User-Agent included; Range when resuming)
        std::vector<uint8_t> body;
        double connectTimeout = 15.0;  // s, <= 0: the platform's
        bool verifyTls = true;
        int maxRedirects = 10;
        bool allowInsecure = false;
        bool head = false;             // HEAD: no body is read

        // seconds left of the whole request's timeout (a large number when there is none);
        // <= 0: the time is up (finish with "timeout")
        double remainingTimeout() const;
        // the timeout in seconds (<= 0: none)
        double totalTimeout() const { return m_timeout; }
        // Followed hops so far
        int redirectCount() const { return m_redirects; }

        // ---- backend calls
        // `status` is 301/302/303/307/308 with a Location (absolute or relative)
        static bool isRedirectStatus(int status);
        HttpRedirect::Action redirect(int status, const std::string& location);
        // the final response's head (names in any case, in order; `finalUrl` "" = url)
        bool begin(int status, HttpHeaders responseHeaders, const std::string& finalUrl = std::string());
        // a stack that follows redirects itself (the browser's fetch) says it did
        void stackRedirected(){ if (m_redirects == 0) m_redirects = 1; }
        bool write(const void* data, size_t size);
        // `errorCode` "" when the backend read everything; else timeout, dns, connect, tls,
        // network, offline, insecure_http, invalid_url, unsupported, cancelled, ...; `detail`
        // the platform's own message (logged in the error text)
        void finish(const std::string& errorCode, const std::string& detail = std::string());

        // Cancelled (or failed with the offline simulation): stop as soon as possible and call
        // finish(); polling backends read it between chunks
        bool isCancelled() const { return m_cancelled.load(std::memory_order_acquire); }

        // ---- the client (Http.cpp)
        HttpTransfer(uint64_t id, HttpRequestDesc desc, const std::string& userAgent, bool allowInsecure,
                     std::string partPath, std::string finalPath);
        ~HttpTransfer();
        HttpTransfer(const HttpTransfer&) = delete;
        HttpTransfer& operator=(const HttpTransfer&) = delete;

        // false when the response arrived already (cancel is too late)
        bool requestCancel(const std::string& errorCode);
        bool takeProgress(uint64_t& received, int64_t& total);   // true when it changed since the last take
        uint64_t received() const { return m_received.load(std::memory_order_acquire); }
        int64_t total() const { return m_total.load(std::memory_order_acquire); }
        bool isFinished() const { return m_finished.load(std::memory_order_acquire); }
        bool isLocalHost() const { return m_localHost.load(std::memory_order_acquire); }

    private:
        HttpResponseType m_type;
        double m_timeout = 30.0;
        size_t m_maxBody = 0;
        bool m_keepPartial = false;
        bool m_resume = false;
        std::string m_partPath;        // File: <system path>.part
        std::string m_finalPath;       // File: the system path
        std::string m_givenPath;       // File: as given
        std::string m_originalUrl;
        std::chrono::steady_clock::time_point m_start;
        std::atomic<bool> m_localHost{false};

        int m_redirects = 0;
        int m_status = 0;
        bool m_begun = false;
        HttpHeaders m_responseHeaders;
        std::string m_finalUrl;
        std::vector<uint8_t> m_memory;
        size_t m_errorBodyLimit = 0;
        FILE* m_file = nullptr;
        bool m_toFile = false;
        uint64_t m_resumeOffset = 0;   // bytes of the .part kept at begin()

        std::string m_failCode;        // why begin/write/redirect stopped it
        std::string m_failDetail;

        std::atomic<uint64_t> m_received{0};
        std::atomic<int64_t> m_total{-1};
        std::atomic<bool> m_progressDirty{false};
        std::atomic<bool> m_cancelled{false};
        std::atomic<bool> m_finished{false};
        std::mutex m_commitMutex;      // cancel vs. the response's arrival
        bool m_committed = false;
        std::string m_cancelCode;

        void closeFile();
        void removePart();
        std::string describeFailure(const std::string& code, const std::string& detail, double elapsed) const;
    };

    class HttpBackend {
    public:
        virtual ~HttpBackend() = default;
        // false with the reason when the platform's stack cannot work here
        virtual bool available(std::string& whyNot) = 0;
        // main thread; the backend keeps the transfer until it called finish()
        virtual void start(const std::shared_ptr<HttpTransfer>& transfer) = 0;
        // main thread; the transfer is cancelled already (isCancelled): end it soon
        virtual void cancel(uint64_t id) = 0;
        // stops the workers (joins them); the backend is not used afterwards
        virtual void shutdown() = 0;
    };

    // One definition per platform (the file the build picks)
    HttpBackend* createPlatformHttpBackend();

    // Shared helpers (HttpUtil.cpp), also used by the backends and the tests
    namespace HttpUtil {
        struct Url {
            std::string scheme;     // "http" / "https" (lowercase)
            std::string host;       // without brackets for IPv6
            int port = 0;           // the default one when not given
            bool explicitPort = false;
            std::string target;     // path + query ("/" at least), no fragment
        };
        // false for anything but http[s]://host[:port][/...] without spaces or control bytes
        JASPERENGINE_API bool parseUrl(const std::string& url, Url& out);
        JASPERENGINE_API bool isLocalHost(const std::string& host);
        // "" when allowed, else "invalid_url" / "insecure_http" (with `error` the text)
        JASPERENGINE_API std::string checkPolicy(const std::string& url, bool allowInsecure, std::string& error);
        // RFC 3986 reference resolution of a Location against the URL it came from
        JASPERENGINE_API std::string resolveUrl(const std::string& base, const std::string& reference);
        JASPERENGINE_API std::string stripFragment(const std::string& url);
        // url + "?a=1&b=2" (or "&..."), before any fragment
        JASPERENGINE_API std::string appendQuery(const std::string& url, const HttpHeaders& query);
        JASPERENGINE_API std::string encode(const std::string& text);
        JASPERENGINE_API std::string decode(const std::string& text, bool plusAsSpace);
        JASPERENGINE_API std::string lower(std::string text);
        // the standard reason phrase ("Not Found"), "" when unknown
        JASPERENGINE_API const char* reasonPhrase(int status);
        // a method a request may have (an HTTP token)
        JASPERENGINE_API bool isToken(const std::string& text);
        // "10", "0.3", "2.5"
        JASPERENGINE_API std::string seconds(double value);
        // parses "Name: value" header lines of a raw block (CRLF or LF; continuation lines joined)
        JASPERENGINE_API HttpHeaders parseHeaderBlock(const std::string& raw, int* status = nullptr);
    }

    // Test seams (editor unit tests): the client keeps its backend for the process
    struct JASPERENGINE_API HttpTesting {
        // Linux: the libraries the curl backend tries ({} = the default list); recreates the
        // backend (joining its worker). Not while requests run.
        static void setCurlLibraryNames(const std::vector<std::string>& names);
        // the curl backend's library list (the default one when not set)
        static std::vector<std::string> curlLibraryNames();
        // Delivers what is due now (MainThreadQueue::drain does the same in a frame)
        static void deliver();
        // Transfers started and not finished by the backend
        static int runningTransfers();
    };

}

#endif // JASPERENGINE_HTTPBACKEND_H
