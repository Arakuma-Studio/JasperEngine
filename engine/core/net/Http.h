// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_HTTP_H
#define JASPERENGINE_HTTP_H

#include "Export.h"
#include "script/AsyncOwner.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine {

    class TextureData;
    struct HttpJson;

    // The HTTP(S) client of the engine (Lua: the `Http` class). Every platform uses its own
    // stack (Linux: the system libcurl, Windows: WinHTTP, Apple: NSURLSession, Android:
    // HttpURLConnection, Web: fetch), so TLS, certificates, proxies and IPv6 come from the OS.
    // Requests run in the background; every callback runs on the main thread, in
    // Engine::runFrame right before Engine::onUpdate (MainThreadQueue). Errors are values:
    // the completion callback runs exactly once per request with a response whose `ok` says
    // how it went (unless the request's owner went first: then nothing runs).
    // The guide: docs/mcp/guide/networking.md.

    enum class HttpResponseType {
        Text,       // body in memory
        Binary,     // body in memory (the same as Text in C++)
        Json,       // body in memory, parsed on the worker (HttpResponse::json, "decode" on failure)
        Texture,    // decoded on the worker into HttpResponse::texture ("decode" on failure)
        File        // streamed into filePath (written to <filePath>.part, renamed on success)
    };

    enum class HttpState {
        Queued,     // waiting for a free slot (Http.maxConcurrentRequests)
        Running,
        Done,       // the response is there and the callback ran (or runs now)
        Cancelled
    };

    struct HttpRequestDesc {
        std::string method = "GET";     // any token; uppercase for the standard ones
        std::string url;                // http:// or https://
        std::vector<std::pair<std::string, std::string>> headers;   // order kept, duplicates allowed
        std::vector<std::pair<std::string, std::string>> query;     // appended, percent-encoded (RFC 3986 unreserved kept)
        std::vector<uint8_t> body;      // empty = no body
        double timeout = 30.0;          // s, the whole request (every redirect hop); <= 0: none
        double connectTimeout = 15.0;   // s
        int maxRedirects = 10;          // 0: do not follow (a 3xx is the response)
        bool verifyTls = true;
        HttpResponseType responseType = HttpResponseType::Binary;
        // File: an engine path ("data://ads/x.mp4", a bare relative path is data://) or an
        // absolute path inside System::getUserDataPath(); missing folders are made
        std::string filePath;
        size_t maxBodySize = 64u << 20; // in-memory bodies (not File); past it: "too_large"; 0: no limit
        bool reportProgress = false;    // call onProgress (implied when one is given)
        // File: continue an existing <filePath>.part (sends Range: bytes=<size>-; a 206 appends,
        // a 200 starts over, a 416 removes the .part and fails); bytes counts include the part
        bool resume = false;
        // File: keep <filePath>.part when the download fails or is cancelled (to resume later)
        bool keepPartial = false;
    };

    struct JASPERENGINE_API HttpResponse {
        bool ok = false;                // a response with status 200-299 that was read whole
        int status = 0;                 // 0: no HTTP response
        std::string errorCode;          // "" when ok: http, timeout, dns, connect, tls, network,
                                        // cancelled, offline, insecure_http, too_many_redirects,
                                        // redirect_blocked, invalid_url, file, decode, too_large, unsupported
        std::string error;              // "" when ok: what failed and the URL ("timeout after 10 s: https://x/y")
        std::string method;             // of the request
        std::vector<std::pair<std::string, std::string>> headers;   // names lowercased, in order
        std::string url;                // final URL after redirects
        bool redirected = false;
        std::vector<uint8_t> body;      // in-memory types; also the error body of a failed File/Texture request
        std::string filePath;           // File: the path as given
        std::shared_ptr<TextureData> texture;   // Texture: owns its pixels until takeTexture()
        std::shared_ptr<const HttpJson> json;   // Json: the parsed body (net/HttpJson.h)
        double elapsed = 0.0;           // s, start to completion
        uint64_t bytesReceived = 0;     // body bytes (with the resumed part of a File)

        // The first header of that name (any case), or nullptr
        const std::string* header(const std::string& name) const;
        // The decoded texture with the engine's TextureData rule (it does not own its pixels:
        // the texture pool that gets it does, see Image::setTexture(id, TextureData)); the
        // response no longer frees them. An empty TextureData when there is none.
        TextureData takeTexture() const;
    };

    class JASPERENGINE_API HttpRequestHandle {
    public:
        uint64_t id() const { return m_id; }
        HttpState state() const { return m_state; }
        // Done or Cancelled (the coroutines' awaitable protocol: Coroutine.waitFor)
        bool isDone() const { return m_state == HttpState::Done || m_state == HttpState::Cancelled; }
        uint64_t bytesReceived() const { return m_received; }
        int64_t bytesTotal() const { return m_total; }     // -1 unknown
        float progress() const;                            // 0..1, -1 unknown
        // Idempotent. A queued or running request ends as cancelled (its callback runs with
        // errorCode "cancelled" in the next frames); a request whose response already arrived
        // (even if its callback has not run yet) or that is done is not changed.
        void cancel();
        // null until Done/Cancelled
        const HttpResponse* response() const { return m_response.get(); }
        std::shared_ptr<HttpResponse> sharedResponse() const { return m_response; }
        const std::string& url() const { return m_url; }   // as requested, the query appended
        const std::string& method() const { return m_method; }

        HttpRequestHandle(uint64_t id, std::string method, std::string url);

    private:
        friend class Http;
        friend struct HttpCore;
        uint64_t m_id = 0;
        std::string m_method;
        std::string m_url;
        // main thread only (updated at the delivery point)
        HttpState m_state = HttpState::Queued;
        uint64_t m_received = 0;
        int64_t m_total = -1;
        std::shared_ptr<HttpResponse> m_response;
    };

    using HttpCallback = std::function<void(const HttpResponse&)>;
    using HttpProgress = std::function<void(uint64_t received, int64_t total)>;

    class JASPERENGINE_API Http {
    public:
        // Main thread. Starts the request (or queues it behind maxConcurrentRequests); the
        // callbacks run on the main thread at the delivery point, never inside this call.
        // With an owner, the request ends silently with it (AsyncOwners).
        static std::shared_ptr<HttpRequestHandle> send(HttpRequestDesc desc, HttpCallback onComplete,
                                                       HttpProgress onProgress = {}, AsyncOwner owner = {});

        // False only when the platform's stack cannot work (Linux without libcurl); requests
        // then fail with "unsupported" and the reason
        static bool isAvailable();
        static std::string unavailableReason();

        // Every request ends as cancelled; their callbacks run with "cancelled"
        static void cancelAll();
        // Play stop: every request ends without a callback; the stats and the settings scripts
        // change (maxConcurrentRequests, userAgent, timeout, logFailures) reset. Never waits for
        // a transfer.
        static void shutdown();
        // Engine shutdown: shutdown() and the platform stack's worker threads stopped (joined,
        // so none outlives the engine); a later request starts them again
        static void terminate();

        // requests running at once (1..64, default 8); more wait in order (HttpState.Queued)
        static int getMaxConcurrentRequests();
        static void setMaxConcurrentRequests(int n);
        // the User-Agent header: "JasperEngine/<version> (<platform>)" until set; "" restores it;
        // the browser's own on the web
        static std::string getUserAgent();
        static void setUserAgent(const std::string& userAgent);
        // the total timeout in seconds of the Lua requests that set none (30)
        static double getDefaultTimeout();
        static void setDefaultTimeout(double seconds);
        // each failed request logs a warning (default off: games retry and probe on purpose)
        static bool getLogFailures();
        static void setLogFailures(bool log);

        // Editor Game view: Network > Offline (simulated). Requests to hosts other than
        // localhost fail with "offline" (running ones too); never saved into builds.
        static bool isOfflineSimulated();
        static void setOfflineSimulated(bool offline);
        // Project setting player.allow_insecure_http: plain http:// to hosts other than
        // localhost / 127.0.0.1 / ::1 fails with "insecure_http" when false (the default)
        static bool isInsecureHttpAllowed();
        static void setInsecureHttpAllowed(bool allowed);

        struct Stats {
            uint32_t active = 0;        // running
            uint32_t queued = 0;
            uint32_t finished = 0;      // delivered ok
            uint32_t failed = 0;        // delivered not ok (cancelled ones not counted)
            uint32_t cancelled = 0;
            std::vector<HttpResponse> lastFailures;   // last 5, newest last, bodies dropped
        };
        static Stats getStats();

        // Percent-encoding (RFC 3986 unreserved characters kept); decodeUrl turns '+' into ' '
        static std::string encodeUrl(const std::string& text);
        static std::string decodeUrl(const std::string& text);
    };

}

#endif // JASPERENGINE_HTTP_H
