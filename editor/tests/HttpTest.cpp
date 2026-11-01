// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The HTTP client (feature http, docs/superpowers/specs/2026-10-31-http-design.md section 9):
// the C++ API, the Lua API and the Linux backend (the system libcurl) against an in-process
// HTTP server on 127.0.0.1 (util/TestHttpServer) and, for TLS, a Python HTTPS server with a
// self-signed certificate. No test needs the internet.

#include "ScriptDataTestSupport.h"
#include "util/TestHttpServer.h"

#include "Engine.h"
#include "Scene.h"
#include "component/Transform.h"
#include "manager/ObjectManager.h"
#include "net/Http.h"
#include "net/HttpBackend.h"
#include "net/HttpJson.h"
#include "script/AsyncOwner.h"
#include "texture/TextureData.h"
#include "thread/MainThreadQueue.h"
#include "log/Logger.h"

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
#include <vector>

using namespace jasperengine;
using jaspertest::TestHttpServer;

namespace {

    namespace fs = std::filesystem;

    class HttpTestSystem : public scriptdata::TestSystem {
    public:
        std::mutex mutex;
        std::vector<std::string> warnings;

        void platformLog(const int type, const char* fmt, va_list args) override {
            char text[4096];
            va_list copy;
            va_copy(copy, args);
            std::vsnprintf(text, sizeof(text), fmt, copy);
            va_end(copy);
            if (type == D_LOG_WARN || type == D_LOG_ERROR) {
                std::lock_guard<std::mutex> lock(mutex);
                warnings.push_back(text);
            }
        }

        int count(const std::string& needle) {
            ::jasperengine::Logger::flush();   // the engine's logger delivers on its own thread
            std::lock_guard<std::mutex> lock(mutex);
            int n = 0;
            for (const std::string& w : warnings) if (w.find(needle) != std::string::npos) n++;
            return n;
        }
    };

    struct Result {
        int calls = 0;
        HttpResponse response;
        std::vector<std::pair<uint64_t, int64_t>> progress;
    };

    class HttpTest : public ::testing::Test {
    protected:
        HttpTestSystem system;
        System* previous = nullptr;
        fs::path root;
        lua_State* L = nullptr;
        std::unique_ptr<TestHttpServer> server;

        void SetUp() override {
            L = scriptdata::engineLua();
            ASSERT_NE(L, nullptr);
            const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
            root = fs::temp_directory_path() / ("jasper-http-" + std::to_string(::getpid()) + "-" + info->name());
            fs::remove_all(root);
            fs::create_directories(root / "data");
            fs::create_directories(root / "assets");
            fs::create_directories(root / "lua");
            system.root = root.string();
            previous = SystemTestAccess::swap(&system);
            server = std::make_unique<TestHttpServer>();
            ASSERT_TRUE(server->started());
            reset();
        }

        void TearDown() override {
            reset();
            // the transfers of this test end before the next one starts
            waitFor([] { return HttpTesting::runningTransfers() == 0; }, 5.0);
            server.reset();
            SystemTestAccess::swap(previous);
            lua_settop(L, 0);
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        void reset() {
            Http::shutdown();
            MainThreadQueue::clear();
            Http::setMaxConcurrentRequests(8);
            Http::setInsecureHttpAllowed(false);
            Http::setOfflineSimulated(false);
            Http::setLogFailures(false);
            Http::setUserAgent("");
            Http::setDefaultTimeout(30.0);
        }

        // runs frames' worth of deliveries until `done` (false on timeout)
        bool waitFor(const std::function<bool()>& done, double seconds = 10.0) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (std::chrono::steady_clock::now() < end) {
                MainThreadQueue::drain();
                if (done()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        }

        std::shared_ptr<Result> start(HttpRequestDesc desc, std::shared_ptr<HttpRequestHandle>* handle = nullptr, AsyncOwner owner = {}) {
            auto result = std::make_shared<Result>();
            auto h = Http::send(std::move(desc), [result](const HttpResponse& r) {
                result->calls++;
                result->response = r;
            }, [result](uint64_t received, int64_t total) {
                result->progress.emplace_back(received, total);
            }, owner);
            if (handle) *handle = h;
            return result;
        }

        HttpResponse run(HttpRequestDesc desc) {
            auto result = start(std::move(desc));
            EXPECT_TRUE(waitFor([&] { return result->calls > 0; })) << "no response";
            return result->response;
        }

        HttpRequestDesc get(const std::string& path) {
            HttpRequestDesc desc;
            desc.url = server->url(path);
            return desc;
        }

        static std::string text(const std::vector<uint8_t>& body) {
            return std::string(body.begin(), body.end());
        }

        static nlohmann::json json(const HttpResponse& r) {
            return nlohmann::json::parse(text(r.body));
        }

        // Lua: runs `code`; "" or the error
        std::string lua(const std::string& code) {
            const int top = lua_gettop(L);
            std::string error;
            if (luaL_loadbuffer(L, code.data(), code.size(), "=httptest") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
                const char* message = lua_tostring(L, -1);
                error = message ? message : "(no message)";
            }
            lua_settop(L, top);
            return error;
        }

        std::string luaValue(const std::string& expression) {
            const std::string code = "return tostring(" + expression + ")";
            const int top = lua_gettop(L);
            std::string out;
            if (luaL_loadbuffer(L, code.data(), code.size(), "=httptest") != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
                out = std::string("ERROR: ") + lua_tostring(L, -1);
            } else {
                out = lua_tostring(L, -1);
            }
            lua_settop(L, top);
            return out;
        }

        bool luaTrue(const std::string& expression) {
            return luaValue(expression) == "true";
        }
    };

}

// 1
TEST_F(HttpTest, GetTextHasStatusBodyHeadersAndFinalUrl) {
    const HttpResponse r = run(get("/text"));
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.errorCode, "");
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(text(r.body), "hello http");
    EXPECT_EQ(r.url, server->url("/text"));
    EXPECT_FALSE(r.redirected);
    EXPECT_GT(r.elapsed, 0.0);
    EXPECT_EQ(r.bytesReceived, 10u);
    ASSERT_NE(r.header("Content-Type"), nullptr);
    EXPECT_EQ(*r.header("CONTENT-TYPE"), "text/plain; charset=utf-8");
    int xtest = 0;
    for (const auto& [name, value] : r.headers) {
        EXPECT_EQ(name, HttpUtil::lower(name));
        if (name == "x-test") xtest++;
    }
    EXPECT_EQ(xtest, 2);
    EXPECT_EQ(r.method, "GET");
}

TEST_F(HttpTest, DefaultUserAgentAndCustomHeaders) {
    HttpRequestDesc desc = get("/echo");
    desc.headers = {{"X-One", "1"}, {"X-Two", "two words"}};
    nlohmann::json echo = json(run(desc));
    EXPECT_EQ(echo["headers"]["x-one"], "1");
    EXPECT_EQ(echo["headers"]["x-two"], "two words");
    EXPECT_EQ(echo["headers"]["user-agent"].get<std::string>().rfind("JasperEngine", 0), 0u);
    EXPECT_NE(echo["headers"]["user-agent"].get<std::string>().find("(Linux)"), std::string::npos);

    Http::setUserAgent("MyGame/2.0");
    echo = json(run(get("/echo")));
    EXPECT_EQ(echo["headers"]["user-agent"], "MyGame/2.0");
    // a request's own User-Agent wins
    desc = get("/echo");
    desc.headers = {{"User-Agent", "Mine"}};
    echo = json(run(desc));
    EXPECT_EQ(echo["headers"]["user-agent"], "Mine");
}

// 2
TEST_F(HttpTest, PostBodiesKeepTheirBytes) {
    HttpRequestDesc desc = get("/echo");
    desc.method = "post";
    desc.body = {'a', '\0', 'b', 0xff};
    nlohmann::json echo = json(run(desc));
    EXPECT_EQ(echo["method"], "POST");
    EXPECT_EQ(echo["body_hex"], "610062ff");
    // one default type for a raw body on every platform (libcurl would send a form one,
    // Android and Apple too)
    EXPECT_EQ(echo["headers"]["content-type"], "application/octet-stream");
    EXPECT_FALSE(echo["headers"].contains("expect"));

    for (const char* method : {"PUT", "PATCH", "DELETE"}) {
        desc = get("/echo");
        desc.method = method;
        desc.body = {'x', 'y'};
        echo = json(run(desc));
        EXPECT_EQ(echo["method"], method);
        EXPECT_EQ(echo["body_hex"], "7879");
    }
    // an empty PUT still says Content-Length: 0
    desc = get("/echo");
    desc.method = "PUT";
    echo = json(run(desc));
    EXPECT_EQ(echo["headers"]["content-length"], "0");
}

// 3
TEST_F(HttpTest, QueryIsEncodedAndAppended) {
    HttpRequestDesc desc = get("/echo");
    desc.query = {{"q", "a b&c"}, {"x", "~ok-._"}};
    nlohmann::json echo = json(run(desc));
    EXPECT_EQ(echo["query"], "q=a%20b%26c&x=~ok-._");

    desc = get("/echo?first=1");
    desc.query = {{"second", "2"}};
    echo = json(run(desc));
    EXPECT_EQ(echo["query"], "first=1&second=2");

    EXPECT_EQ(Http::encodeUrl("a b&c/é"), "a%20b%26c%2F%C3%A9");
    EXPECT_EQ(Http::decodeUrl("a%20b+c%2F%C3%A9%zz"), "a b c/é%zz");
}

// 4
TEST_F(HttpTest, NonSuccessStatusIsAnErrorValueWithItsBody) {
    const HttpResponse r = run(get("/status/404"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.status, 404);
    EXPECT_EQ(r.errorCode, "http");
    EXPECT_EQ(r.error, "HTTP 404 Not Found: " + server->url("/status/404"));
    EXPECT_EQ(text(r.body), "status 404");

    const HttpResponse ok = run(get("/status/204"));
    EXPECT_TRUE(ok.ok);
    EXPECT_EQ(ok.status, 204);
}

// 5
TEST_F(HttpTest, TimeoutEndsTheRequestInTime) {
    HttpRequestDesc desc = get("/slow/2000");
    desc.timeout = 0.3;
    const auto begin = std::chrono::steady_clock::now();
    const HttpResponse r = run(desc);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorCode, "timeout");
    EXPECT_EQ(r.status, 0);
    EXPECT_LT(took, 1.0);
    EXPECT_EQ(r.error, "timeout after 0.3 s: " + server->url("/slow/2000"));
}

TEST_F(HttpTest, ConnectionErrorsAreValues) {
    // a port nothing listens on
    int port = 0;
    {
        const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        socklen_t length = sizeof(address);
        ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
        port = ntohs(address.sin_port);
        ::close(probe);
    }
    HttpRequestDesc desc;
    desc.url = "http://127.0.0.1:" + std::to_string(port) + "/x";
    HttpResponse r = run(desc);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.errorCode == "connect" || r.errorCode == "network") << r.errorCode;

