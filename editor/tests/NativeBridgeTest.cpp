// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The native bridge (feature native-bridge, docs/superpowers/specs/2026-10-31-native-bridge-design.md
// section 9): C++ plugins, a fake platform layer, Lua mocks and the Lua API on the Linux host,
// plus the plugins/native folder tools (scan, listing, scaffolding) and the project.yaml
// settings of the native plugins.

#include "ScriptDataTestSupport.h"

#include "Engine.h"
#include "Project.h"
#include "Scene.h"
#include "Stream.h"
#include "Workspace.h"
#include "native/NativeBridge.h"
#include "native/NativePlatform.h"
#include "script/AsyncOwner.h"
#include "thread/MainThreadQueue.h"
#include "log/Logger.h"
#include "util/NativeExportAndroid.h"
#include "util/NativePlugins.h"

#include <gtest/gtest.h>

#include "LuaBridge.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using jasperengine::editor::NativePlugins::CreateOptions;
using namespace jasperengine::editor;

namespace {

    namespace fs = std::filesystem;

    class NativeTestSystem : public scriptdata::TestSystem {
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

    // A platform layer answering for "Plat": sync echo / fail, async calls completed by the test
    class FakePlatform : public NativePlatform {
    public:
        std::mutex mutex;
        std::map<uint64_t, NativeJson> started;     // id -> args
        std::vector<uint64_t> cancelled;
        std::vector<bool> backgrounds;
        int shutdowns = 0;

        const char* name() const override { return "Android"; }
        bool has(const std::string& plugin, const std::string& method) override {
            if (plugin != "Plat" && plugin != "Shared") return false;
            return method.empty() || method == "echo" || method == "fail" || method == "later";
        }
        NativeJson call(const std::string& plugin, const std::string& method, const NativeJson& args, NativeError& err) override {
            if (method == "echo") return NativeJson{{"plugin", plugin}, {"args", args}};
            if (method == "fail") {
                err = NativeError("exception", "java.lang.IllegalStateException: nope");
                return nullptr;
            }
            err = NativeError("no_method", "no method " + method);
            return nullptr;
        }
        void callAsync(uint64_t id, const std::string&, const std::string&, const NativeJson& args, bool background) override {
            std::lock_guard<std::mutex> lock(mutex);
            started[id] = args;
            backgrounds.push_back(background);
        }
        void cancel(uint64_t id) override {
            std::lock_guard<std::mutex> lock(mutex);
            cancelled.push_back(id);
        }
        void shutdown() override { shutdowns++; }
    };

    struct Delivery {
        int calls = 0;
        NativeJson result;
        std::string code;
        std::string message;
    };

    class NativeBridgeTest : public ::testing::Test {
    protected:
        NativeTestSystem system;
        System* previous = nullptr;
        fs::path root;
        lua_State* L = nullptr;

        void SetUp() override {
            L = scriptdata::engineLua();
            ASSERT_NE(L, nullptr);
            const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
            root = fs::temp_directory_path() / ("jasper-native-" + std::to_string(::getpid()) + "-" + info->name());
            fs::remove_all(root);
            fs::create_directories(root / "data");
            fs::create_directories(root / "assets");
            fs::create_directories(root / "lua");
            system.root = root.string();
            previous = SystemTestAccess::swap(&system);
            reset();
        }

