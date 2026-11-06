// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <map>
#include <memory>
#include <string>

namespace jasperengine::editor {
    class CommandRunner;
}

namespace jasperengine::editor::htmlui {

    // One GET (phase-2 §3.13.1). error is empty on success (2xx); a failed transfer has status
    // 0 and curl's message, an HTTP error its status and "HTTP <status>".
    struct HttpResponse {
        int status = 0;
        std::string contentType;        // lowercase, without parameters ("text/css")
        std::string finalUrl;           // after redirects, as the caller named it (host rewrites undone)
        std::string error;
        std::filesystem::path bodyFile; // the body (also of an HTTP error); removed with the client

        bool ok() const { return error.empty() && status >= 200 && status < 300; }
    };

    // The editor's HTTP client: the curl executable (Windows 10 1803+, macOS, Linux; no TLS
    // stack in the editor) through CommandRunner, with a per-request config file (curl -K),
    // so no URL is ever quoted for a shell. Blocking: run it on a worker thread. One client
    // per thread.
    //
    // JASPERENGINE_IMPORT_MIRROR=<dir> (tests, phase 4): https://<host><path-and-query> is read
    // from <dir>/<host>/<urlquote(path-and-query, safe="")> (Python's urllib.parse.quote with
    // nothing safe) instead; a missing file fails with "not found". No network in that mode.
    class HttpClient {
    public:
        static constexpr const char* kUserAgent = "JasperEngineEditor/1.0";
        static constexpr int kDefaultTimeoutSeconds = 60;
        static constexpr long long kMaxFileSize = 64ll * 1024 * 1024;

        HttpClient();
        ~HttpClient();
        HttpClient(const HttpClient&) = delete;
        HttpClient& operator=(const HttpClient&) = delete;

        // `curl --version` succeeds (checked once per PATH value)
        static bool available();

        HttpResponse get(const std::string& url);
        // A large download: the body goes to `partial`, continuing what it holds with a Range
        // request. A failed transfer keeps the bytes it got for the next call (no error page is ever
        // appended to them); a server that ignores the Range fails with "HTTP server doesn't seem to
        // support byte ranges" and leaves the file alone. A stalled transfer fails after a minute.
        HttpResponse getResuming(const std::string& url, const std::filesystem::path& partial);

        // Tests: URL prefixes replaced before the request ("https://fonts.googleapis.com" ->
        // "http://127.0.0.1:PORT/gf"); responses report the URL as the caller named it
        void setHostRewrite(std::map<std::string, std::string> rewrites);
        const std::map<std::string, std::string>& getHostRewrite() const { return hostRewrite; }
        void setTimeoutSeconds(int seconds);
        // the largest body this client accepts (kMaxFileSize by default; a player runtime is larger)
        void setMaxFileSize(long long bytes);

        // From another thread, while get() runs: stops this client's request (it fails with
        // "cancelled"), and how many bytes of the body have arrived (a download's progress)
        void cancel();
        unsigned long long receivedBytes() const;

        // Process-wide defaults every client starts with (the fixture-server tests)
        static void setDefaultHostRewrite(std::map<std::string, std::string> rewrites);
        static std::map<std::string, std::string> getDefaultHostRewrite();
        static void setDefaultTimeoutSeconds(int seconds);

        // The mirror directory of JASPERENGINE_IMPORT_MIRROR, "" when not set
        static std::string mirrorDirectory();
        // "<host>/<quoted path-and-query>" of an http(s) URL, "" for anything else
        static std::string mirrorPath(const std::string& url);
        // Python's urllib.parse.quote(text, safe=""): everything but A-Z a-z 0-9 _ . - ~ as %XX
        static std::string urlQuote(const std::string& text);

        // Editor shutdown: stops every running request (their processes are terminated) and
        // fails every later one with "cancelled". Permanent for the process.
        static void cancelAll();
        static bool cancelled();

        // The whole body of a response; false when it cannot be read
        static bool readBody(const HttpResponse& response, std::string& body);

    private:
        long long maxFileSize = kMaxFileSize;
        HttpResponse getFromMirror(const std::string& mirror, const std::string& url);
        HttpResponse transfer(const std::string& url, const std::filesystem::path& body, bool resume);
        std::string rewrite(const std::string& url) const;
        std::string unrewrite(const std::string& url) const;
        std::filesystem::path newTempFile(const char* suffix);

        std::map<std::string, std::string> hostRewrite;
        int timeoutSeconds = kDefaultTimeoutSeconds;
        std::filesystem::path tempDir;
        unsigned counter = 0;
        std::unique_ptr<CommandRunner> runner;
        mutable std::mutex bodyMutex;
        std::filesystem::path currentBody;
        std::atomic<bool> cancelRequested{false};
    };

}