    // the server hangs up without a response
    r = run(get("/drop"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorCode, "network");
    EXPECT_EQ(r.status, 0);
}

// 6
TEST_F(HttpTest, RedirectsFollowCountAndStop) {
    HttpResponse r = run(get("/redirect/3"));
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.redirected);
    EXPECT_EQ(r.url, server->url("/redirect/0"));
    EXPECT_EQ(text(r.body), "redirected");

    HttpRequestDesc desc = get("/redirect/3");
    desc.maxRedirects = 2;
    r = run(desc);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorCode, "too_many_redirects");

    desc = get("/redirect/3");
    desc.maxRedirects = 0;
    r = run(desc);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.status, 302);
    EXPECT_EQ(r.errorCode, "http");
    EXPECT_FALSE(r.redirected);
    ASSERT_NE(r.header("location"), nullptr);
}

TEST_F(HttpTest, RedirectsChangeMethodsLikeBrowsers) {
    auto post = [&](int code) {
        HttpRequestDesc desc = get("/redirect-code/" + std::to_string(code) + "?to=/echo");
        desc.method = "POST";
        desc.body = {'h', 'i'};
        desc.headers = {{"Content-Type", "text/plain"}};
        return json(run(desc));
    };
    for (int code : {301, 302, 303}) {
        nlohmann::json echo = post(code);
        EXPECT_EQ(echo["method"], "GET") << code;
        EXPECT_EQ(echo["body_size"], 0) << code;
        EXPECT_FALSE(echo["headers"].contains("content-type")) << code;
    }
    for (int code : {307, 308}) {
        nlohmann::json echo = post(code);
        EXPECT_EQ(echo["method"], "POST") << code;
        EXPECT_EQ(echo["body_hex"], "6869") << code;
        EXPECT_EQ(echo["headers"]["content-type"], "text/plain") << code;
    }
    // 303 turns a PUT into a GET too; 302 keeps it
    HttpRequestDesc put = get("/redirect-code/303?to=/echo");
    put.method = "PUT";
    put.body = {'x'};
    EXPECT_EQ(json(run(put))["method"], "GET");
    put = get("/redirect-code/302?to=/echo");
    put.method = "PUT";
    put.body = {'x'};
    EXPECT_EQ(json(run(put))["method"], "PUT");

    // another origin (localhost vs 127.0.0.1) does not get the credentials
    HttpRequestDesc desc = get("/redirect-code/302?to=" + Http::encodeUrl("http://localhost:" + std::to_string(server->port()) + "/echo"));
    desc.headers = {{"Authorization", "Bearer secret"}, {"X-Keep", "1"}};
    const HttpResponse r = run(desc);
    nlohmann::json echo = json(r);
    EXPECT_FALSE(echo["headers"].contains("authorization"));
    EXPECT_EQ(echo["headers"]["x-keep"], "1");
    EXPECT_EQ(r.url, "http://localhost:" + std::to_string(server->port()) + "/echo");
    // the same origin keeps them
    desc = get("/redirect-code/302?to=/echo");
    desc.headers = {{"Authorization", "Bearer secret"}};
    EXPECT_EQ(json(run(desc))["headers"]["authorization"], "Bearer secret");
}

// 7
TEST_F(HttpTest, CancelQueuedRunningAndDone) {
    Http::setMaxConcurrentRequests(1);
    std::shared_ptr<HttpRequestHandle> first, second;
    auto running = start(get("/slow/3000"), &first);
    auto queued = start(get("/count/0"), &second);
    EXPECT_EQ(first->state(), HttpState::Running);
    EXPECT_EQ(second->state(), HttpState::Queued);

    second->cancel();
    second->cancel();
    EXPECT_EQ(second->state(), HttpState::Cancelled);
    EXPECT_TRUE(second->isDone());
    ASSERT_NE(second->response(), nullptr);
    EXPECT_EQ(second->response()->errorCode, "cancelled");
    EXPECT_EQ(queued->calls, 0) << "a callback never runs inside cancel()";
    ASSERT_TRUE(waitFor([&] { return queued->calls > 0; }));
    EXPECT_EQ(queued->response.errorCode, "cancelled");

    // the server saw the slow one start
    ASSERT_TRUE(waitFor([&] { return server->requests("/slow/3000") == 1; }));
    const auto begin = std::chrono::steady_clock::now();
    first->cancel();
    EXPECT_EQ(first->state(), HttpState::Cancelled);
    ASSERT_TRUE(waitFor([&] { return running->calls > 0; }, 2.0));
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count(), 1.0);
    EXPECT_EQ(running->response.errorCode, "cancelled");
    EXPECT_FALSE(running->response.ok);

    std::shared_ptr<HttpRequestHandle> third;
    auto done = start(get("/text"), &third);
    ASSERT_TRUE(waitFor([&] { return done->calls > 0; }));
    third->cancel();
    EXPECT_EQ(third->state(), HttpState::Done);
    waitFor([] { return false; }, 0.2);
    EXPECT_EQ(done->calls, 1);
    EXPECT_EQ(running->calls, 1);
    EXPECT_EQ(queued->calls, 1);
    EXPECT_EQ(server->requests("/count/0"), 0) << "a cancelled queued request never starts";
}

TEST_F(HttpTest, CancelAllRunsTheCallbacksWithCancelled) {
    auto a = start(get("/slow/3000"));
    auto b = start(get("/slow/3000"));
    Http::cancelAll();
    ASSERT_TRUE(waitFor([&] { return a->calls == 1 && b->calls == 1; }, 3.0));
    EXPECT_EQ(a->response.errorCode, "cancelled");
    EXPECT_EQ(b->response.errorCode, "cancelled");
    EXPECT_EQ(Http::getStats().cancelled, 2u);
    EXPECT_EQ(Http::getStats().failed, 0u);
}