        void TearDown() override {
            // a test's fake platform is gone with the test body
            NativeBridgeInternal::setPlatformForTests(nullptr);
            reset();
            ::jasperengine::Logger::flush();   // nothing is delivered to the capture after it goes
            SystemTestAccess::swap(previous);
            lua_settop(L, 0);
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        void reset() {
            NativeBridge::shutdown();
            NativeBridge::clearCppPlugins();
            NativeBridge::setPlatformOnlyPlugins({});
            NativeBridge::setMockWarnings(false);
            MainThreadQueue::clear();
        }

        // one frame's delivery point
        void frame() {
            NativeBridge::update();
            MainThreadQueue::drain();
        }

        bool waitFor(const std::function<bool()>& done, double seconds = 5.0) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (std::chrono::steady_clock::now() < end) {
                frame();
                if (done()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        }

        NativeBridge::Completion recorder(const std::shared_ptr<Delivery>& d) {
            return [d](const NativeJson& result, const NativeError* error) {
                d->calls++;
                d->result = result;
                d->code = error ? error->code : std::string();
                d->message = error ? error->message : std::string();
            };
        }

        std::string lua(const std::string& code) {
            const int top = lua_gettop(L);
            std::string error;
            if (luaL_loadbuffer(L, code.data(), code.size(), "=nativetest") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
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
            if (luaL_loadbuffer(L, code.data(), code.size(), "=nativetest") != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
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

    void registerEcho(const char* name = "Echo") {
        NativeBridge::registerPlugin(name)
            .method("echo", [](const NativeJson& args) { return args; })
            .method("fail", [](const NativeJson&) -> NativeJson { throw NativeError("bad_args", "level must be > 0"); })
            .method("crash", [](const NativeJson&) -> NativeJson { throw std::runtime_error("boom"); })
            .method("nan", [](const NativeJson&) -> NativeJson { return NativeJson(std::nan("")); })
            .asyncMethod("later", [](NativeCall call) {
                std::thread([call]() { call.resolve(NativeJson{{"ok", true}, {"n", call.args().size()}}); }).detach();
            });
    }

}

// 1
TEST_F(NativeBridgeTest, CppSyncRoundTripsJson) {
    registerEcho();
    const NativeJson args = NativeJson::parse(R"([{"a": {"b": [1, 2.5, null, "x"]}}, 7, 7.0, null, "héllo ✓ 😀", true])");
    NativeError err;
    const NativeJson out = NativeBridge::call("Echo", "echo", args, err);
    ASSERT_FALSE(err.isSet()) << err.message;
    EXPECT_EQ(out, args);
    EXPECT_TRUE(out[1].is_number_integer());
    EXPECT_TRUE(out[2].is_number_float());

    // from Lua: integers stay integers, floats keep their point, nested nulls are Json.null
    ASSERT_EQ(lua("v, e, c = Native.call('Echo', 'echo', 7, 7.5, {x = {1, 2}}, 'é', false)"), "");
    EXPECT_EQ(luaValue("e == nil and c == nil"), "true");
    EXPECT_EQ(luaValue("math.type(v[1])"), "integer");
    EXPECT_EQ(luaValue("v[2]"), "7.5");
    EXPECT_EQ(luaValue("v[3].x[2]"), "2");
    EXPECT_EQ(luaValue("v[4]"), "é");
    EXPECT_EQ(luaValue("v[5]"), "false");
    ASSERT_EQ(lua("v = Native.call('Echo', 'echo', Json.null)"), "");
    EXPECT_EQ(luaValue("v[1] == Json.null"), "true");
    // a null result is nil
    NativeBridge::registerPlugin("Void").method("nothing", [](const NativeJson&) { return NativeJson(); });
    EXPECT_EQ(luaValue("select('#', Native.call('Void', 'nothing')) == 3 and Native.call('Void', 'nothing') == nil"), "true");
}

// 2
TEST_F(NativeBridgeTest, ErrorsAreValues) {
    registerEcho();
    ASSERT_EQ(lua("v, e, c = Native.call('Echo', 'fail')"), "");
    EXPECT_EQ(luaValue("v == nil and e .. '|' .. c"), "level must be > 0|bad_args");
    ASSERT_EQ(lua("v, e, c = Native.call('Echo', 'crash')"), "");
    EXPECT_EQ(luaValue("c"), "exception");
    EXPECT_NE(luaValue("e").find("boom"), std::string::npos);
    ASSERT_EQ(lua("v, e, c = Native.call('Echo', 'nope')"), "");
    EXPECT_EQ(luaValue("c"), "no_method");
    ASSERT_EQ(lua("v, e, c = Native.call('Echo', 'nan')"), "");
    EXPECT_EQ(luaValue("c"), "unsupported_type");
    ASSERT_EQ(lua("v, e, c = Native.call('Echo', 'later')"), "");
    EXPECT_EQ(luaValue("c"), "async_only");
    // arguments JSON cannot hold
    ASSERT_EQ(lua("v, e, c = Native.call('Echo', 'echo', function() end)"), "");
    EXPECT_EQ(luaValue("c"), "bad_args");
    ASSERT_EQ(lua("v, e, c = Native.call('Echo', 'echo', 0/0)"), "");
    EXPECT_EQ(luaValue("c"), "bad_args");
    ASSERT_EQ(lua("v, e, c = Native.call('Echo', 'echo', '\\xff')"), "");
    EXPECT_EQ(luaValue("c"), "invalid_utf8");
    ASSERT_EQ(lua("t = {}; t.self = t; v, e, c = Native.call('Echo', 'echo', t)"), "");
    EXPECT_EQ(luaValue("c"), "bad_args");
    // too deep (64 levels)
    ASSERT_EQ(lua("t = {}; local x = t; for i = 1, 70 do x[1] = {}; x = x[1] end; v, e, c = Native.call('Echo', 'echo', t)"), "");
    EXPECT_EQ(luaValue("c"), "bad_args");
    // programming errors raise
    EXPECT_NE(lua("Native.call(3, 'x')"), "");
    EXPECT_NE(lua("Native.call('Echo')"), "");
    EXPECT_NE(lua("Native.callAsync('Echo', 'echo', 5)"), "");
    EXPECT_NE(lua("Native.callAsync('Echo', 'echo', {a = 1})"), "");
    EXPECT_NE(lua("Native.callAsync('Echo', 'echo', {}, function() end, {thread = 'ui'})"), "");
    EXPECT_NE(lua("Native.on('Echo', 'x', 5)"), "");
}

// 3
TEST_F(NativeBridgeTest, AsyncResolvesFromAWorkerOnceAtTheDeliveryPoint) {
    registerEcho();
    auto d = std::make_shared<Delivery>();
    NativeCall call = NativeBridge::callAsync("Echo", "later", NativeJson::array({1, 2}), recorder(d));
    EXPECT_EQ(d->calls, 0) << "never inside callAsync";
    ASSERT_TRUE(waitFor([&] { return d->calls > 0; }));
    EXPECT_EQ(d->calls, 1);
    EXPECT_EQ(d->code, "");
    EXPECT_EQ(d->result["n"], 2);
    EXPECT_TRUE(call.isDelivered());
    EXPECT_TRUE(call.isDone());
    EXPECT_EQ(call.result()["ok"], true);
    // a second completion is ignored with a warning
    EXPECT_FALSE(call.resolve(1));
    EXPECT_EQ(system.count("completed twice"), 1);
    frame();
    EXPECT_EQ(d->calls, 1);

    // reject keeps the code; a sync method called async runs next frame
    NativeBridge::registerPlugin("R").asyncMethod("no", [](NativeCall c) { c.reject("not_ready", "later please"); })
        .method("sum", [](const NativeJson& a) { return a[0].get<int>() + a[1].get<int>(); });
    auto r = std::make_shared<Delivery>();
    NativeBridge::callAsync("R", "no", NativeJson::array(), recorder(r));
    auto s = std::make_shared<Delivery>();
    NativeBridge::callAsync("R", "sum", NativeJson::array({2, 3}), recorder(s), true);
    ASSERT_TRUE(waitFor([&] { return r->calls == 1 && s->calls == 1; }));
    EXPECT_EQ(r->code, "not_ready");
    EXPECT_EQ(r->message, "later please");
    EXPECT_EQ(s->result, 5);
}

TEST_F(NativeBridgeTest, TimeoutFailsAndNativeSeesCancelled) {
    NativeCall seen;
    NativeBridge::registerPlugin("Slow").asyncMethod("never", [&seen](NativeCall c) { seen = c; });
    auto d = std::make_shared<Delivery>();
    NativeCall call = NativeBridge::callAsync("Slow", "never", NativeJson::array(), recorder(d), false, {}, 0.05);
    ASSERT_TRUE(waitFor([&] { return d->calls > 0; }, 3.0));
    EXPECT_EQ(d->code, "timeout");
    ASSERT_TRUE(seen.isValid());
    EXPECT_TRUE(seen.isCancelled());
    EXPECT_FALSE(seen.resolve(1)) << "too late";
    frame();
    EXPECT_EQ(d->calls, 1);
    EXPECT_EQ(system.count("completed twice"), 0);

    // Lua: options.timeout and the callback's (nil, message, code)
    ASSERT_EQ(lua("got = nil; Native.callAsync('Slow', 'never', nil, function(r, e, c) got = c end, {timeout = 0.05})"), "");
    ASSERT_TRUE(waitFor([&] { return luaTrue("got == 'timeout'"); }, 3.0));
}

// 4
TEST_F(NativeBridgeTest, CancelAndOwnersDropTheCallback) {
    std::vector<NativeCall> seen;
    NativeBridge::registerPlugin("Hold").asyncMethod("wait", [&seen](NativeCall c) { seen.push_back(c); });
    auto a = std::make_shared<Delivery>();
    NativeCall call = NativeBridge::callAsync("Hold", "wait", NativeJson::array(), recorder(a));
    frame();
    ASSERT_EQ(seen.size(), 1u);
    call.cancel();
    EXPECT_TRUE(seen[0].isCancelled());
    EXPECT_FALSE(seen[0].resolve(1));
    frame();
    EXPECT_EQ(a->calls, 0);
    EXPECT_EQ(call.error().code, "cancelled");

    // owner destroyed: no callback, native sees cancelled
    Scene scene;
    const Entity entity = scene.createUserEntity();
    auto b = std::make_shared<Delivery>();
    NativeBridge::callAsync("Hold", "wait", NativeJson::array(), recorder(b), false, AsyncOwners::fromEntity(&scene, entity));
    frame();
    ASSERT_EQ(seen.size(), 2u);
    scene.destroyEntity(entity);
    EXPECT_TRUE(seen[1].isCancelled());
    seen[1].resolve(1);
    frame();
    frame();
    EXPECT_EQ(b->calls, 0);

    // a result already queued is dropped when the owner goes before the delivery
    int key = 0;
    auto c = std::make_shared<Delivery>();
    NativeBridge::callAsync("Hold", "wait", NativeJson::array(), recorder(c), false, AsyncOwners::fromScriptInstance(&key));
    frame();
    ASSERT_EQ(seen.size(), 3u);
    seen[2].resolve(1);
    AsyncOwners::scriptInstanceReleased(&key);
    frame();
    EXPECT_EQ(c->calls, 0);

    // a platform call: the platform is told
    FakePlatform platform;
    NativeBridgeInternal::setPlatformForTests(&platform);
    auto p = std::make_shared<Delivery>();
    NativeCall pc = NativeBridge::callAsync("Plat", "later", NativeJson::array({1}), recorder(p), true);
    ASSERT_EQ(platform.started.size(), 1u);
    EXPECT_TRUE(platform.backgrounds[0]);
    pc.cancel();
    ASSERT_EQ(platform.cancelled.size(), 1u);
    EXPECT_EQ(platform.cancelled[0], pc.id());
    NativeBridge::completeFromPlatform(pc.id(), 3, nullptr);
    frame();
    EXPECT_EQ(p->calls, 0);
}

// 5
TEST_F(NativeBridgeTest, EventsFromAWorkerReachHandlersInOrder) {
    std::vector<std::string> order;
    const uint64_t first = NativeBridge::on("Ads", "rewarded", [&order](const NativeJson& p) { order.push_back("a" + p["n"].dump()); });
    NativeBridge::on("Ads", "rewarded", [&order](const NativeJson& p) { order.push_back("b" + p["n"].dump()); });
    NativeBridge::on("Ads", "other", [&order](const NativeJson&) { order.push_back("other"); });
    std::thread([]() {
        NativeBridge::emit("Ads", "rewarded", NativeJson{{"n", 1}});
        NativeBridge::emit("Ads", "rewarded", NativeJson{{"n", 2}});
    }).join();
    EXPECT_TRUE(order.empty());
    frame();
    EXPECT_EQ(order, (std::vector<std::string>{"a1", "b1", "a2", "b2"}));
    NativeBridge::off(first);
    NativeBridge::off(first);
    NativeBridge::off(12345678);
    NativeBridge::emit("Ads", "rewarded", NativeJson{{"n", 3}});
    frame();
    EXPECT_EQ(order.back(), "b3");
    EXPECT_EQ(order.size(), 5u);

    // owner removal
    Scene scene;
    const Entity entity = scene.createUserEntity();
    int owned = 0;
    NativeBridge::on("Ads", "x", [&owned](const NativeJson&) { owned++; }, AsyncOwners::fromEntity(&scene, entity));
    NativeBridge::emit("Ads", "x", nullptr);
    frame();
    EXPECT_EQ(owned, 1);
    scene.destroyEntity(entity);
    NativeBridge::emit("Ads", "x", nullptr);
    frame();
    EXPECT_EQ(owned, 1);

    // Lua: Native.on / off and the payload (a null payload is nil)
    ASSERT_EQ(lua("got = {}; id1 = Native.on('Ads', 'paid', function(p) got[#got + 1] = p and p.amount or 'nil' end)"), "");
    ASSERT_EQ(lua("id2 = Native.on('Ads', 'paid', function(p) got[#got + 1] = 'second' end)"), "");
    NativeBridge::emit("Ads", "paid", NativeJson{{"amount", 5}});
    NativeBridge::emit("Ads", "paid", nullptr);
    frame();
    EXPECT_EQ(luaValue("table.concat(got, ',')"), "5,second,nil,second");
    ASSERT_EQ(lua("Native.off(id2); Native.emitMock('Ads', 'paid', {amount = 9})"), "");
    EXPECT_EQ(luaValue("#got"), "4") << "emitMock delivers next frame";
    frame();
    EXPECT_EQ(luaValue("table.concat(got, ',')"), "5,second,nil,second,9");
    EXPECT_EQ(luaValue("math.type(id1)"), "integer");
}

TEST_F(NativeBridgeTest, EventsWhilePausedWaitForTheResume) {
    int got = 0;
    NativeBridge::on("P", "e", [&got](const NativeJson&) { got++; });
    NativeBridge::pauseEngine();
    NativeBridge::pauseEngine();
    NativeBridge::update();
    EXPECT_TRUE(Engine::isPaused());
    NativeBridge::emit("P", "e", nullptr);
    // a paused engine does not run the update block, so the delivery point waits
    Engine::pauseGameEvents(false);
    Engine::systemUpdate();
    Engine::pauseGameEvents(true);
    EXPECT_EQ(got, 0);
    NativeBridge::resumeEngine();
    NativeBridge::update();
    EXPECT_TRUE(Engine::isPaused()) << "nested pauses need as many resumes";
    NativeBridge::resumeEngine();
    NativeBridge::update();
    EXPECT_FALSE(Engine::isPaused());
    MainThreadQueue::drain();
    EXPECT_EQ(got, 1);
    NativeBridge::resumeEngine();
    EXPECT_EQ(system.count("resumeEngine without a pauseEngine"), 1);
    EXPECT_EQ(NativeBridge::getPauseCount(), 0);

    // shutdown (Play stop) resets a pause the game left
    NativeBridge::pauseEngine();
    NativeBridge::update();
    EXPECT_TRUE(Engine::isPaused());
    NativeBridge::shutdown();
    EXPECT_FALSE(Engine::isPaused());
    EXPECT_EQ(NativeBridge::getPauseCount(), 0);
}

// 6
TEST_F(NativeBridgeTest, ResolutionOrderCppPlatformMock) {
    FakePlatform platform;
    NativeBridgeInternal::setPlatformForTests(&platform);
    ASSERT_EQ(lua("Native.setMock('Shared', {echo = function() return 'mock' end, only = function() return 1 end})"), "");
    // the platform beats the mock
    EXPECT_EQ(luaValue("Native.call('Shared', 'echo').plugin"), "Shared");
    // C++ beats the platform
    NativeBridge::registerPlugin("Shared").method("echo", [](const NativeJson&) { return NativeJson("cpp"); });
    EXPECT_EQ(luaValue("Native.call('Shared', 'echo')"), "cpp");
    NativeBridge::unregisterPlugin("Shared");
    // the platform's errors
    ASSERT_EQ(lua("v, e, c = Native.call('Plat', 'fail')"), "");
    EXPECT_EQ(luaValue("c"), "exception");
    // the mock answers when nothing else does
    ASSERT_EQ(lua("Native.setMock('OnlyMock', {get = function(a, b) return a + b end})"), "");
    EXPECT_EQ(luaValue("Native.call('OnlyMock', 'get', 2, 3)"), "5");
    // has: C++, platform, mock, with and without a method
    NativeBridge::registerPlugin("CppOnly").method("m", [](const NativeJson&) { return NativeJson(1); });
    EXPECT_TRUE(luaTrue("Native.has('CppOnly') and Native.has('CppOnly', 'm') and not Native.has('CppOnly', 'x')"));
    EXPECT_TRUE(luaTrue("Native.has('Plat') and Native.has('Plat', 'echo') and not Native.has('Plat', 'zzz')"));
    EXPECT_TRUE(luaTrue("Native.has('OnlyMock', 'get') and not Native.has('OnlyMock', 'set')"));
    EXPECT_TRUE(luaTrue("not Native.has('Nothing')"));
    ASSERT_EQ(lua("Native.clearMock('OnlyMock')"), "");
    EXPECT_TRUE(luaTrue("not Native.has('OnlyMock')"));

    // async through the platform: completed from another thread
    auto d = std::make_shared<Delivery>();
    NativeCall call = NativeBridge::callAsync("Plat", "later", NativeJson::array({"x"}), recorder(d));
    ASSERT_EQ(platform.started.count(call.id()), 1u);
    EXPECT_EQ(platform.started[call.id()], NativeJson::array({"x"}));
    EXPECT_FALSE(platform.backgrounds[0]) << "main (the UI thread) is the default";
    std::thread([id = call.id()]() { NativeBridge::completeFromPlatform(id, NativeJson{{"done", 1}}, nullptr); }).join();
    ASSERT_TRUE(waitFor([&] { return d->calls == 1; }));
    EXPECT_EQ(d->result["done"], 1);
    // unknown ids are ignored
    NativeBridge::completeFromPlatform(999999, 1, nullptr);
    frame();
}

// 7
TEST_F(NativeBridgeTest, MockAutoLoadAndPending) {
    scriptdata::writeFile(root / "lua" / "native_mocks" / "Device.lua",
        "local Device = {}\n"
        "loads = (loads or 0) + 1\n"
        "function Device.getCode() return 42 end\n"
        "function Device.fail() return nil, 'no device', 'not_ready' end\n"
        "function Device.oops() error('mock broke') end\n"
        "function Device.login(user, call) pendingCall = call; lastUser = user; return Native.pending() end\n"
        "function Device.quick(call) return {ok = true} end\n"
        "return Device\n");
    ASSERT_EQ(lua("loads = 0"), "");
    EXPECT_EQ(luaValue("Native.call('Device', 'getCode')"), "42");
    EXPECT_EQ(luaValue("Native.call('Device', 'getCode')"), "42");
    EXPECT_EQ(luaValue("loads"), "1") << "loaded once";
    ASSERT_EQ(lua("v, e, c = Native.call('Device', 'fail')"), "");
    EXPECT_EQ(luaValue("e .. '|' .. c"), "no device|not_ready");
    ASSERT_EQ(lua("v, e, c = Native.call('Device', 'oops')"), "");
    EXPECT_EQ(luaValue("c"), "exception");
    EXPECT_NE(luaValue("e").find("mock broke"), std::string::npos);
    ASSERT_EQ(lua("v, e, c = Native.call('Device', 'login')"), "");
    EXPECT_EQ(luaValue("c"), "async_only");

    // pending + a later call:resolve
    ASSERT_EQ(lua("res = nil; call = Native.callAsync('Device', 'login', {'ann'}, function(r, e, c) res = r end)"), "");
    EXPECT_EQ(luaValue("lastUser"), "ann");
    EXPECT_EQ(luaValue("call:isDone()"), "false");
    frame();
    frame();
    EXPECT_EQ(luaValue("res"), "nil");
    EXPECT_EQ(luaValue("pendingCall:resolve({token = 'abc'})"), "true");
    EXPECT_EQ(luaValue("pendingCall:resolve(1)"), "false");
    frame();
    EXPECT_EQ(luaValue("res.token"), "abc");
    EXPECT_EQ(luaValue("call:isDone() and call.result.token"), "abc");
    EXPECT_EQ(luaValue("call.errorCode"), "nil");

    // a plain return is delivered next frame
    ASSERT_EQ(lua("q = nil; Native.callAsync('Device', 'quick', function(r) q = r end)"), "");
    EXPECT_EQ(luaValue("q"), "nil");
    frame();
    EXPECT_EQ(luaValue("q.ok"), "true");
    // reject from the mock's side
    ASSERT_EQ(lua("rc = nil; Native.callAsync('Device', 'login', {'bob'}, function(r, e, c) rc = c .. ':' .. e end)"), "");
    ASSERT_EQ(lua("pendingCall:reject('cancelled_by_user', 'user closed it')"), "");
    frame();
    EXPECT_EQ(luaValue("rc"), "cancelled_by_user:user closed it");

    // a missing module is no mock (no error logged), a broken one logs
    EXPECT_EQ(luaValue("Native.has('Missing')"), "false");
    scriptdata::writeFile(root / "lua" / "native_mocks" / "Broken.lua", "return 5\n");
    EXPECT_EQ(luaValue("Native.has('Broken')"), "false");
    EXPECT_EQ(system.count("not a table"), 1);
}

// 8
TEST_F(NativeBridgeTest, NotFoundAndUnavailableOnLinux) {
    ASSERT_EQ(lua("v, e, c = Native.call('Nobody', 'x')"), "");
    EXPECT_EQ(luaValue("v == nil and c"), "not_found");
    NativeBridge::setPlatformOnlyPlugins({{"Device", "Java/Kotlin plugins run only on Android"}});
    ASSERT_EQ(lua("v, e, c = Native.call('Device', 'x'); v2, e2, c2 = Native.call('Device', 'y')"), "");
    EXPECT_EQ(luaValue("c"), "unavailable");
    EXPECT_NE(luaValue("e").find("Java/Kotlin plugins run only on Android"), std::string::npos);
    EXPECT_NE(luaValue("e").find("native_mocks/Device.lua"), std::string::npos);
    EXPECT_EQ(system.count("Native plugin \"Device\" has no implementation here"), 1) << "logged once per plugin";
    ASSERT_EQ(lua("got = nil; Native.callAsync('Device', 'x', nil, function(r, e, c) got = c end)"), "");
    frame();
    EXPECT_EQ(luaValue("got"), "unavailable");
}

// mobile-shipping: in a shipped game a Lua mock standing in for a plugin with code for other
// platforms only is a fake SDK: it says so once per plugin (never in the editor's Play)
TEST_F(NativeBridgeTest, MockInAShippedBuildWarnsOnce) {
    NativeBridge::setPlatformOnlyPlugins({{"Ads", "Java/Kotlin plugins run only on Android"}});
    ASSERT_EQ(lua("Native.setMock('Ads', {show = function() return true end}); Native.setMock('Free', {get = function() return 1 end})"), "");
    ASSERT_EQ(lua("assert(Native.call('Ads', 'show') == true)"), "");
    EXPECT_EQ(system.count("answered by its Lua mock"), 0) << "Play: mocks are what answers there";
    NativeBridge::setMockWarnings(true);
    ASSERT_EQ(lua("Native.call('Ads', 'show'); Native.callAsync('Ads', 'show'); Native.call('Free', 'get')"), "");
    frame();
    EXPECT_EQ(system.count("Native plugin \"Ads\" is answered by its Lua mock in this build: Java/Kotlin plugins run only on Android"), 1)
        << "once per plugin";
    EXPECT_EQ(system.count("\"Free\" is answered"), 0) << "a plugin with no code anywhere is the game's own mock";
}

// 9
TEST_F(NativeBridgeTest, ShutdownAndClearLeaveNothing) {
    std::vector<NativeCall> seen;
    NativeBridge::registerPlugin("Hold").asyncMethod("wait", [&seen](NativeCall c) { seen.push_back(c); });
    auto d = std::make_shared<Delivery>();
    NativeBridge::callAsync("Hold", "wait", NativeJson::array(), recorder(d));
    frame();
    int handled = 0;
    NativeBridge::on("Hold", "e", [&handled](const NativeJson&) { handled++; });
    scriptdata::writeFile(root / "lua" / "native_mocks" / "Cached.lua", "return {v = function() return 1 end}\n");
    ASSERT_EQ(lua("Native.setMock('Set', {v = function() return 2 end}); assert(Native.call('Cached', 'v') == 1)"), "");
    EXPECT_EQ(luaValue("package.loaded['native_mocks.Cached'] ~= nil"), "true");
    FakePlatform platform;
    NativeBridgeInternal::setPlatformForTests(&platform);

    NativeBridge::Stats before = NativeBridge::getStats();
    EXPECT_EQ(before.pendingCalls, 1u);
    EXPECT_GE(before.handlers, 1u);
    NativeBridge::shutdown();
    EXPECT_TRUE(seen[0].isCancelled());
    NativeBridge::emit("Hold", "e", nullptr);
    frame();
    EXPECT_EQ(handled, 0);
    EXPECT_EQ(d->calls, 0);
    const NativeBridge::Stats after = NativeBridge::getStats();
    EXPECT_EQ(after.pendingCalls, 0u);
    EXPECT_EQ(after.handlers, 0u);
    EXPECT_EQ(platform.shutdowns, 1);
    EXPECT_EQ(luaValue("package.loaded['native_mocks.Cached']"), "nil");
    EXPECT_EQ(luaValue("Native.has('Set')"), "false");
    // C++ plugins stay until clearCppPlugins
    EXPECT_TRUE(NativeBridge::hasCppPlugin("Hold"));
    NativeBridge::clearCppPlugins();
    EXPECT_FALSE(NativeBridge::hasCppPlugin("Hold"));
    EXPECT_EQ(NativeBridge::getStats().cppPlugins, 0u);
}

TEST_F(NativeBridgeTest, ClearCppPluginsWaitsForBackgroundCalls) {
    std::atomic<bool> release{false};
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    NativeBridge::registerPlugin("Bg").method("work", [&](const NativeJson&) {
        running = true;
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        finished = true;
        return NativeJson(1);
    });
    NativeBridge::callAsync("Bg", "work", NativeJson::array(), nullptr, true);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!running && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(running);
    std::thread releaser([&release]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        release = true;
    });
    NativeBridge::clearCppPlugins();
    EXPECT_TRUE(finished) << "the plugin code finished before the plugins went";
    releaser.join();
}

// 10 and the Lua handle
TEST_F(NativeBridgeTest, LuaCallAsyncHandleAndNoOffThreadLua) {
    registerEcho();
    ASSERT_EQ(lua("r = nil; c = Native.callAsync('Echo', 'later', {1, 2, 3}, function(res, err, code) r = res; rerr = err; rcode = code end, {thread = 'background', owner = nil})"), "");
    EXPECT_EQ(luaValue("c:isDone()"), "false");
    EXPECT_EQ(luaValue("c.result"), "nil");
    EXPECT_EQ(luaValue("c.plugin .. '.' .. c.method"), "Echo.later");
    EXPECT_EQ(luaValue("math.type(c.id) == 'integer' and c.id > 0"), "true");
    ASSERT_TRUE(waitFor([&] { return luaTrue("r ~= nil"); }));
    EXPECT_EQ(luaValue("r.n"), "3");
    EXPECT_EQ(luaValue("rerr == nil and rcode == nil"), "true");
    EXPECT_EQ(luaValue("c:isDone() and c.result.ok"), "true");
    EXPECT_EQ(luaValue("tostring(c):match('^NativeCall%(%d+ Echo.later done%)$') ~= nil"), "true");

    // cancel: no callback; the handle says cancelled
    NativeCall* held = nullptr;
    (void)held;
    NativeBridge::registerPlugin("Hold").asyncMethod("wait", [](NativeCall) {});
    ASSERT_EQ(lua("called = false; h = Native.callAsync('Hold', 'wait', nil, function() called = true end); h:cancel(); h:cancel()"), "");
    frame();
    EXPECT_EQ(luaValue("called"), "false");
    EXPECT_EQ(luaValue("h:isDone() and h.errorCode"), "cancelled");

    // args JSON cannot hold: the callback says so, next frame
    ASSERT_EQ(lua("bc = nil; Native.callAsync('Echo', 'echo', {function() end}, function(r, e, c) bc = c end)"), "");
    EXPECT_EQ(luaValue("bc"), "nil");
    frame();
    EXPECT_EQ(luaValue("bc"), "bad_args");

    // a Lua error in the callback is logged, other deliveries go on
    ASSERT_EQ(lua("ok2 = false; Native.callAsync('Echo', 'echo', {}, function() error('cb broke') end); Native.callAsync('Echo', 'echo', {}, function() ok2 = true end)"), "");
    frame();
    EXPECT_EQ(luaValue("ok2"), "true");
    EXPECT_GE(system.count("cb broke"), 1);

    // unknown options warn once
    ASSERT_EQ(lua("Native.callAsync('Echo', 'echo', {}, nil, {timout = 3}); Native.callAsync('Echo', 'echo', {}, nil, {timout = 3})"), "");
    EXPECT_EQ(system.count("unknown option \"timout\""), 1);
    frame();
    EXPECT_EQ(LuaBinding::getOffThreadUseCount(), 0u);
}

TEST_F(NativeBridgeTest, LuaOwnerDropsCallsAndHandlers) {
    NativeBridge::registerPlugin("Hold").asyncMethod("wait", [](NativeCall c) { c.resolve(1); });
    auto scene = std::make_unique<Scene>();
    ASSERT_TRUE(luabridge::push(L, scene.get()));
    lua_setglobal(L, "ownerScene");
    ASSERT_EQ(lua("called = 0; handled = 0; Native.callAsync('Hold', 'wait', nil, function() called = called + 1 end, {owner = ownerScene});"
                  "Native.on('Hold', 'e', function() handled = handled + 1 end, ownerScene); ownerScene = nil"), "");
    scene.reset();
    NativeBridge::emit("Hold", "e", nullptr);
    frame();
    frame();
    EXPECT_EQ(luaValue("called"), "0");
    EXPECT_EQ(luaValue("handled"), "0");
    EXPECT_NE(lua("Native.callAsync('Hold', 'wait', nil, nil, {owner = 5})"), "");
    EXPECT_NE(lua("Native.on('Hold', 'e', function() end, {})"), "");
}

TEST_F(NativeBridgeTest, ManySyncCallsWarnOnce) {
    registerEcho();
    for (int i = 0; i < 450; i++) {
        NativeError err;
        NativeBridge::call("Echo", "echo", NativeJson::array(), err);
    }
    EXPECT_EQ(system.count("more than 200 Native.call in one frame"), 1);
}

TEST_F(NativeBridgeTest, MacroRegistersAtLoad) {
    // a JASPERENGINE_NATIVE_PLUGIN below registered "MacroPlugin" at static initialization; the
    // fixture's clearCppPlugins dropped it: register again the way the macro does
    extern void nativeBridgeTestRegisterMacroPlugin();
    nativeBridgeTestRegisterMacroPlugin();
    NativeError err;
    EXPECT_EQ(NativeBridge::call("MacroPlugin", "hello", NativeJson::array({"x"}), err), "hello x");
}

namespace {
    JASPERENGINE_NATIVE_PLUGIN(MacroPlugin) {
        plugin.method("hello", [](const NativeJson& args) { return NativeJson("hello " + args[0].get<std::string>()); });
    }
}

void nativeBridgeTestRegisterMacroPlugin() {
    jasperNativePlugin_MacroPlugin(NativeBridge::registerPlugin("MacroPlugin"));
}

// ---------------------------------------------------------------- plugins/native, scaffolding, settings

TEST_F(NativeBridgeTest, ScaffoldWritesEveryPlatformAndRefusesToOverwrite) {
    namespace NP = jasperengine::editor::NativePlugins;
    const fs::path project = root / "project";
    fs::create_directories(project / "lua");
    CreateOptions options;
    options.name = "Device";
    options.platforms = {"android", "ios", "macos", "web", "cpp"};
    options.package = "com.me.plugins";
    options.methods = {{"getCode", false}, {"login", true}};
    EXPECT_EQ(NP::validate(options), "");
    std::vector<NP::CreatedFile> written;
    std::string error;
    ASSERT_TRUE(NP::create(project, project / "lua", options, written, error)) << error;
    std::vector<std::string> paths;
    for (const auto& file : written) paths.push_back(file.path);
    EXPECT_EQ(paths, (std::vector<std::string>{
        "plugins/native/android/java/com/me/plugins/Device.java", "plugins/native/apple/Device.h", "plugins/native/apple/Device.m",
        "plugins/native/web/device.js", "plugins/native/cpp/DevicePlugin.cpp", "lua/native_mocks/Device.lua"}));
    const std::string java = scriptdata::readFile(project / "plugins/native/android/java/com/me/plugins/Device.java");
    EXPECT_NE(java.find("package com.me.plugins;"), std::string::npos);
    EXPECT_NE(java.find("public static Object getCode()"), std::string::npos);
    EXPECT_NE(java.find("public static void login(final NativeCall call)"), std::string::npos);
    const std::string objc = scriptdata::readFile(project / "plugins/native/apple/Device.m");
    EXPECT_NE(objc.find("+ (id)getCode:(NSArray*)args"), std::string::npos);
    EXPECT_NE(objc.find("+ (void)login:(NSArray*)args call:(JasperNativeCall*)call"), std::string::npos);
    const std::string cpp = scriptdata::readFile(project / "plugins/native/cpp/DevicePlugin.cpp");
    EXPECT_NE(cpp.find("JASPERENGINE_NATIVE_PLUGIN(Device)"), std::string::npos);
    EXPECT_NE(cpp.find("#if !(defined(__ANDROID__) ||"), std::string::npos) << "the C++ plugin would shadow the platform ones";

    // refused without overwrite, nothing written
    fs::remove(project / "plugins/native/web/device.js");
    EXPECT_FALSE(NP::create(project, project / "lua", options, written, error));
    EXPECT_NE(error.find("already exists"), std::string::npos);
    EXPECT_FALSE(fs::exists(project / "plugins/native/web/device.js"));
    options.overwrite = true;
    ASSERT_TRUE(NP::create(project, project / "lua", options, written, error)) << error;

    // the listing finds them, their methods and the mock
    const NP::Scan scan = NP::scan(project);
    EXPECT_TRUE(scan.exists);
    EXPECT_EQ(scan.count("android"), 1u);
    EXPECT_EQ(scan.count("apple"), 2u);
    const std::vector<NP::PluginInfo> plugins = NP::listPlugins(scan);
    ASSERT_EQ(plugins.size(), 1u);
    EXPECT_EQ(plugins[0].name, "Device");
    EXPECT_EQ(plugins[0].methods, (std::vector<std::string>{"getCode", "login"}));
    EXPECT_EQ(plugins[0].files.size(), 4u);   // android, apple, web, cpp
    EXPECT_EQ(NP::listMocks(project / "lua"), (std::vector<std::string>{"Device"}));
    EXPECT_EQ(NP::platformOf(project, project / "plugins/native/ios/X.m"), "ios");
    EXPECT_EQ(NP::platformOf(project, project / "plugins/other/X.m"), "");

    // Kotlin and Swift, one Apple platform
    CreateOptions k;
    k.name = "Store";
    k.platforms = {"android", "ios"};
    k.androidLanguage = "kotlin";
    k.appleLanguage = "swift";
    k.methods = {{"buy", true}};
    ASSERT_TRUE(NP::create(project, project / "lua", k, written, error)) << error;
    EXPECT_TRUE(fs::exists(project / "plugins/native/android/kotlin/com/example/plugins/Store.kt"));
    EXPECT_TRUE(fs::exists(project / "plugins/native/apple/Store.swift"));
    const std::string swift = scriptdata::readFile(project / "plugins/native/apple/Store.swift");
    EXPECT_NE(swift.find("@objc(Store) public class Store: NSObject, JasperNativePlugin"), std::string::npos);
    EXPECT_NE(swift.find("@objc public static func buy(_ args: [Any], call: JasperNativeCall)"), std::string::npos);

    // only platform plugins: unavailable on the desktop, fine on their platform
    const auto only = NP::platformOnlyPlugins(NP::listPlugins(NP::scan(project)), "desktop");
    EXPECT_EQ(only.count("Device"), 0u) << "it has a C++ plugin";
    ASSERT_EQ(only.count("Store"), 1u);
    EXPECT_NE(only.at("Store").find("Java/Kotlin plugins run only on Android"), std::string::npos);
    EXPECT_EQ(NP::platformOnlyPlugins(NP::listPlugins(NP::scan(project)), "android").count("Store"), 0u);

    // validation
    CreateOptions bad = options;
    bad.name = "1x";
    EXPECT_NE(NP::validate(bad), "");
    bad = options;
    bad.platforms = {"windows"};
    EXPECT_NE(NP::validate(bad), "");
    bad = options;
    bad.methods = {{"a", false}, {"a", true}};
    EXPECT_NE(NP::validate(bad), "");
    bad = options;
    bad.package = "com.class.x";
    EXPECT_NE(NP::validate(bad), "");
}

TEST_F(NativeBridgeTest, ProjectSettingsRoundTripThroughProjectYaml) {
    editor::Project project;
    AndroidProjectSettings& android = project.getAndroidProjectSettings();
    android.gradleDependencies = {"com.wortise:android-sdk:1.7.1", "implementation platform('com.google.firebase:firebase-bom:33.0.0')"};
    android.gradleRepositories = {"https://maven.wortise.com/artifactory/public"};
    android.gradleProperties = {"android.enableJetifier=true"};
    android.extraPermissions = {"com.google.android.gms.permission.AD_ID"};
    android.manifestXml = "<queries><package android:name=\"com.x\"/></queries>";
    android.manifestApplicationXml = "<meta-data android:name=\"k\" android:value=\"v\"/>";
    IOSProjectSettings& ios = project.getIOSProjectSettings();
    NativePlistValue text;
    text.text = "This identifier will be used to deliver personalized ads to you.";
    NativePlistValue looksNumeric;
    looksNumeric.text = "15";
    NativePlistValue flag;
    flag.kind = NativePlistValue::Kind::Bool;
    flag.flag = true;
    NativePlistValue integer;
    integer.kind = NativePlistValue::Kind::Integer;
    integer.integer = 3;
    NativePlistValue real;
    real.kind = NativePlistValue::Kind::Real;
    real.real = 2.0;
    NativePlistValue list;
    list.kind = NativePlistValue::Kind::StringArray;
    list.items = {"fetch", "remote-notification"};
    ios.plistEntries = {{"NSUserTrackingUsageDescription", text}, {"Numeric", looksNumeric}, {"Flag", flag},
                        {"Count", integer}, {"Ratio", real}, {"UIBackgroundModes", list}};
    ios.plistXml = "<key>SKAdNetworkItems</key><array/>";
    ios.frameworks = {"StoreKit", "AppTrackingTransparency?"};
    ios.cocoapods = {{"WortiseSDK", "~> 1.7", ""}, {"Other", "", "https://github.com/x/specs.git"}};
    ios.swiftPackages = {{"https://github.com/RevenueCat/purchases-ios", "4.0.0", {"RevenueCat"}}};
    ios.entitlementsXml = "<key>aps-environment</key><string>development</string>";
    ios.deploymentTarget = "16.0";
    MacOSProjectSettings& macos = project.getMacOSProjectSettings();
    macos.plistEntries = {{"LSApplicationCategoryType", text}};
    macos.frameworks = {"GameKit"};
    project.getWebProjectSettings().scriptUrls = {"https://sdk.example.com/sdk.js"};

    const YAML::Node node = editor::Stream::encodeProject(&project);
    YAML::Emitter out;
    out << node;
    const YAML::Node reread = YAML::Load(out.c_str());
    editor::Project loaded;
    editor::Workspace workspace;
    editor::Stream::decodeProject(&loaded, reread, workspace);
    EXPECT_EQ(loaded.getAndroidProjectSettings().gradleDependencies, android.gradleDependencies);
    EXPECT_EQ(loaded.getAndroidProjectSettings().gradleRepositories, android.gradleRepositories);
    EXPECT_EQ(loaded.getAndroidProjectSettings().gradleProperties, android.gradleProperties);
    EXPECT_EQ(loaded.getAndroidProjectSettings().extraPermissions, android.extraPermissions);
    EXPECT_EQ(loaded.getAndroidProjectSettings().manifestXml, android.manifestXml);
    EXPECT_EQ(loaded.getAndroidProjectSettings().manifestApplicationXml, android.manifestApplicationXml);
    const IOSProjectSettings& lios = loaded.getIOSProjectSettings();
    EXPECT_TRUE(lios.plistEntries == ios.plistEntries);
    EXPECT_EQ(lios.plistEntries.at("Numeric").kind, NativePlistValue::Kind::String) << "a numeric-looking string stays a string";
    EXPECT_EQ(lios.plistEntries.at("Ratio").kind, NativePlistValue::Kind::Real);
    EXPECT_EQ(lios.plistXml, ios.plistXml);
    EXPECT_EQ(lios.frameworks, ios.frameworks);
    EXPECT_TRUE(lios.cocoapods == ios.cocoapods);
    EXPECT_TRUE(lios.swiftPackages == ios.swiftPackages);
    EXPECT_EQ(lios.entitlementsXml, ios.entitlementsXml);
    EXPECT_EQ(lios.deploymentTarget, "16.0");
    EXPECT_TRUE(loaded.getMacOSProjectSettings().plistEntries == macos.plistEntries);
    EXPECT_EQ(loaded.getMacOSProjectSettings().frameworks, macos.frameworks);
    EXPECT_EQ(loaded.getWebProjectSettings().scriptUrls, project.getWebProjectSettings().scriptUrls);

    // defaults are not written (old projects read back the same)
    editor::Project empty;
    const YAML::Node emptyNode = editor::Stream::encodeProject(&empty);
    EXPECT_FALSE(emptyNode["android"] && emptyNode["android"]["gradleDependencies"]);
    EXPECT_FALSE(emptyNode["ios"] && emptyNode["ios"]["deploymentTarget"]);
    EXPECT_FALSE(emptyNode["web"] && emptyNode["web"]["scriptUrls"]);
}

TEST_F(NativeBridgeTest, AndroidExportRegistryAndGradleText) {
    namespace NEA = jasperengine::editor::NativeExportAndroid;
    const auto java = NEA::parseSource(
        "// class Fake {}\npackage com.me.plugins;\nimport java.util.List;\n/* public class Hidden {} */\n"
        "@Deprecated\npublic final class Device {\n    public static class Inner {}\n    String s = \"class Nope\";\n}\n"
        "class PackagePrivate {}\npublic interface Callback {}\n", false, "java/com/me/plugins/Device.java");
    ASSERT_EQ(java.size(), 2u);
    EXPECT_EQ(java[0].qualifiedName, "com.me.plugins.Device");
    EXPECT_EQ(java[1].qualifiedName, "com.me.plugins.Callback");
    const auto kotlin = NEA::parseSource(
        "package com.me.k\n\nimport android.app.Activity\nimport com.x.Y as Z\n\nobject Store {\n    @JvmStatic fun buy() {}\n"
        "    companion object Nested {}\n}\ninternal class Hidden\nprivate object Secret\ndata class Item(val id: String)\n"
        "enum class Kind { A, B }\nval anon = object : Runnable { override fun run() {} }\n", true, "kotlin/Store.kt");
    std::vector<std::string> names;
    for (const auto& cls : kotlin) names.push_back(cls.qualifiedName);
    EXPECT_EQ(names, (std::vector<std::string>{"com.me.k.Store", "com.me.k.Item", "com.me.k.Kind"}));
    // modifiers of a member do not leak into the next top-level declaration (no ';' in Kotlin)
    const auto members = NEA::parseSource("package p\nprivate lateinit var adapter: Any\nobject AdsBridge {\n}\n"
                                          "internal val x = 1\nclass Visible\n", true, "Ads.kt");
    names.clear();
    for (const auto& cls : members) names.push_back(cls.qualifiedName);
    EXPECT_EQ(names, (std::vector<std::string>{"p.AdsBridge", "p.Visible"}));
    const auto loose = NEA::parseSource("public class Top {}\n", false, "Top.java");
    ASSERT_EQ(loose.size(), 1u);
    EXPECT_EQ(loose[0].qualifiedName, "Top");

    // duplicates are an error naming both files
    const fs::path android = root / "android";
    scriptdata::writeFile(android / "a" / "Device.java", "package a;\npublic class Device {}\n");
    scriptdata::writeFile(android / "b" / "Device.kt", "package b\nobject Device\n");
    std::vector<NEA::PluginClass> classes;
    std::string error;
    EXPECT_FALSE(NEA::collectClasses(android, classes, error));
    EXPECT_NE(error.find("a/Device.java"), std::string::npos);
    EXPECT_NE(error.find("b/Device.kt"), std::string::npos);
    fs::remove(android / "b" / "Device.kt");
    ASSERT_TRUE(NEA::collectClasses(android, classes, error)) << error;
    const std::string registry = NEA::registrySource(classes);
    EXPECT_NE(registry.find("classes.put(\"Device\", \"a.Device\");"), std::string::npos);
    const std::string rules = NEA::keepRules(classes, "-keep class x.** { *; }");
    EXPECT_NE(rules.find("-keep class a.** { *; }"), std::string::npos);
    EXPECT_NE(rules.find("-keep class x.** { *; }\n"), std::string::npos);

    EXPECT_EQ(NEA::dependencyLine("com.wortise:android-sdk:1.7.1", "api"), "api \"com.wortise:android-sdk:1.7.1\"");
    EXPECT_EQ(NEA::dependencyLine("implementation platform('a:b:1')", "api"), "implementation platform('a:b:1')");
    EXPECT_EQ(NEA::withGradleProperties("a=1\n# b=2\nc=3\n", {"c=4", "d=5"}), "a=1\n# b=2\nc=4\nd=5\n");
    EXPECT_EQ(NEA::withGradleProperties(NEA::withGradleProperties("a=1\n", {"a=2"}), {"a=2"}), "a=2\n") << "idempotent";

    EXPECT_EQ(jasperengine::editor::NativePlugins::xmlFragmentError("<a/><b x=\"1\">t</b>"), "");
    EXPECT_EQ(jasperengine::editor::NativePlugins::xmlFragmentError("<meta-data android:name=\"x\" tools:node=\"remove\"/>"), "");
    EXPECT_NE(jasperengine::editor::NativePlugins::xmlFragmentError("<a><b></a>"), "");
    EXPECT_NE(jasperengine::editor::NativePlugins::xmlFragmentError("<?xml version=\"1.0\"?><a/>"), "");
}

TEST_F(NativeBridgeTest, OsAndNativePausesAreSeparateHolds) {
    int pauses = 0, resumes = 0;
    Engine::onPause.add("NativeBridgeTest", [&pauses]() { pauses++; });
    Engine::onResume.add("NativeBridgeTest", [&resumes]() { resumes++; });
    Engine::pauseGameEvents(false);
    // an ad covers the game, then the app goes to the background and back while it shows
    NativeBridge::pauseEngine();
    NativeBridge::update();
    Engine::systemPause();
    Engine::systemPause();   // idempotent
    Engine::systemResume();
    NativeBridge::update();
    EXPECT_TRUE(Engine::isPaused()) << "the ad still holds the engine";
    EXPECT_EQ(pauses, 1);
    EXPECT_EQ(resumes, 0) << "no onResume while the ad shows";
    NativeBridge::resumeEngine();
    NativeBridge::update();
    EXPECT_FALSE(Engine::isPaused());
    EXPECT_EQ(resumes, 1);
    // the other way round: the OS hold outlives the bridge's
    Engine::systemPause();
    NativeBridge::pauseEngine();
    NativeBridge::update();
    NativeBridge::resumeEngine();
    NativeBridge::update();
    EXPECT_TRUE(Engine::isPaused()) << "the OS still holds it";
    Engine::systemResume();
    EXPECT_FALSE(Engine::isPaused());
    EXPECT_EQ(pauses, 2);
    EXPECT_EQ(resumes, 2);
    Engine::pauseGameEvents(true);
    Engine::onPause.remove("NativeBridgeTest");
    Engine::onResume.remove("NativeBridgeTest");
}

TEST_F(NativeBridgeTest, GameThreadTasksRunWhilePausedAndCanResume) {
    NativeBridge::pauseEngine();
    NativeBridge::update();
    ASSERT_TRUE(Engine::isPaused());
    int ran = 0;
    std::thread([&ran]() {
        NativeBridge::postToGameThread([&ran]() {
            ran++;
            NativeBridge::resumeEngine();
        });
    }).join();
    NativeBridge::update();   // the task runs, then the pause count (0) applies
    EXPECT_EQ(ran, 1);
    EXPECT_FALSE(Engine::isPaused());
    // Play stop drops the queued ones
    NativeBridge::postToGameThread([&ran]() { ran++; });
    NativeBridge::shutdown();
    NativeBridge::update();
    EXPECT_EQ(ran, 1);
}

TEST_F(NativeBridgeTest, EventsOfAStoppedGameAreDropped) {
    int got = 0;
    NativeBridge::emit("Late", "e", nullptr);
    NativeBridge::shutdown();   // Play stop, before the delivery point
    NativeBridge::on("Late", "e", [&got](const NativeJson&) { got++; });
    MainThreadQueue::drain();
    EXPECT_EQ(got, 0) << "an event emitted before the stop does not reach the next Play";
    NativeBridge::emit("Late", "e", nullptr);
    MainThreadQueue::drain();
    EXPECT_EQ(got, 1);
}

TEST_F(NativeBridgeTest, CppValuesAreCheckedForUtf8AndHugeTimeoutsAreClamped) {
    registerEcho();
    NativeError err;
    NativeBridge::call("Echo", "echo", NativeJson::array({std::string("\xff\xfe")}), err);
    EXPECT_EQ(err.code, "invalid_utf8");
    NativeBridge::registerPlugin("Bad").method("text", [](const NativeJson&) { return NativeJson(std::string("a\xc0\x80")); });
    NativeBridge::call("Bad", "text", NativeJson::array(), err);
    EXPECT_EQ(err.code, "invalid_utf8") << "an overlong form";
    // a timeout too long for the clock: no overflow, the call still completes normally
    auto d = std::make_shared<Delivery>();
    NativeBridge::callAsync("Echo", "echo", NativeJson::array({1}), recorder(d), false, {}, 1e300);
    ASSERT_TRUE(waitFor([&] { return d->calls == 1; }));
    EXPECT_EQ(d->code, "");
}
