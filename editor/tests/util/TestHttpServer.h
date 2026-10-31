// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A small HTTP/1.1 server on 127.0.0.1 (an ephemeral port) for the HTTP client's tests
// (HttpTest.cpp): one thread per connection, "Connection: close" after each response.
//
// Routes:
//   /text                 "hello http" (text/plain)
//   /json                 {"name":"jasper","n":3,"list":[1,2],"nested":{"ok":true}}
//   /badjson              "{not json"
//   /status/{code}        that status, body "status {code}"
//   /redirect/{n}         302 to /redirect/{n-1}; /redirect/0 is 200 "redirected"
//   /redirect-code/{code}?to=PATH   that redirect status to PATH (absolute URL or path)
//   /slow/{ms}            waits, then 200 "slow"
//   /bytes/{n}            n bytes (byte i = i % 251) with Content-Length; honours Range: bytes=N-
//   /chunked/{n}          the same bytes, chunked
//   /trickle/{n}/{ms}     n bytes in 16 KiB chunks, `ms` between chunks (Content-Length)
//   /echo                 JSON {method, path, query, headers (lowercase), body_hex, body_size}
//   /gzip                 "gzip body " x 100, Content-Encoding: gzip
//   /png                  a 4x2 RGBA PNG
//   /cookies              two Set-Cookie headers
//   /count/{ms}           counts the requests running at once (maxConcurrent), waits, 200
//   /drop                 closes the connection without answering
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace jaspertest {

    class TestHttpServer {
    public:
        TestHttpServer();
        ~TestHttpServer();
        TestHttpServer(const TestHttpServer&) = delete;
        TestHttpServer& operator=(const TestHttpServer&) = delete;

        bool started() const { return listenSocket >= 0; }
        int port() const { return boundPort; }
        // "http://127.0.0.1:<port>" + path
        std::string url(const std::string& path) const;

        // requests seen per path (without the query)
        int requests(const std::string& path) const;
        int totalRequests() const;
        int maxConcurrent() const { return maxActive.load(); }
        void resetCounters();

        // the bytes /bytes/{n} sends
        static std::string pattern(size_t n, size_t offset = 0);

    private:
        int listenSocket = -1;
        int boundPort = 0;
        std::atomic<bool> stopping{false};
        std::thread acceptThread;
        std::mutex mutex;
        std::vector<std::thread> connections;
        mutable std::mutex countMutex;
        std::map<std::string, int> counts;
        std::atomic<int> active{0};
        std::atomic<int> maxActive{0};

        void acceptLoop();
        void serve(int socket);
    };

}