// 8
TEST_F(HttpTest, ConcurrencyLimitQueuesInOrder) {
    Http::setMaxConcurrentRequests(4);
    std::vector<std::shared_ptr<Result>> results;
    std::vector<std::shared_ptr<HttpRequestHandle>> handles(20);
    for (int i = 0; i < 20; i++) results.push_back(start(get("/count/100"), &handles[i]));
    EXPECT_EQ(Http::getStats().queued, 16u);
    EXPECT_EQ(handles[4]->state(), HttpState::Queued);
    ASSERT_TRUE(waitFor([&] {
        for (auto& r : results) if (r->calls == 0) return false;
        return true;
    }, 20.0));
    EXPECT_LE(server->maxConcurrent(), 4);
    EXPECT_GE(server->maxConcurrent(), 2);
    for (auto& r : results) EXPECT_TRUE(r->response.ok);
    // ids increase
    for (int i = 1; i < 20; i++) EXPECT_GT(handles[i]->id(), handles[i - 1]->id());
    EXPECT_EQ(Http::getMaxConcurrentRequests(), 4);
    Http::setMaxConcurrentRequests(1000);
    EXPECT_EQ(Http::getMaxConcurrentRequests(), 64);
    Http::setMaxConcurrentRequests(0);
    EXPECT_EQ(Http::getMaxConcurrentRequests(), 1);
}

// 9
TEST_F(HttpTest, DownloadToAFileWithPartAndRename) {
    HttpRequestDesc desc = get("/bytes/1048576");
    desc.responseType = HttpResponseType::File;
    desc.filePath = "data://t/x.bin";
    const HttpResponse r = run(desc);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.filePath, "data://t/x.bin");
    EXPECT_TRUE(r.body.empty());
    EXPECT_EQ(r.bytesReceived, 1048576u);
    const fs::path file = root / "data" / "t" / "x.bin";
    EXPECT_EQ(scriptdata::readFile(file), TestHttpServer::pattern(1048576));
    EXPECT_FALSE(fs::exists(root / "data" / "t" / "x.bin.part"));

    // a failure keeps the file there
    desc = get("/status/500");
    desc.responseType = HttpResponseType::File;
    desc.filePath = "data://t/x.bin";
    const HttpResponse failed = run(desc);
    EXPECT_EQ(failed.errorCode, "http");
    EXPECT_EQ(text(failed.body), "status 500") << "the error body is kept in memory";
    EXPECT_EQ(scriptdata::readFile(file), TestHttpServer::pattern(1048576));
    EXPECT_FALSE(fs::exists(root / "data" / "t" / "x.bin.part"));

    // a chunked body, an absolute path inside the user data folder
    desc = get("/chunked/100000");
    desc.responseType = HttpResponseType::File;
    desc.filePath = (root / "data" / "abs.bin").string();
    ASSERT_TRUE(run(desc).ok);
    EXPECT_EQ(scriptdata::readFile(root / "data" / "abs.bin"), TestHttpServer::pattern(100000));

    // paths outside the user data
    for (const std::string& bad : {std::string("asset://x.bin"), std::string("/tmp/x.bin"), std::string("data://../x.bin"), std::string("")}) {
        desc = get("/bytes/10");
        desc.responseType = HttpResponseType::File;
        desc.filePath = bad;
        const HttpResponse refused = run(desc);
        EXPECT_EQ(refused.errorCode, "file") << bad;
    }
    EXPECT_EQ(server->requests("/bytes/10"), 0);
}

TEST_F(HttpTest, CancelledDownloadLeavesNoFile) {
    HttpRequestDesc desc = get("/trickle/2000000/20");
    desc.responseType = HttpResponseType::File;
    desc.filePath = "data://c/y.bin";
    std::shared_ptr<HttpRequestHandle> handle;
    auto result = start(desc, &handle);
    ASSERT_TRUE(waitFor([&] { return handle->bytesReceived() > 0; }));
    handle->cancel();
    ASSERT_TRUE(waitFor([&] { return result->calls > 0; }));
    EXPECT_EQ(result->response.errorCode, "cancelled");
    waitFor([] { return HttpTesting::runningTransfers() == 0; }, 3.0);
    EXPECT_FALSE(fs::exists(root / "data" / "c" / "y.bin"));
    EXPECT_FALSE(fs::exists(root / "data" / "c" / "y.bin.part"));
}

TEST_F(HttpTest, DownloadResumesAndKeepsPartials) {
    const fs::path part = root / "data" / "r" / "z.bin.part";
    const fs::path meta = root / "data" / "r" / "z.bin.part.meta";
    scriptdata::writeFile(part, TestHttpServer::pattern(1000));
    scriptdata::writeFile(meta, "\"v1\"\n");
    HttpRequestDesc desc = get("/bytes/300000");
    desc.responseType = HttpResponseType::File;
    desc.filePath = "data://r/z.bin";
    desc.resume = true;
    std::shared_ptr<HttpRequestHandle> handle;
    auto result = start(desc, &handle);
    ASSERT_TRUE(waitFor([&] { return result->calls > 0; }));
    ASSERT_TRUE(result->response.ok) << result->response.error;
    EXPECT_EQ(result->response.status, 206);
    EXPECT_EQ(server->lastHeader("/bytes/300000", "range"), "bytes=1000-");
    EXPECT_EQ(server->lastHeader("/bytes/300000", "if-range"), "\"v1\"");
    EXPECT_EQ(server->lastHeader("/bytes/300000", "accept-encoding"), "identity");
    EXPECT_EQ(result->response.bytesReceived, 300000u);
    EXPECT_EQ(handle->bytesTotal(), 300000);
    EXPECT_EQ(scriptdata::readFile(root / "data" / "r" / "z.bin"), TestHttpServer::pattern(300000));
    EXPECT_FALSE(fs::exists(part));
    EXPECT_FALSE(fs::exists(meta));

    // the file changed on the server (another ETag): a 200, written from the start
    scriptdata::writeFile(part, std::string(1000, 'x'));
    scriptdata::writeFile(meta, "\"old\"\n");
    desc.url = server->url("/bytes/300000?etag=v2");
    HttpResponse changed = run(desc);
    ASSERT_TRUE(changed.ok) << changed.error;
    EXPECT_EQ(changed.status, 200);
    EXPECT_EQ(scriptdata::readFile(root / "data" / "r" / "z.bin"), TestHttpServer::pattern(300000));

    // a part without a validator is not continued
    scriptdata::writeFile(part, std::string(1000, 'x'));
    desc.url = server->url("/bytes/300000");
    changed = run(desc);
    ASSERT_TRUE(changed.ok);
    EXPECT_EQ(changed.status, 200);
    EXPECT_EQ(server->lastHeader("/bytes/300000", "range"), "");
    EXPECT_EQ(scriptdata::readFile(root / "data" / "r" / "z.bin"), TestHttpServer::pattern(300000));

    // a part longer than the file: 416, the part goes
    scriptdata::writeFile(part, TestHttpServer::pattern(500000));
    scriptdata::writeFile(meta, "\"v1\"\n");
    const HttpResponse r416 = run(desc);
    EXPECT_EQ(r416.status, 416);
    EXPECT_FALSE(fs::exists(part));
    EXPECT_FALSE(fs::exists(meta));

    // keepPartial: a cancelled download leaves its part, with the validator a resume sends
    desc = get("/trickle/2000000/20");
    desc.responseType = HttpResponseType::File;
    desc.filePath = "data://r/k.bin";
    desc.keepPartial = true;
    result = start(desc, &handle);
    ASSERT_TRUE(waitFor([&] { return handle->bytesReceived() > 0; }));
    handle->cancel();
    ASSERT_TRUE(waitFor([&] { return result->calls > 0; }));
    waitFor([] { return HttpTesting::runningTransfers() == 0; }, 3.0);
    EXPECT_TRUE(fs::exists(root / "data" / "r" / "k.bin.part"));
    EXPECT_GT(fs::file_size(root / "data" / "r" / "k.bin.part"), 0u);
    EXPECT_EQ(scriptdata::readFile(root / "data" / "r" / "k.bin.part.meta"), "\"t1\"\n");
    EXPECT_FALSE(fs::exists(root / "data" / "r" / "k.bin"));
}

TEST_F(HttpTest, OneDownloadPerFile) {
    HttpRequestDesc desc = get("/trickle/400000/10");
    desc.responseType = HttpResponseType::File;
    desc.filePath = "data://same/x.bin";
    std::shared_ptr<HttpRequestHandle> first;
    auto a = start(desc, &first);
    desc.filePath = (root / "data" / "same" / "x.bin").string();   // the same file by another name
    auto b = start(desc);
    ASSERT_TRUE(waitFor([&] { return a->calls == 1 && b->calls == 1; }, 10.0));
    EXPECT_TRUE(a->response.ok) << a->response.error;
    EXPECT_EQ(b->response.errorCode, "file");
    EXPECT_NE(b->response.error.find("is being downloaded already (request " + std::to_string(first->id()) + ")"), std::string::npos)
        << b->response.error;
    EXPECT_EQ(scriptdata::readFile(root / "data" / "same" / "x.bin"), TestHttpServer::pattern(400000));
    // done: the path is free again
    desc.filePath = "data://same/x.bin";
    EXPECT_TRUE(run(desc).ok);
}

TEST_F(HttpTest, PrivateNetworkRedirectsAndHosts) {
    for (const char* host : {"127.0.0.1", "10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.0.10", "169.254.1.1",
                             "100.64.0.1", "0.0.0.0", "::1", "[::1]", "fd00::1", "fe80::1", "::ffff:192.168.1.1",
                             "localhost", "printer.localhost"}) {
        EXPECT_TRUE(HttpUtil::isPrivateHost(host)) << host;
    }
    for (const char* host : {"8.8.8.8", "172.32.0.1", "192.169.0.1", "example.com", "2001:db8::1", "::ffff:8.8.8.8",
                             "100.128.0.1", "1.2.3.4.5", "10.0.0"}) {
        EXPECT_FALSE(HttpUtil::isPrivateHost(host)) << host;
    }

    auto transfer = [](const std::string& url) {
        HttpRequestDesc desc;
        desc.url = url;
        return std::make_unique<HttpTransfer>(999999, desc, "", true, "", "");
    };
    // a public host may not send the request into the local network
    auto t = transfer("https://public.example.com/a");
    EXPECT_EQ(t->redirect(302, "http://192.168.1.1/admin"), HttpRedirect::Action::Fail);
    t = transfer("https://public.example.com/a");
    EXPECT_EQ(t->redirect(302, "http://localhost:8080/"), HttpRedirect::Action::Fail);
    t = transfer("https://public.example.com/a");
    EXPECT_EQ(t->redirect(302, "https://cdn.example.com/b"), HttpRedirect::Action::Follow);
    // a local host may
    t = transfer("http://127.0.0.1:1234/a");
    EXPECT_EQ(t->redirect(302, "http://10.0.0.2/b"), HttpRedirect::Action::Follow);
    // a stack that followed the redirect itself (the browser) reports where it ended
    t = transfer("https://public.example.com/a");
    t->stackRedirected();
    EXPECT_FALSE(t->begin(200, {}, "http://192.168.1.1/"));
    t = transfer("https://public.example.com/a");
    EXPECT_TRUE(t->begin(200, {}, "https://other.example.com/"));

    // end to end: the redirect fails with redirect_blocked (from a public-looking URL the test
    // cannot serve, so through the transfer above) and a local one is followed
    EXPECT_TRUE(run(get("/redirect-code/302?to=" + Http::encodeUrl("http://localhost:" + std::to_string(server->port()) + "/text"))).ok);
}

TEST_F(HttpTest, ShutdownResetsWhatScriptsSet) {
    Http::setMaxConcurrentRequests(3);
    Http::setUserAgent("Game/1");
    Http::setDefaultTimeout(5);
    Http::setLogFailures(true);
    Http::setOfflineSimulated(true);
    Http::shutdown();
    EXPECT_EQ(Http::getMaxConcurrentRequests(), 8);
    EXPECT_EQ(Http::getUserAgent().rfind("JasperEngine", 0), 0u);
    EXPECT_EQ(Http::getDefaultTimeout(), 30.0);
    EXPECT_FALSE(Http::getLogFailures());
    EXPECT_TRUE(Http::isOfflineSimulated()) << "the editor's toggle survives Play stop";
    Http::setOfflineSimulated(false);
}

TEST_F(HttpTest, TerminateStopsTheWorkerAndTheNextRequestStartsIt) {
    EXPECT_TRUE(run(get("/text")).ok);
    Http::terminate();
    EXPECT_EQ(HttpTesting::runningTransfers(), 0);
    EXPECT_TRUE(run(get("/text")).ok);
}

// 10
TEST_F(HttpTest, TextureFromUrl) {
    HttpRequestDesc desc = get("/png");
    desc.responseType = HttpResponseType::Texture;
    HttpResponse r = run(desc);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_TRUE(r.texture);
    EXPECT_EQ(r.texture->getWidth(), 4);
    EXPECT_EQ(r.texture->getHeight(), 2);
    EXPECT_TRUE(r.body.empty());
    TextureData taken = r.takeTexture();
    EXPECT_EQ(taken.getWidth(), 4);
    EXPECT_FALSE(taken.getDataOwned());
    EXPECT_FALSE(r.texture->getDataOwned());
    taken.releaseImageData();

    desc = get("/text");
    desc.responseType = HttpResponseType::Texture;
    r = run(desc);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorCode, "decode");
    EXPECT_FALSE(r.texture);
    EXPECT_EQ(text(r.body), "hello http");
}

TEST_F(HttpTest, JsonResponsesParseOnTheWorker) {
    HttpRequestDesc desc = get("/json");
    desc.responseType = HttpResponseType::Json;
    HttpResponse r = run(desc);
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.json);
    EXPECT_EQ(r.json->value["name"], "jasper");

    desc = get("/badjson");
    desc.responseType = HttpResponseType::Json;
    r = run(desc);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorCode, "decode");
    EXPECT_NE(r.error.find("syntax error"), std::string::npos) << r.error;
}

TEST_F(HttpTest, BodyLimit) {
    HttpRequestDesc desc = get("/bytes/5000");
    desc.maxBodySize = 4096;
    HttpResponse r = run(desc);
    EXPECT_EQ(r.errorCode, "too_large");
    desc = get("/chunked/5000");
    desc.maxBodySize = 4096;
    r = run(desc);
    EXPECT_EQ(r.errorCode, "too_large");
    desc = get("/chunked/5000");
    desc.maxBodySize = 0;
    EXPECT_TRUE(run(desc).ok);
}

// 11
TEST_F(HttpTest, PlainHttpPolicy) {
    HttpRequestDesc desc;
    desc.url = "http://example.invalid/";
    HttpResponse r = run(desc);
    EXPECT_EQ(r.errorCode, "insecure_http");
    EXPECT_NE(r.error.find("player.allow_insecure_http"), std::string::npos);
    EXPECT_TRUE(run(get("/text")).ok) << "127.0.0.1 is always allowed";
    desc.url = "http://localhost:" + std::to_string(server->port()) + "/text";
    EXPECT_TRUE(run(desc).ok);

    Http::setInsecureHttpAllowed(true);
    desc.url = "http://example.invalid/";
    desc.timeout = 5;
    r = run(desc);
    EXPECT_NE(r.errorCode, "insecure_http");
    EXPECT_FALSE(r.ok);

    // a redirect to a plain http host is checked too
    Http::setInsecureHttpAllowed(false);
    r = run(get("/redirect-code/302?to=" + Http::encodeUrl("http://example.invalid/x")));
    EXPECT_EQ(r.errorCode, "insecure_http");
    EXPECT_NE(r.error.find("redirected to http://example.invalid/x"), std::string::npos) << r.error;
}

TEST_F(HttpTest, InvalidUrls) {
    for (const std::string& url : {std::string("ftp://x/"), std::string("https://"), std::string("https://a b/"),
                                   std::string("nothing"), std::string("https://host:99999/"), std::string("")}) {
        HttpRequestDesc desc;
        desc.url = url;
        EXPECT_EQ(run(desc).errorCode, "invalid_url") << url;
    }
    HttpRequestDesc desc = get("/text");
    desc.method = "BAD METHOD";
    EXPECT_EQ(run(desc).errorCode, "unsupported");
}

// 12
TEST_F(HttpTest, OfflineSimulation) {
    Http::setOfflineSimulated(true);
    HttpRequestDesc desc;
    desc.url = "https://example.invalid/";
    std::shared_ptr<HttpRequestHandle> handle;
    auto result = start(desc, &handle);
    EXPECT_EQ(result->calls, 0);
    MainThreadQueue::drain();
    EXPECT_EQ(result->calls, 1) << "fails after one frame";
    EXPECT_EQ(result->response.errorCode, "offline");
    EXPECT_NE(result->response.error.find("simulated"), std::string::npos);
    EXPECT_TRUE(run(get("/text")).ok) << "localhost is not affected";
    Http::setOfflineSimulated(false);
    EXPECT_FALSE(Http::isOfflineSimulated());
}

// 13
TEST_F(HttpTest, OwnerDestroyedEndsTheRequestSilently) {
    Scene scene;
    const Entity entity = scene.createUserEntity();
    std::shared_ptr<HttpRequestHandle> handle;
    auto result = start(get("/slow/3000"), &handle, AsyncOwners::fromEntity(&scene, entity));
    ASSERT_TRUE(waitFor([&] { return server->requests("/slow/3000") == 1; }));
    scene.destroyEntity(entity);
    EXPECT_EQ(handle->state(), HttpState::Cancelled);
    ASSERT_TRUE(waitFor([] { return HttpTesting::runningTransfers() == 0; }, 2.0)) << "the transfer is cancelled";
    waitFor([] { return false; }, 0.2);
    EXPECT_EQ(result->calls, 0);

    // a queued one never starts
    Http::setMaxConcurrentRequests(1);
    const Entity other = scene.createUserEntity();
    auto blocker = start(get("/slow/300"));
    auto queued = start(get("/count/0"), &handle, AsyncOwners::fromEntity(&scene, other));
    scene.destroyEntity(other);
    ASSERT_TRUE(waitFor([&] { return blocker->calls == 1; }));
    waitFor([] { return false; }, 0.2);
    EXPECT_EQ(queued->calls, 0);
    EXPECT_EQ(server->requests("/count/0"), 0);

    // an entity that is gone already: nothing is sent
    auto gone = start(get("/count/0"), &handle, AsyncOwners::fromEntity(&scene, other));
    EXPECT_EQ(handle->state(), HttpState::Cancelled);
    waitFor([] { return false; }, 0.1);
    EXPECT_EQ(gone->calls, 0);
}

TEST_F(HttpTest, SceneAndScriptOwners) {
    auto scene = std::make_unique<Scene>();
    const Entity entity = scene->createUserEntity();
    auto bySceneResult = start(get("/slow/3000"), nullptr, AsyncOwners::fromScene(scene.get()));
    auto byEntityResult = start(get("/slow/3000"), nullptr, AsyncOwners::fromEntity(scene.get(), entity));
    int instanceKey = 0;
    auto byScriptResult = start(get("/slow/3000"), nullptr, AsyncOwners::fromScriptInstance(&instanceKey, scene.get(), entity));
    int otherKey = 0;
    std::shared_ptr<HttpRequestHandle> scriptHandle;
    auto byOtherScript = start(get("/slow/3000"), &scriptHandle, AsyncOwners::fromScriptInstance(&otherKey));
    EXPECT_GE(AsyncOwners::count(), 4u);

    AsyncOwners::scriptInstanceReleased(&otherKey);
    EXPECT_EQ(scriptHandle->state(), HttpState::Cancelled);
    scene.reset();
    ASSERT_TRUE(waitFor([] { return HttpTesting::runningTransfers() == 0; }, 2.0));
    waitFor([] { return false; }, 0.2);
    EXPECT_EQ(bySceneResult->calls, 0);
    EXPECT_EQ(byEntityResult->calls, 0);
    EXPECT_EQ(byScriptResult->calls, 0);
    EXPECT_EQ(byOtherScript->calls, 0);

    // without an owner a request survives scene deletes
    auto global = start(get("/text"));
    Scene().createUserEntity();
    ASSERT_TRUE(waitFor([&] { return global->calls == 1; }));
}

// persistence: a GameObject moved to another scene (DontDestroyOnLoad) keeps the requests it
// and its scripts own; its old scene's own requests, and the GameObjects left there, do not
TEST_F(HttpTest, OwnersFollowGameObjectsMovedToAnotherScene) {
    auto level = std::make_unique<Scene>();
    auto persistent = std::make_unique<Scene>();
    const Entity manager = level->createUserEntity();
    level->addComponent<Transform>(manager, Transform());
    const Entity child = level->createUserEntity();
    level->addComponent<Transform>(child, Transform());
    level->addEntityChild(manager, child, false);
    const Entity stays = level->createUserEntity();
    level->addComponent<Transform>(stays, Transform());

    int scriptKey = 0;
    auto byManager = start(get("/slow/400"), nullptr, AsyncOwners::fromEntity(level.get(), manager));
    auto byChild = start(get("/slow/400"), nullptr, AsyncOwners::fromEntity(level.get(), child));
    auto byScript = start(get("/slow/400"), nullptr, AsyncOwners::fromScriptInstance(&scriptKey, level.get(), manager));
    auto byScene = start(get("/slow/3000"), nullptr, AsyncOwners::fromScene(level.get()));
    auto byStays = start(get("/slow/3000"), nullptr, AsyncOwners::fromEntity(level.get(), stays));

    const std::vector<Entity> copies = ObjectManager::moveToScene(level.get(), manager, persistent.get());
    ASSERT_EQ(copies.size(), 2u);
    EXPECT_FALSE(level->isEntityCreated(manager));
    level.reset();
    ASSERT_TRUE(waitFor([&] { return byManager->calls == 1 && byChild->calls == 1 && byScript->calls == 1; }))
        << "the moved GameObjects' requests still call back";
    EXPECT_TRUE(byManager->response.ok);
    EXPECT_EQ(text(byScript->response.body), "slow");
    EXPECT_EQ(byScene->calls, 0);
    EXPECT_EQ(byStays->calls, 0);

    // the owners now are the copies: destroying one ends its requests
    auto byCopy = start(get("/slow/3000"), nullptr, AsyncOwners::fromEntity(persistent.get(), copies[0]));
    auto byMovedScript = start(get("/slow/3000"), nullptr, AsyncOwners::fromScriptInstance(&scriptKey, persistent.get(), copies[0]));
    persistent->destroyEntity(copies[0]);
    ASSERT_TRUE(waitFor([] { return HttpTesting::runningTransfers() == 0; }, 2.0));
    waitFor([] { return false; }, 0.2);
    EXPECT_EQ(byCopy->calls, 0);
    EXPECT_EQ(byMovedScript->calls, 0);
    EXPECT_EQ(byScene->calls, 0);
    EXPECT_EQ(byStays->calls, 0);
}

TEST_F(HttpTest, ShutdownDropsEverythingWithoutCallbacks) {
    auto a = start(get("/slow/3000"));
    Http::setMaxConcurrentRequests(1);
    auto b = start(get("/slow/3000"));
    Http::shutdown();
    MainThreadQueue::clear();
    ASSERT_TRUE(waitFor([] { return HttpTesting::runningTransfers() == 0; }, 2.0));
    waitFor([] { return false; }, 0.2);
    EXPECT_EQ(a->calls, 0);
    EXPECT_EQ(b->calls, 0);
    EXPECT_EQ(Http::getStats().active, 0u);
    // and the next request is delivered (the queue's clear dropped nothing it needs)
    EXPECT_TRUE(run(get("/text")).ok);
}

// 14
TEST_F(HttpTest, DeliveryIsInRunFrameBeforeOnUpdate) {
    Engine::pauseGameEvents(false);
    std::vector<std::string> order;
    Engine::onUpdate.add("HttpTest", [&order]() { order.push_back("update"); });
    auto result = std::make_shared<Result>();
    Http::send(get("/text"), [&order, result](const HttpResponse& r) {
        order.push_back("http");
        result->calls++;
        result->response = r;
    });
    // the response arrives between frames (nothing drains the queue here)
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((MainThreadQueue::pending() == 0 || HttpTesting::runningTransfers() > 0) && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_GT(MainThreadQueue::pending(), 0u);
    {
        EXPECT_TRUE(order.empty());
        Engine::setSimulationHeld(true);
        Engine::systemUpdate();
        EXPECT_EQ(result->calls, 0) << "nothing is delivered while the simulation is held";
        Engine::setSimulationHeld(false);
        Engine::systemUpdate();
        ASSERT_EQ(result->calls, 1);
        ASSERT_GE(order.size(), 2u);
        EXPECT_EQ(order[0], "http");
        EXPECT_EQ(order[1], "update");
    }
    Engine::onUpdate.remove("HttpTest");
    Engine::pauseGameEvents(true);
    EXPECT_EQ(LuaBinding::getOffThreadUseCount(), 0u);
}

// 15
TEST_F(HttpTest, ProgressIsCoalescedMonotonicAndBeforeCompletion) {
    HttpRequestDesc desc = get("/trickle/1048576/3");
    desc.reportProgress = true;
    auto result = std::make_shared<Result>();
    bool progressAfterCompletion = false;
    int frames = 0;
    Http::send(desc, [result](const HttpResponse& r) {
        result->calls++;
        result->response = r;
    }, [result, &progressAfterCompletion](uint64_t received, int64_t total) {
        if (result->calls > 0) progressAfterCompletion = true;
        result->progress.emplace_back(received, total);
    });
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (result->calls == 0 && std::chrono::steady_clock::now() < end) {
        MainThreadQueue::drain();
        frames++;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    ASSERT_EQ(result->calls, 1);
    ASSERT_TRUE(result->response.ok);
    ASSERT_FALSE(result->progress.empty());
    EXPECT_LE((int)result->progress.size(), frames);
    EXPECT_GT(result->progress.size(), 2u);
    for (size_t i = 1; i < result->progress.size(); i++) EXPECT_GE(result->progress[i].first, result->progress[i - 1].first);
    EXPECT_EQ(result->progress.back().first, 1048576u);
    EXPECT_EQ(result->progress.back().second, 1048576);
    EXPECT_FALSE(progressAfterCompletion);
}

// 16
TEST_F(HttpTest, GzipIsDecodedTransparently) {
    const HttpResponse r = run(get("/gzip"));
    ASSERT_TRUE(r.ok);
    std::string expected;
    for (int i = 0; i < 100; i++) expected += "gzip body ";
    EXPECT_EQ(text(r.body), expected);
}

// 17
TEST_F(HttpTest, AvailableAndTheMissingLibrarySeam) {
    EXPECT_TRUE(Http::isAvailable());
    EXPECT_EQ(Http::unavailableReason(), "");
    HttpTesting::setCurlLibraryNames({"libjasper-no-such-curl.so.4"});
    EXPECT_FALSE(Http::isAvailable());
    const HttpResponse r = run(get("/text"));
    EXPECT_EQ(r.errorCode, "unsupported");
    EXPECT_NE(r.error.find("libcurl not found: install libcurl4"), std::string::npos) << r.error;
    EXPECT_EQ(system.count("libcurl not found"), 1) << "logged once as a warning";
    EXPECT_EQ(server->totalRequests(), 0);
    HttpTesting::setCurlLibraryNames({});
    EXPECT_TRUE(Http::isAvailable());
    EXPECT_TRUE(run(get("/text")).ok);
}

TEST_F(HttpTest, ManagedHeadersAreIgnoredWithOneWarning) {
    HttpRequestDesc desc = get("/echo");
    desc.headers = {{"Host", "evil"}, {"Connection", "keep-alive"}, {"Accept-Encoding", "br"}};
    nlohmann::json echo = json(run(desc));
    EXPECT_NE(echo["headers"]["host"], "evil");
    // once per process (a repeated run of the tests warned already)
    const int warned = system.count("Host header");
    EXPECT_LE(warned, 1);
    run(desc);
    EXPECT_EQ(system.count("Host header"), warned);
}

TEST_F(HttpTest, StatsAndLogFailures) {
    run(get("/text"));
    run(get("/status/500"));
    Http::setLogFailures(true);
    run(get("/status/404"));
    const Http::Stats stats = Http::getStats();
    EXPECT_EQ(stats.finished, 1u);
    EXPECT_EQ(stats.failed, 2u);
    ASSERT_EQ(stats.lastFailures.size(), 2u);
    EXPECT_EQ(stats.lastFailures[1].status, 404);
    EXPECT_EQ(stats.lastFailures[1].method, "GET");
    EXPECT_TRUE(stats.lastFailures[1].body.empty());
    EXPECT_EQ(system.count("HTTP GET " + server->url("/status/404") + " failed: HTTP 404 Not Found"), 1);
    EXPECT_EQ(system.count("/status/500 failed"), 0);
    for (int i = 0; i < 7; i++) run(get("/status/503"));
    EXPECT_EQ(Http::getStats().lastFailures.size(), 5u);
    Http::shutdown();
    EXPECT_EQ(Http::getStats().failed, 0u);
}

TEST_F(HttpTest, UrlHelpers) {
    EXPECT_EQ(HttpUtil::resolveUrl("https://a.com/x/y?q=1", "/z"), "https://a.com/z");
    EXPECT_EQ(HttpUtil::resolveUrl("https://a.com/x/y?q=1", "z"), "https://a.com/x/z");
    EXPECT_EQ(HttpUtil::resolveUrl("https://a.com/x/y", "../z?k"), "https://a.com/z?k");
    EXPECT_EQ(HttpUtil::resolveUrl("https://a.com/x/y", "//b.org/p"), "https://b.org/p");
    EXPECT_EQ(HttpUtil::resolveUrl("https://a.com/x/y", "http://c.net/"), "http://c.net/");
    EXPECT_EQ(HttpUtil::resolveUrl("https://a.com/x/y?q", "?r"), "https://a.com/x/y?r");
    EXPECT_EQ(HttpUtil::resolveUrl("https://a.com", "p"), "https://a.com/p");
    HttpUtil::Url url;
    ASSERT_TRUE(HttpUtil::parseUrl("https://user:pw@[::1]:8443/p?q#f", url));
    EXPECT_EQ(url.host, "::1");
    EXPECT_EQ(url.port, 8443);
    EXPECT_EQ(url.target, "/p?q");
    EXPECT_TRUE(HttpUtil::isLocalHost(url.host));
    ASSERT_TRUE(HttpUtil::parseUrl("HTTP://Example.com", url));
    EXPECT_EQ(url.scheme, "http");
    EXPECT_EQ(url.port, 80);
    EXPECT_EQ(url.target, "/");
}

// ------------------------------------------------------------------------------------- TLS

namespace {

    // A Python HTTPS server with a self-signed certificate (openssl), on 127.0.0.1
    struct TlsServer {
        pid_t pid = -1;
        int port = 0;
        fs::path dir;

        explicit TlsServer(const fs::path& folder) : dir(folder) {
            fs::create_directories(dir);
            const std::string cert = (dir / "cert.pem").string(), key = (dir / "key.pem").string();
            const std::string command = "openssl req -x509 -newkey rsa:2048 -nodes -keyout '" + key + "' -out '" + cert +
                                        "' -days 1 -subj /CN=127.0.0.1 >/dev/null 2>&1";
            if (std::system(command.c_str()) != 0) return;
            const fs::path script = dir / "server.py";
            scriptdata::writeFile(script, R"PY(import http.server, ssl, sys
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/to-http'):
            self.send_response(302)
            self.send_header('Location', 'http://example.invalid/x')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        body = b'secure'
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *args):
        pass
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(sys.argv[1], sys.argv[2])
server.socket = context.wrap_socket(server.socket, server_side=True)
print(server.server_address[1], flush=True)
server.serve_forever()
)PY");
            int pipeFds[2];
            if (::pipe(pipeFds) != 0) return;
            pid = ::fork();
            if (pid == 0) {
                ::dup2(pipeFds[1], STDOUT_FILENO);
                ::close(pipeFds[0]);
                ::close(pipeFds[1]);
                ::execlp("python3", "python3", "-I", script.c_str(), cert.c_str(), key.c_str(), (char*)nullptr);
                ::_exit(127);
            }
            ::close(pipeFds[1]);
            FILE* out = ::fdopen(pipeFds[0], "r");
            if (out && std::fscanf(out, "%d", &port) != 1) port = 0;
            if (out) std::fclose(out);
        }

        ~TlsServer() {
            if (pid > 0) {
                ::kill(pid, SIGTERM);
                ::waitpid(pid, nullptr, 0);
            }
        }
    };

}

TEST_F(HttpTest, TlsVerificationIsOnByDefault) {
    TlsServer tls(root / "tls");
    if (tls.port == 0) GTEST_SKIP() << "python3 and openssl are needed for the HTTPS server";
    const std::string url = "https://127.0.0.1:" + std::to_string(tls.port) + "/";
    HttpRequestDesc desc;
    desc.url = url;
    HttpResponse r = run(desc);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorCode, "tls") << r.error;

    desc.verifyTls = false;
    r = run(desc);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(text(r.body), "secure");
    const int warned = system.count("TLS certificate verification is off for 127.0.0.1");
    EXPECT_LE(warned, 1) << "once per host (and process)";
    run(desc);
    EXPECT_EQ(system.count("TLS certificate verification is off for 127.0.0.1"), warned) << "once per host";

    // https -> http is refused unless insecure HTTP is allowed
    desc.url = "https://127.0.0.1:" + std::to_string(tls.port) + "/to-http";
    r = run(desc);
    EXPECT_EQ(r.errorCode, "insecure_http");
}

// ------------------------------------------------------------------------------------- Lua

TEST_F(HttpTest, LuaGetAndResponseObject) {
    ASSERT_EQ(lua("res = nil; calls = 0; req = Http.get('" + server->url("/text") + "', function(r) calls = calls + 1; res = r end)"), "");
    EXPECT_EQ(luaValue("req.state == HttpState.Running"), "true");
    EXPECT_EQ(luaValue("req:isDone()"), "false");
    EXPECT_EQ(luaValue("req.response"), "nil");
    ASSERT_TRUE(waitFor([&] { return luaTrue("calls == 1"); }));
    EXPECT_EQ(luaValue("res.ok"), "true");
    EXPECT_EQ(luaValue("res.status"), "200");
    EXPECT_EQ(luaValue("res.body"), "hello http");
    EXPECT_EQ(luaValue("res.errorCode == '' and res.error == ''"), "true");
    EXPECT_EQ(luaValue("res.headers['x-test']"), "one, two");
    EXPECT_EQ(luaValue("res:header('Content-Type')"), "text/plain; charset=utf-8");
    EXPECT_EQ(luaValue("res:header('nope')"), "nil");
    EXPECT_EQ(luaValue("res.url"), server->url("/text"));
    EXPECT_EQ(luaValue("res.method"), "GET");
    EXPECT_EQ(luaValue("res.elapsed > 0"), "true");
    EXPECT_EQ(luaValue("res.bytesReceived"), "10");
    EXPECT_EQ(luaValue("res.redirected"), "false");
    EXPECT_EQ(luaValue("req.response == res"), "true");
    EXPECT_EQ(luaValue("req:isDone() and req.state == HttpState.Done"), "true");
    EXPECT_EQ(luaValue("req.progress"), "1.0");
    EXPECT_EQ(luaValue("res"), "HttpResponse(200 GET " + server->url("/text") + ")");
    EXPECT_EQ(luaValue("req.id > 0 and req.method == 'GET' and req.url"), server->url("/text"));
    EXPECT_EQ(luaValue("res.body == res.body"), "true");
    EXPECT_EQ(luaValue("res.textureData"), "nil");
    EXPECT_EQ(luaValue("res.data"), "nil");
}

TEST_F(HttpTest, LuaJsonPostQueryFormAndHeaders) {
    const std::string echo = server->url("/echo");
    ASSERT_EQ(lua("done = 0\n"
                  "Http.post('" + echo + "', {a = 1, list = {1, 2}}, function(r) post = r:json(); done = done + 1 end)\n"
                  "Http.get('" + echo + "', {query = {b = 2, a = 'x y', c = true}}, function(r) query = r:json(); done = done + 1 end)\n"
                  "Http.request{url = '" + echo + "', method = 'put', form = {name = 'J D', n = 1.5}, onComplete = function(r) form = r:json(); done = done + 1 end}\n"
                  "Http.request{url = '" + echo + "', method = 'POST', body = 'raw\\0bytes', headers = {{'Content-Type', 'application/x-raw'}, {'X-A', 'b'}}, onComplete = function(r) raw = r:json(); done = done + 1 end}\n"
                  "Http.get('" + server->url("/json") + "', {responseType = 'json'}, function(r) data = r.data; done = done + 1 end)\n"
                  "Http.get('" + server->url("/badjson") + "', {responseType = 'json'}, function(r) bad = r; done = done + 1 end)\n"), "");
    ASSERT_TRUE(waitFor([&] { return luaTrue("done == 6"); }));
    EXPECT_EQ(luaValue("post.method"), "POST");
    EXPECT_EQ(luaValue("post.headers['content-type']"), "application/json");
    EXPECT_EQ(luaValue("post.body_hex"), "7b2261223a312c226c697374223a5b312c325d7d");   // {"a":1,"list":[1,2]}
    EXPECT_EQ(luaValue("query.query"), "a=x%20y&b=2&c=true");
    EXPECT_EQ(luaValue("form.method"), "PUT");
    EXPECT_EQ(luaValue("form.headers['content-type']"), "application/x-www-form-urlencoded");
    EXPECT_EQ(luaValue("form.body_hex"), "6e3d312e35266e616d653d4a2b44");   // n=1.5&name=J+D
    EXPECT_EQ(luaValue("raw.body_hex"), "726177006279746573");   // raw\0bytes
    EXPECT_EQ(luaValue("raw.headers['content-type'] .. raw.headers['x-a']"), "application/x-rawb");
    EXPECT_EQ(luaValue("data.name .. data.n .. #data.list .. tostring(data.nested.ok)"), "jasper32true");
    EXPECT_EQ(luaValue("bad.ok"), "false");
    EXPECT_EQ(luaValue("bad.errorCode"), "decode");
    EXPECT_EQ(luaValue("select(2, bad:json()) ~= nil"), "true");
}

TEST_F(HttpTest, LuaArgumentErrorsRaiseAndOptionTyposWarnOnce) {
    EXPECT_NE(lua("Http.get(42)").find("Http.get: url must be a string"), std::string::npos);
    EXPECT_NE(lua("Http.get('http://127.0.0.1/', 5)").find("Http.get: options must be a table"), std::string::npos);
    EXPECT_NE(lua("Http.get('http://127.0.0.1/', {}, 5)").find("callback must be a function"), std::string::npos);
    EXPECT_NE(lua("Http.post('http://127.0.0.1/', 5)").find("Http.post: body must be"), std::string::npos);
    EXPECT_NE(lua("Http.download('http://127.0.0.1/')").find("Http.download: path must be a string"), std::string::npos);
    EXPECT_NE(lua("Http.request('x')").find("Http.request: the argument must be an options table"), std::string::npos);
    EXPECT_NE(lua("Http.request{}").find("options.url is missing"), std::string::npos);
    EXPECT_NE(lua("Http.get('http://127.0.0.1/', {timeout = 'x'})").find("options.timeout must be a number"), std::string::npos);
    EXPECT_NE(lua("Http.get('http://127.0.0.1/', {body = 'x'})").find("has no body"), std::string::npos);
    EXPECT_NE(lua("Http.get('http://127.0.0.1/', {owner = {}})").find("owner must be a script instance"), std::string::npos);
    EXPECT_NE(lua("Http.post('http://127.0.0.1/', {f = print})").find("cannot be sent as JSON"), std::string::npos);
    EXPECT_NE(lua("Http.post('http://127.0.0.1/', 'x', {json = {}})").find("given twice"), std::string::npos);
    EXPECT_NE(lua("Http.get('http://127.0.0.1/', {file = 'data://x', responseType = 'json'})").find("not both"), std::string::npos);
    EXPECT_NE(lua("Http.request{url = 'http://127.0.0.1/', responseType = 'text', file = 'data://x'}").find("not both"), std::string::npos);

    const std::string url = server->url("/text");
    ASSERT_EQ(lua("Http.get('" + url + "', {timout = 3})\nHttp.get('" + url + "', {timout = 3})"), "");
    EXPECT_LE(system.count("unknown option \"timout\""), 1) << "once per key (and process)";
    // errors are values, not Lua errors
    ASSERT_EQ(lua("r404 = nil; Http.get('" + server->url("/status/404") + "', function(r) r404 = r end)"), "");
    ASSERT_TRUE(waitFor([&] { return luaTrue("r404 ~= nil"); }));
    EXPECT_EQ(luaValue("r404.ok == false and r404.errorCode == 'http' and r404.status == 404 and r404.body"), "status 404");
    EXPECT_EQ(luaValue("Http.encodeUrl('a b&c')"), "a%20b%26c");
    EXPECT_EQ(luaValue("Http.decodeUrl('a+b%21')"), "a b!");
}

TEST_F(HttpTest, LuaCallbackErrorsAreLoggedAndOthersStillRun) {
    const std::string url = server->url("/text");
    ASSERT_EQ(lua("second = false\n"
                  "Http.get('" + url + "', function(r) error('boom in callback') end)\n"
                  "Http.get('" + url + "', function(r) second = true end)"), "");
    ASSERT_TRUE(waitFor([&] { return luaTrue("second"); }));
    EXPECT_EQ(system.count("boom in callback"), 1);
    EXPECT_EQ(system.count("stack traceback"), 1);
}

TEST_F(HttpTest, LuaProgressDownloadTextureAndProperties) {
    ASSERT_EQ(lua("steps = {}; downloaded = nil; tex = nil\n"
                  "dl = Http.download('" + server->url("/trickle/200000/2") + "', 'data://lua/d.bin', {onProgress = function(req, got, total)\n"
                  "  steps[#steps + 1] = got; sameReq = (req == dl); lastTotal = total end}, function(r) downloaded = r; stepsAtEnd = #steps end)\n"
                  "Http.getTexture('" + server->url("/png") + "', function(r) tex = r end)"), "");
    ASSERT_TRUE(waitFor([&] { return luaTrue("downloaded ~= nil and tex ~= nil"); }));
    EXPECT_EQ(luaValue("downloaded.ok"), "true");
    EXPECT_EQ(luaValue("downloaded.file"), "data://lua/d.bin");
    EXPECT_EQ(luaValue("downloaded.body"), "");
    EXPECT_EQ(luaValue("#steps > 0 and steps[#steps]"), "200000");
    EXPECT_EQ(luaValue("stepsAtEnd == #steps"), "true");
    EXPECT_EQ(luaValue("sameReq"), "true");
    EXPECT_EQ(luaValue("lastTotal"), "200000");
    EXPECT_EQ(luaValue("dl.bytesTotal"), "200000");
    EXPECT_EQ(scriptdata::readFile(root / "data" / "lua" / "d.bin"), TestHttpServer::pattern(200000));
    EXPECT_EQ(luaValue("tex.ok"), "true");
    EXPECT_EQ(luaValue("tex.textureData:getWidth() .. 'x' .. tex.textureData:getHeight()"), "4x2");
    EXPECT_EQ(luaValue("tex.body"), "");

    // properties
    EXPECT_EQ(luaValue("Http.maxConcurrentRequests"), "8");
    ASSERT_EQ(lua("Http.maxConcurrentRequests = 3; Http.timeout = 12.5; Http.userAgent = 'X/1'; Http.logFailures = true"), "");
    EXPECT_EQ(Http::getMaxConcurrentRequests(), 3);
    EXPECT_EQ(Http::getDefaultTimeout(), 12.5);
    EXPECT_EQ(Http::getUserAgent(), "X/1");
    EXPECT_TRUE(Http::getLogFailures());
    EXPECT_EQ(luaValue("Http.isAvailable()"), "true");
}

TEST_F(HttpTest, LuaCancelAndOwnerNil) {
    Http::setMaxConcurrentRequests(1);
    ASSERT_EQ(lua("got = {}\n"
                  "a = Http.get('" + server->url("/slow/2000") + "', {owner = nil}, function(r) got[#got + 1] = 'a:' .. r.errorCode end)\n"
                  "b = Http.get('" + server->url("/count/0") + "', function(r) got[#got + 1] = 'b:' .. r.errorCode end)\n"
                  "bState = b.state == HttpState.Queued\n"
                  "b:cancel(); a:cancel(); a:cancel()\n"
                  "cancelledNow = a.state == HttpState.Cancelled and a:isDone() and a.response.errorCode == 'cancelled'"), "");
    EXPECT_EQ(luaValue("bState"), "true");
    EXPECT_EQ(luaValue("cancelledNow"), "true");
    ASSERT_TRUE(waitFor([&] { return luaTrue("#got == 2"); }));
    EXPECT_EQ(luaValue("table.concat(got, ',')"), "b:cancelled,a:cancelled");
    EXPECT_EQ(luaValue("a.response == a.response"), "true");
}

TEST_F(HttpTest, LuaRequestsFromCoroutinesCallBackOnTheMainState) {
    ASSERT_EQ(lua("viaCoroutine = nil\n"
                  "local co = coroutine.create(function()\n"
                  "  Http.get('" + server->url("/text") + "', function(r) viaCoroutine = select(2, coroutine.running()) and r.body end)\n"
                  "end)\n"
                  "assert(coroutine.resume(co))"), "");
    ASSERT_TRUE(waitFor([&] { return luaValue("viaCoroutine") != "nil"; }));
    EXPECT_EQ(luaValue("viaCoroutine"), "hello http");
    EXPECT_EQ(LuaBinding::getOffThreadUseCount(), 0u);
}

// ------------------------------------------------------------------------------------- export scan

#include "util/ExportFeatureScan.h"

TEST(HttpExportScan, FindsHttpInLuaHtmlAndCpp) {
    namespace fs = std::filesystem;
    using jasperengine::editor::ExportFeatureScan;
    const fs::path root = fs::temp_directory_path() / ("jasper-http-scan-" + std::to_string(::getpid()));
    fs::remove_all(root);
    scriptdata::writeFile(root / "lua" / "a.lua", "local x = 1\nprint(HttpX)\n");
    scriptdata::writeFile(root / "assets" / "ui" / "menu.html", "<p>Http.get in text</p><script>Http.get()</script>");
    EXPECT_TRUE(ExportFeatureScan::scan(root, {ExportFeatureScan::http()}).empty()) << "no Lua use yet";

    scriptdata::writeFile(root / "assets" / "ui" / "shop.html", "<script type=\"text/lua\">\n  Http.get(url, cb)\n</script>");
    scriptdata::writeFile(root / "lua" / "net" / "b.lua", "Http :post(1)");
    scriptdata::writeFile(root / "scripts" / "Net.cpp", "#include \"net/Http.h\"\n");
    scriptdata::writeFile(root / ".hidden" / "c.lua", "Http.get()");
    auto uses = ExportFeatureScan::scan(root, {ExportFeatureScan::http()});
    ASSERT_EQ(uses.size(), 1u);
    EXPECT_EQ(uses[0].feature, "Http");
    EXPECT_EQ(uses[0].files, 3);
    EXPECT_EQ(uses[0].file, "assets/ui/shop.html");
    // an alias is a use; other names that start with Http are not
    const fs::path alias = root.parent_path() / ("jasper-http-scan-alias-" + std::to_string(::getpid()));
    fs::remove_all(alias);
    scriptdata::writeFile(alias / "lua" / "net.lua", "local H = Http\nreturn H");
    scriptdata::writeFile(alias / "lua" / "other.lua", "local s = HttpState.Done; local r = MyHttpThing");
    uses = ExportFeatureScan::scan(alias, {ExportFeatureScan::http()});
    ASSERT_EQ(uses.size(), 1u);
    EXPECT_EQ(uses[0].file, "lua/net.lua");
    EXPECT_EQ(uses[0].files, 1);
    fs::remove_all(alias);
    EXPECT_EQ(ExportFeatureScan::luaOfHtml("<SCRIPT type='text/lua'>a()</SCRIPT><script src=x.js></script>"), "a()\n");
    fs::remove_all(root);
}
