// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "native/NativeBridge.h"
#include "native/NativePlatform.h"

#include "Engine.h"
#include "Log.h"
#include "thread/MainThreadQueue.h"
#ifndef NO_THREAD_SUPPORT
    #include "jobs/JobSystem.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace jasperengine {

    // One async call. `id`..`background` never change after callAsync; `phase` and `cancelled`
    // are read and written from any thread; the rest belongs to the main thread.
    struct NativeCallState {
        enum class Source { Cpp, Platform, Mock, None };

        uint64_t id = 0;
        std::string plugin;
        std::string method;
        NativeJson args = NativeJson::array();
        Source source = Source::None;
        bool background = false;

        // 0 pending, 1 completed (the result is posted to the main thread), 2 cannot complete
        // any more (cancelled, timed out)
        std::atomic<int> phase{0};
        // what native code sees (NativeCall::isCancelled)
        std::atomic<bool> cancelled{false};

        // main thread
        bool finished = false;      // delivered, cancelled or ended by shutdown: no callback any more
        bool timedOut = false;      // the timeout's delivery is queued
        NativeJson result;
        NativeError error;
        NativeBridge::Completion done;
        AsyncOwner owner;
        bool hasDeadline = false;
        std::chrono::steady_clock::time_point deadline;
    };

    struct NativeBridgeCore {
        // ---- C++ plugins (any thread)
        std::mutex pluginMutex;
        std::unordered_map<std::string, std::shared_ptr<NativePlugin>> plugins;
        // background calls running plugin code: clearCppPlugins waits for them
        std::mutex inFlightMutex;
        std::condition_variable inFlightDone;
        int inFlight = 0;

        // ---- calls
        std::mutex callMutex;
        std::unordered_map<uint64_t, std::shared_ptr<NativeCallState>> pending;   // not finished
        std::atomic<uint64_t> nextCallId{1};

        // ---- events (main thread)
        struct Handler {
            std::string key;          // plugin '\n' event
            NativeBridge::EventHandler fn;
            AsyncOwner owner;
        };
        std::unordered_map<uint64_t, Handler> handlers;
        std::unordered_map<std::string, std::vector<uint64_t>> handlersByKey;   // registration order
        uint64_t nextHandlerId = 1;

        // ---- pause requests
        std::atomic<int> pauseCount{0};
        // game-thread tasks of native code (JasperNative.runOnGameThread): run by update(),
        // paused frames too (resumeEngine from one must work)
        std::mutex taskMutex;
        std::vector<std::function<void()>> gameTasks;
        // incremented by shutdown: an event emitted for a game that stopped is dropped
        std::atomic<uint64_t> generation{0};

        // ---- the platform layer and the mocks (main thread)
        bool platformMade = false;
        NativePlatform* platform = nullptr;
        bool platformOverridden = false;
        NativeMockProvider* mocks = nullptr;

        // ---- messages and stats (main thread)
        std::map<std::string, std::string> platformOnly;
        std::set<std::string> warnedUnavailable;
        // a shipped game (setMockWarnings): a mock answering for a plugin with code for other
        // platforms only is a fake SDK there, said once per plugin
        bool mockWarnings = false;
        std::set<std::string> warnedMocks;
        uint64_t syncCalls = 0, asyncCalls = 0, events = 0;
        int syncCallsThisFrame = 0;
        bool warnedSyncCalls = false;
        bool ownerHooked = false;
        // main thread: a C++ method running at the delivery point (startCpp) completes its call
        // here, delivered right after it returns (the same frame, not the next)
        bool collecting = false;
        struct Completed {
            std::shared_ptr<NativeCallState> state;
            NativeJson value;
            NativeError error;
        };
        std::vector<Completed> collected;

        static NativeBridgeCore& instance(){
            // never destroyed: native threads may complete calls while the process exits
            static NativeBridgeCore* core = new NativeBridgeCore();
            return *core;
        }

        NativePlatform* getPlatform(){
            if (!platformMade && !platformOverridden){
                platformMade = true;
                platform = createNativePlatform();
            }
            return platform;
        }

        void hookOwners(){
            if (ownerHooked) return;
            ownerHooked = true;
            AsyncOwners::onOwnerGone().add("NativeBridge", [](uint64_t token) { NativeBridgeCore::instance().ownerGone(token); });
        }

        std::shared_ptr<NativePlugin> findPlugin(const std::string& name){
            std::lock_guard<std::mutex> lock(pluginMutex);
            auto it = plugins.find(name);
            return it == plugins.end() ? nullptr : it->second;
        }

        // ---- JSON checks

        static bool validUtf8(const std::string& text){
            const unsigned char* s = reinterpret_cast<const unsigned char*>(text.data());
            const size_t n = text.size();
            size_t i = 0;
            while (i < n){
                const unsigned char c = s[i];
                size_t len = 0;
                uint32_t cp = 0;
                if (c < 0x80){ i++; continue; }
                if ((c & 0xE0) == 0xC0){ len = 2; cp = c & 0x1F; }
                else if ((c & 0xF0) == 0xE0){ len = 3; cp = c & 0x0F; }
                else if ((c & 0xF8) == 0xF0){ len = 4; cp = c & 0x07; }
                else return false;
                if (i + len > n) return false;
                for (size_t k = 1; k < len; k++){
                    if ((s[i + k] & 0xC0) != 0x80) return false;
                    cp = (cp << 6) | (s[i + k] & 0x3F);
                }
                // overlong forms, surrogates, past U+10FFFF
                if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
                    (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return false;
                i += len;
            }
            return true;
        }

        // NaN and infinities have no JSON form, strings are UTF-8 (C++ code can build any),
        // nesting is limited (spec: 64). `code`: bad_args/unsupported_type or invalid_utf8.
        static bool checkJson(const NativeJson& value, int depth, std::string& why, bool* utf8 = nullptr){
            if (depth > maxDepth){
                why = "nested deeper than " + std::to_string(maxDepth) + " levels";
                return false;
            }
            if (value.is_number_float() && !std::isfinite(value.get<double>())){
                why = "NaN and infinities have no JSON form";
                return false;
            }
            if (value.is_string() && !validUtf8(value.get_ref<const std::string&>())){
                why = "a string is not valid UTF-8";
                if (utf8) *utf8 = false;
                return false;
            }
            if (value.is_object()){
                for (auto it = value.begin(); it != value.end(); ++it){
                    if (!validUtf8(it.key())){
                        why = "a key is not valid UTF-8";
                        if (utf8) *utf8 = false;
                        return false;
                    }
                    if (!checkJson(it.value(), depth + 1, why, utf8)) return false;
                }
            } else if (value.is_array()){
                for (const auto& item : value){
                    if (!checkJson(item, depth + 1, why, utf8)) return false;
                }
            }
            return true;
        }

        // the error of a value checkJson refused: invalid_utf8, else `code`
        static NativeError jsonError(const std::string& code, const std::string& text, const NativeJson& value){
            std::string why;
            bool utf8 = true;
            checkJson(value, 0, why, &utf8);
            return NativeError(utf8 ? code : "invalid_utf8", text + why);
        }

        static constexpr int maxDepth = 64;

        // ---- resolution

        const char* platformKind(){
            NativePlatform* p = getPlatform();
            if (!p) return nullptr;
            const std::string name = p->name();
            if (name == "Android") return "Java/Kotlin class";
            if (name == "iOS" || name == "macOS") return "Objective-C/Swift class";
            if (name == "Web") return "JavaScript object";
            return "platform plugin";
        }

        void noteMockAnswer(const std::string& plugin){
            if (!mockWarnings) return;
            auto hint = platformOnly.find(plugin);
            if (hint == platformOnly.end() || !warnedMocks.insert(plugin).second) return;
            Log::warn("Native plugin \"%s\" is answered by its Lua mock in this build: %s", plugin.c_str(), hint->second.c_str());
        }

        NativeError notFound(const std::string& plugin){
            const char* platformName = "this platform";
            switch (Engine::getPlatform()){
                case Platform::MacOS: platformName = "macOS"; break;
                case Platform::iOS: platformName = "iOS"; break;
                case Platform::Web: platformName = "Web"; break;
                case Platform::Android: platformName = "Android"; break;
                case Platform::Linux: platformName = "Linux"; break;
                case Platform::Windows: platformName = "Windows"; break;
            }
            auto hint = platformOnly.find(plugin);
            if (hint != platformOnly.end()){
                const std::string message = "native plugin \"" + plugin + "\" has no implementation here (" + platformName + "): " +
                    hint->second + "; add native_mocks/" + plugin + ".lua to the Lua folder or call Native.setMock";
                if (warnedUnavailable.insert(plugin).second){
                    Log::warn("Native plugin \"%s\" has no implementation here (%s): %s. Add native_mocks/%s.lua to the Lua folder or call Native.setMock",
                              plugin.c_str(), platformName, hint->second.c_str(), plugin.c_str());
                }
                return NativeError("unavailable", message);
            }
            const char* kind = platformKind();
            return NativeError("not_found", "no native plugin \"" + plugin + "\" here (no C++ plugin, " +
                               std::string(kind ? kind : "platform plugin") + " or Lua mock of that name)");
        }

        void countSyncCall(){
            syncCalls++;
            if (++syncCallsThisFrame > 200 && !warnedSyncCalls){
                warnedSyncCalls = true;
                Log::warn("Native: more than 200 Native.call in one frame; each one converts JSON (and crosses JNI on Android): "
                          "batch the work into fewer calls");
            }
        }

        NativeJson callCpp(const std::shared_ptr<NativePlugin>& plugin, const std::string& method, const NativeJson& args, NativeError& err){
            auto it = plugin->methods.find(method);
            if (it == plugin->methods.end()){
                err = NativeError("no_method", "native plugin \"" + plugin->getName() + "\" (C++) has no method \"" + method + "\"");
                return nullptr;
            }
            if (!it->second.sync){
                err = NativeError("async_only", plugin->getName() + "." + method + " is asynchronous: use Native.callAsync");
                return nullptr;
            }
            NativeJson result;
            const NativePlugin::Sync fn = it->second.sync;
            if (!runSync(fn, plugin->getName(), method, args, result, err)) return nullptr;
            return result;
        }

        // a C++ sync method; false with err on failure
        static bool runSync(const NativePlugin::Sync& fn, const std::string& plugin, const std::string& method,
                            const NativeJson& args, NativeJson& result, NativeError& err){
            try {
                result = fn(args);
            } catch (const NativeError& e) {
                err = e.code.empty() ? NativeError("exception", e.message) : e;
                return false;
            } catch (const std::exception& e) {
                err = NativeError("exception", plugin + "." + method + " threw: " + e.what());
                Log::error("Native: %s.%s threw: %s", plugin.c_str(), method.c_str(), e.what());
                return false;
            } catch (...) {
                err = NativeError("exception", plugin + "." + method + " threw an unknown exception");
                Log::error("Native: %s.%s threw an unknown exception", plugin.c_str(), method.c_str());
                return false;
            }
            std::string why;
            if (!checkJson(result, 0, why)){
                err = jsonError("unsupported_type", plugin + "." + method + " returned a value JSON cannot hold: ", result);
                result = nullptr;
                return false;
            }
            return true;
        }

        // ---- async completion

        // any thread
        bool complete(const std::shared_ptr<NativeCallState>& state, NativeJson value, const NativeError* error){
            int expected = 0;
            if (!state->phase.compare_exchange_strong(expected, 1)){
                if (expected == 1){
                    Log::warn("Native: %s.%s (call %llu) was completed twice; the second result is ignored",
                              state->plugin.c_str(), state->method.c_str(), (unsigned long long)state->id);
                }
                return false;
            }
            NativeError err = error ? *error : NativeError();
            if (err.isSet()){
                value = nullptr;
            } else {
                std::string why;
                if (!checkJson(value, 0, why)){
                    err = jsonError("unsupported_type", state->plugin + "." + state->method + " completed with a value JSON cannot hold: ", value);
                    value = nullptr;
                }
            }
            if (MainThreadQueue::isMainThread() && collecting){
                collected.push_back({state, std::move(value), std::move(err)});
                return true;
            }
            MainThreadQueue::post([state, value = std::move(value), err = std::move(err)]() mutable {
                NativeBridgeCore::instance().deliver(state, std::move(value), std::move(err));
            });
            return true;
        }

        // runs `fn` on the main thread at the delivery point; what it completes is delivered
        // when it returns
        template <typename Fn>
        void runCollecting(Fn&& fn){
            const bool outer = collecting;
            collecting = true;
            fn();
            collecting = outer;
            if (outer) return;
            std::vector<Completed> done;
            done.swap(collected);
            for (Completed& c : done) deliver(c.state, std::move(c.value), std::move(c.error));
        }

        // main thread: the callback
        void deliver(const std::shared_ptr<NativeCallState>& state, NativeJson value, NativeError err){
            if (state->finished) return;
            state->finished = true;
            {
                std::lock_guard<std::mutex> lock(callMutex);
                pending.erase(state->id);
            }
            state->result = std::move(value);
            state->error = std::move(err);
            NativeBridge::Completion done = std::move(state->done);
            state->done = nullptr;
            if (done) done(state->result, state->error.isSet() ? &state->error : nullptr);
        }

        // main thread: no callback; native code sees isCancelled()
        void cancel(const std::shared_ptr<NativeCallState>& state){
            if (state->finished) return;
            state->finished = true;
            const bool wasPending = endNative(state);
            {
                std::lock_guard<std::mutex> lock(callMutex);
                pending.erase(state->id);
            }
            state->done = nullptr;
            // what the handle shows (Lua: call.errorCode)
            state->error = NativeError("cancelled", state->plugin + "." + state->method + " was cancelled");
            if (wasPending && state->source == NativeCallState::Source::Platform && platform){
                platform->cancel(state->id);
            }
        }

        // native side: the call cannot complete any more; true when it was still pending there
        static bool endNative(const std::shared_ptr<NativeCallState>& state){
            state->cancelled.store(true);
            int expected = 0;
            return state->phase.compare_exchange_strong(expected, 2);
        }

        void ownerGone(uint64_t token){
            if (!MainThreadQueue::isMainThread()){
                MainThreadQueue::post([token]() { NativeBridgeCore::instance().ownerGone(token); });
                return;
            }
            std::vector<std::shared_ptr<NativeCallState>> doomed;
            {
                std::lock_guard<std::mutex> lock(callMutex);
                for (const auto& [id, state] : pending){
                    if (state->owner.token == token) doomed.push_back(state);
                }
            }
            for (const auto& state : doomed) cancel(state);

            std::vector<uint64_t> handlerIds;
            for (const auto& [id, handler] : handlers){
                if (handler.owner.token == token) handlerIds.push_back(id);
            }
            for (uint64_t id : handlerIds) removeHandler(id);
        }

        // ---- events (main thread)

        void dispatch(const std::string& key, const NativeJson& payload){
            auto it = handlersByKey.find(key);
            if (it == handlersByKey.end()) return;
            // handlers may subscribe or unsubscribe while this runs
            const std::vector<uint64_t> ids = it->second;
            for (uint64_t id : ids){
                auto handler = handlers.find(id);
                if (handler == handlers.end()) continue;
                if (!AsyncOwners::alive(handler->second.owner)){
                    removeHandler(id);
                    continue;
                }
                const NativeBridge::EventHandler fn = handler->second.fn;
                events++;
                if (fn) fn(payload);
            }
        }

        void removeHandler(uint64_t id){
            auto it = handlers.find(id);
            if (it == handlers.end()) return;
            auto byKey = handlersByKey.find(it->second.key);
            if (byKey != handlersByKey.end()){
                auto& ids = byKey->second;
                ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
                if (ids.empty()) handlersByKey.erase(byKey);
            }
            // the handler (a Lua function reference) goes on the main thread
            NativeBridge::EventHandler fn = std::move(it->second.fn);
            handlers.erase(it);
        }

        // ---- background work

        void runInBackground(std::function<void()> task){
#ifndef NO_THREAD_SUPPORT
            {
                std::lock_guard<std::mutex> lock(inFlightMutex);
                inFlight++;
            }
            auto wrapped = [task = std::move(task)]() mutable {
                task();
                // what the task held (plugin references) goes before clearCppPlugins sees zero
                task = nullptr;
                NativeBridgeCore& core = NativeBridgeCore::instance();
                std::lock_guard<std::mutex> lock(core.inFlightMutex);
                core.inFlight--;
                core.inFlightDone.notify_all();
            };
            try {
                jobs::enqueueDetached(std::move(wrapped), jobs::Lane::Background, "native-call");
                return;
            } catch (const std::exception& e) {
                Log::warn("Native: no worker thread (%s); the background call runs on the main thread", e.what());
            }
            {
                std::lock_guard<std::mutex> lock(inFlightMutex);
                inFlight--;
            }
#endif
            MainThreadQueue::post(std::move(task));
        }

        // a C++ plugin's method of an async call, run where the call asked for (looked up by
        // name when it runs: the plugin may be gone by then)
        void startCpp(const std::shared_ptr<NativeCallState>& state){
            auto run = [state]() {
                NativeBridgeCore& core = NativeBridgeCore::instance();
                if (state->cancelled.load()) return;
                std::shared_ptr<NativePlugin> plugin = core.findPlugin(state->plugin);
                if (!plugin){
                    const NativeError err("not_found", "native plugin \"" + state->plugin + "\" (C++) was unregistered");
                    core.complete(state, nullptr, &err);
                    return;
                }
                auto it = plugin->methods.find(state->method);
                if (it == plugin->methods.end()){
                    const NativeError err("no_method", "native plugin \"" + state->plugin + "\" (C++) has no method \"" + state->method + "\"");
                    core.complete(state, nullptr, &err);
                    return;
                }
                if (it->second.async){
                    const NativePlugin::Async fn = it->second.async;
                    try {
                        fn(NativeCall(state));
                    } catch (const NativeError& e) {
                        const NativeError err = e.code.empty() ? NativeError("exception", e.message) : e;
                        core.complete(state, nullptr, &err);
                    } catch (const std::exception& e) {
                        Log::error("Native: %s.%s threw: %s", state->plugin.c_str(), state->method.c_str(), e.what());
                        const NativeError err("exception", state->plugin + "." + state->method + " threw: " + e.what());
                        core.complete(state, nullptr, &err);
                    } catch (...) {
                        const NativeError err("exception", state->plugin + "." + state->method + " threw an unknown exception");
                        core.complete(state, nullptr, &err);
                    }
                    return;
                }
                NativeJson result;
                NativeError err;
                if (runSync(it->second.sync, state->plugin, state->method, state->args, result, err)){
                    core.complete(state, std::move(result), nullptr);
                } else {
                    core.complete(state, nullptr, &err);
                }
            };
            if (state->background){
                runInBackground(std::move(run));
            } else {
                MainThreadQueue::post([run = std::move(run)]() { NativeBridgeCore::instance().runCollecting(run); });
            }
        }
    };

}

// =============================================================================================
// NativeCall

namespace {
    const std::string& emptyString(){
        static const std::string* empty = new std::string();
        return *empty;
    }
    const NativeJson& nullJson(){
        static const NativeJson* value = new NativeJson();
        return *value;
    }
    const NativeJson& emptyArray(){
        static const NativeJson* value = new NativeJson(NativeJson::array());
        return *value;
    }
    const NativeError& noError(){
        static const NativeError* error = new NativeError();
        return *error;
    }
}

uint64_t NativeCall::id() const{ return state ? state->id : 0; }
const std::string& NativeCall::plugin() const{ return state ? state->plugin : emptyString(); }
const std::string& NativeCall::method() const{ return state ? state->method : emptyString(); }
const NativeJson& NativeCall::args() const{ return state ? state->args : emptyArray(); }

bool NativeCall::resolve(NativeJson value) const{
    if (!state) return false;
    return NativeBridgeCore::instance().complete(state, std::move(value), nullptr);
}

bool NativeCall::reject(const std::string& code, const std::string& message) const{
    if (!state) return false;
    const NativeError err(code.empty() ? "exception" : code, message);
    return NativeBridgeCore::instance().complete(state, nullptr, &err);
}

bool NativeCall::isCancelled() const{ return !state || state->cancelled.load(); }
bool NativeCall::isDone() const{ return !state || state->phase.load() != 0; }
bool NativeCall::isDelivered() const{ return !state || state->finished; }
const NativeJson& NativeCall::result() const{ return state && state->finished ? state->result : nullJson(); }
const NativeError& NativeCall::error() const{ return state && state->finished ? state->error : noError(); }

void NativeCall::cancel() const{
    if (!state) return;
    if (!MainThreadQueue::isMainThread()){
        std::shared_ptr<NativeCallState> s = state;
        MainThreadQueue::post([s]() { NativeBridgeCore::instance().cancel(s); });
        return;
    }
    NativeBridgeCore::instance().cancel(state);
}

// =============================================================================================
// NativePlugin

NativePlugin::NativePlugin(std::string name): name(std::move(name)) {}

NativePlugin& NativePlugin::method(const std::string& methodName, Sync fn){
    Method& m = methods[methodName];
    m.sync = std::move(fn);
    m.async = nullptr;
    return *this;
}

NativePlugin& NativePlugin::asyncMethod(const std::string& methodName, Async fn){
    Method& m = methods[methodName];
    m.async = std::move(fn);
    m.sync = nullptr;
    return *this;
}

bool NativePlugin::has(const std::string& methodName) const{
    return methods.count(methodName) != 0;
}

bool NativePlugin::isAsync(const std::string& methodName) const{
    auto it = methods.find(methodName);
    return it != methods.end() && it->second.async != nullptr;
}

std::vector<std::string> NativePlugin::getMethodNames() const{
    std::vector<std::string> names;
    for (const auto& [methodName, m] : methods) names.push_back(methodName);
    return names;
}

NativePluginRegistrar::NativePluginRegistrar(const char* name, void (*setup)(NativePlugin& plugin)){
    NativePlugin& plugin = NativeBridge::registerPlugin(name ? name : "");
    if (setup) setup(plugin);
}

// =============================================================================================
// NativeBridge

NativePlugin& NativeBridge::registerPlugin(const std::string& name){
    NativeBridgeCore& core = NativeBridgeCore::instance();
    auto plugin = std::make_shared<NativePlugin>(name);
    NativePlugin& ref = *plugin;
    std::shared_ptr<NativePlugin> old;
    {
        std::lock_guard<std::mutex> lock(core.pluginMutex);
        auto& slot = core.plugins[name];
        old = std::move(slot);
        slot = std::move(plugin);
    }
    return ref;
}

void NativeBridge::unregisterPlugin(const std::string& name){
    NativeBridgeCore& core = NativeBridgeCore::instance();
    std::shared_ptr<NativePlugin> old;
    std::lock_guard<std::mutex> lock(core.pluginMutex);
    auto it = core.plugins.find(name);
    if (it == core.plugins.end()) return;
    old = std::move(it->second);
    core.plugins.erase(it);
}

bool NativeBridge::hasCppPlugin(const std::string& name){
    return NativeBridgeCore::instance().findPlugin(name) != nullptr;
}

void NativeBridge::clearCppPlugins(){
    NativeBridgeCore& core = NativeBridgeCore::instance();
    std::unordered_map<std::string, std::shared_ptr<NativePlugin>> old;
    {
        std::lock_guard<std::mutex> lock(core.pluginMutex);
        old.swap(core.plugins);
    }
    {
        // background calls run plugin code: the library must outlive them
        std::unique_lock<std::mutex> lock(core.inFlightMutex);
        if (!core.inFlightDone.wait_for(lock, std::chrono::seconds(10), [&core]() { return core.inFlight == 0; })){
            Log::warn("Native: %d background C++ plugin calls still run after 10 s", core.inFlight);
        }
    }
    // the plugins' functions go here, while their code is loaded
    old.clear();
}

NativeJson NativeBridge::call(const std::string& plugin, const std::string& method, const NativeJson& args, NativeError& err){
    NativeBridgeCore& core = NativeBridgeCore::instance();
    err = NativeError();
    core.countSyncCall();

    std::string why;
    if (!args.is_array()){
        err = NativeError("bad_args", "the arguments must be a JSON array");
        return nullptr;
    }
    if (!NativeBridgeCore::checkJson(args, 0, why)){
        err = NativeBridgeCore::jsonError("bad_args", "the arguments of " + plugin + "." + method + " cannot be sent: ", args);
        return nullptr;
    }

    if (std::shared_ptr<NativePlugin> cpp = core.findPlugin(plugin)){
        return core.callCpp(cpp, method, args, err);
    }
    if (NativePlatform* platform = core.getPlatform()){
        if (platform->has(plugin, "")){
            NativeJson result = platform->call(plugin, method, args, err);
            if (err.isSet()) return nullptr;
            if (!NativeBridgeCore::checkJson(result, 0, why)){
                err = NativeBridgeCore::jsonError("unsupported_type", plugin + "." + method + " returned a value JSON cannot hold: ", result);
                return nullptr;
            }
            return result;
        }
    }
    if (core.mocks && core.mocks->has(plugin, "")){
        core.noteMockAnswer(plugin);
        NativeJson result = core.mocks->call(plugin, method, args, err);
        if (err.isSet()) return nullptr;
        return result;
    }
    err = core.notFound(plugin);
    return nullptr;
}

NativeCall NativeBridge::callAsync(const std::string& plugin, const std::string& method, NativeJson args,
                                   Completion done, bool background, AsyncOwner owner, double timeout){
    NativeBridgeCore& core = NativeBridgeCore::instance();
    core.hookOwners();
    core.asyncCalls++;

    auto state = std::make_shared<NativeCallState>();
    state->id = core.nextCallId.fetch_add(1);
    state->plugin = plugin;
    state->method = method;
    state->background = background;
    state->done = std::move(done);
    state->owner = owner;
    if (timeout > 0.0 && std::isfinite(timeout)){
        // a year at most: longer is "none" in practice, and the conversion cannot overflow
        timeout = std::min(timeout, 365.0 * 24.0 * 3600.0);
        state->hasDeadline = true;
        state->deadline = std::chrono::steady_clock::now() +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(timeout));
    }
    NativeCall call(state);

    // an owner already gone: nothing runs
    if (!AsyncOwners::alive(owner)){
        NativeBridgeCore::endNative(state);
        state->finished = true;
        state->done = nullptr;
        return call;
    }
    {
        std::lock_guard<std::mutex> lock(core.callMutex);
        core.pending[state->id] = state;
    }

    std::string why;
    if (!args.is_array()){
        const NativeError err("bad_args", "the arguments must be a JSON array");
        core.complete(state, nullptr, &err);
        return call;
    }
    if (!NativeBridgeCore::checkJson(args, 0, why)){
        const NativeError err = NativeBridgeCore::jsonError("bad_args", "the arguments of " + plugin + "." + method + " cannot be sent: ", args);
        core.complete(state, nullptr, &err);
        return call;
    }
    state->args = std::move(args);

    if (core.findPlugin(plugin)){
        state->source = NativeCallState::Source::Cpp;
        core.startCpp(state);
        return call;
    }
    if (NativePlatform* platform = core.getPlatform()){
        if (platform->has(plugin, "")){
            state->source = NativeCallState::Source::Platform;
            platform->callAsync(state->id, plugin, method, state->args, background);
            return call;
        }
    }
    if (core.mocks && core.mocks->has(plugin, "")){
        core.noteMockAnswer(plugin);
        state->source = NativeCallState::Source::Mock;
        core.mocks->callAsync(call);
        return call;
    }
    const NativeError err = core.notFound(plugin);
    core.complete(state, nullptr, &err);
    return call;
}

bool NativeBridge::has(const std::string& plugin, const std::string& method){
    NativeBridgeCore& core = NativeBridgeCore::instance();
    if (std::shared_ptr<NativePlugin> cpp = core.findPlugin(plugin)){
        return method.empty() || cpp->has(method);
    }
    if (NativePlatform* platform = core.getPlatform()){
        if (platform->has(plugin, "")) return method.empty() || platform->has(plugin, method);
    }
    if (core.mocks && core.mocks->has(plugin, "")){
        return method.empty() || core.mocks->has(plugin, method);
    }
    return false;
}

void NativeBridge::emit(const std::string& plugin, const std::string& event, NativeJson payload){
    std::string key = plugin + "\n" + event;
    const uint64_t generation = NativeBridgeCore::instance().generation.load();
    MainThreadQueue::post([key = std::move(key), payload = std::move(payload), generation]() {
        NativeBridgeCore& core = NativeBridgeCore::instance();
        if (core.generation.load() != generation) return;
        core.dispatch(key, payload);
    });
}

uint64_t NativeBridge::on(const std::string& plugin, const std::string& event, EventHandler handler, AsyncOwner owner){
    NativeBridgeCore& core = NativeBridgeCore::instance();
    core.hookOwners();
    const uint64_t id = core.nextHandlerId++;
    // an owner already gone: the id is valid (off is a no-op) and nothing runs
    if (!AsyncOwners::alive(owner)) return id;
    NativeBridgeCore::Handler entry;
    entry.key = plugin + "\n" + event;
    entry.fn = std::move(handler);
    entry.owner = owner;
    core.handlersByKey[entry.key].push_back(id);
    core.handlers.emplace(id, std::move(entry));
    return id;
}

void NativeBridge::off(uint64_t handlerId){
    NativeBridgeCore::instance().removeHandler(handlerId);
}

void NativeBridge::pauseEngine(){
    NativeBridgeCore::instance().pauseCount.fetch_add(1);
}

void NativeBridge::resumeEngine(){
    NativeBridgeCore& core = NativeBridgeCore::instance();
    int count = core.pauseCount.load();
    while (count > 0 && !core.pauseCount.compare_exchange_weak(count, count - 1)) {}
    if (count <= 0){
        Log::warn("Native: resumeEngine without a pauseEngine before it is ignored");
    }
}

void NativeBridge::postToGameThread(std::function<void()> task){
    if (!task) return;
    NativeBridgeCore& core = NativeBridgeCore::instance();
    std::lock_guard<std::mutex> lock(core.taskMutex);
    core.gameTasks.push_back(std::move(task));
}

void NativeBridge::initPlugins(){
    NativeBridgeCore& core = NativeBridgeCore::instance();
    if (NativePlatform* platform = core.getPlatform()) platform->initPlugins();
}

int NativeBridge::getPauseCount(){
    return NativeBridgeCore::instance().pauseCount.load();
}

void NativeBridge::completeFromPlatform(uint64_t callId, NativeJson result, const NativeError* error){
    NativeBridgeCore& core = NativeBridgeCore::instance();
    std::shared_ptr<NativeCallState> state;
    {
        std::lock_guard<std::mutex> lock(core.callMutex);
        auto it = core.pending.find(callId);
        if (it != core.pending.end()) state = it->second;
    }
    // cancelled, timed out or never started here: nobody waits for it
    if (!state) return;
    NativeError err;
    if (error && error->isSet()) err = *error;
    core.complete(state, std::move(result), err.isSet() ? &err : nullptr);
}

void NativeBridge::setPlatformOnlyPlugins(std::map<std::string, std::string> plugins){
    NativeBridgeCore& core = NativeBridgeCore::instance();
    core.platformOnly = std::move(plugins);
    core.warnedUnavailable.clear();
    core.warnedMocks.clear();
}

void NativeBridge::setMockWarnings(bool on){
    NativeBridgeCore::instance().mockWarnings = on;
}

void NativeBridge::update(){
    NativeBridgeCore& core = NativeBridgeCore::instance();
    core.syncCallsThisFrame = 0;

    // native code's game-thread tasks (paused frames too)
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(core.taskMutex);
        tasks.swap(core.gameTasks);
    }
    for (std::function<void()>& task : tasks){
        try {
            task();
        } catch (const std::exception& e) {
            Log::error("Native: a game-thread task failed: %s", e.what());
        } catch (...) {
            Log::error("Native: a game-thread task failed");
        }
    }

    // full-screen native views (ads) hold the engine paused, next to the OS's own pause
    Engine::setNativePause(core.pauseCount.load() > 0);

    // timeouts: delivered at the delivery point, like any result
    std::vector<std::shared_ptr<NativeCallState>> expired;
    {
        std::lock_guard<std::mutex> lock(core.callMutex);
        if (core.pending.empty()) return;
        const auto now = std::chrono::steady_clock::now();
        for (const auto& [id, state] : core.pending){
            if (state->hasDeadline && !state->timedOut && now >= state->deadline) expired.push_back(state);
        }
    }
    for (const auto& state : expired){
        state->timedOut = true;
        const bool wasPending = NativeBridgeCore::endNative(state);
        if (wasPending && state->source == NativeCallState::Source::Platform && core.platform){
            core.platform->cancel(state->id);
        }
        // a result that came just before the deadline keeps its delivery (it is queued)
        if (!wasPending) continue;
        NativeError err("timeout", state->plugin + "." + state->method + " did not complete within its timeout");
        MainThreadQueue::post([state, err = std::move(err)]() mutable {
            NativeBridgeCore::instance().deliver(state, nullptr, std::move(err));
        });
    }
}

void NativeBridge::shutdown(){
    NativeBridgeCore& core = NativeBridgeCore::instance();
    std::vector<std::shared_ptr<NativeCallState>> calls;
    {
        std::lock_guard<std::mutex> lock(core.callMutex);
        for (const auto& [id, state] : core.pending) calls.push_back(state);
    }
    for (const auto& state : calls) core.cancel(state);
    if (core.platform) core.platform->shutdown();

    std::vector<uint64_t> handlerIds;
    for (const auto& [id, handler] : core.handlers) handlerIds.push_back(id);
    for (uint64_t id : handlerIds) core.removeHandler(id);

    // a pause the game's native code held ends with the game, its queued tasks and events too
    core.pauseCount.store(0);
    Engine::setNativePause(false);
    {
        std::lock_guard<std::mutex> lock(core.taskMutex);
        core.gameTasks.clear();
    }
    core.generation.fetch_add(1);

    if (core.mocks) core.mocks->reset();
    core.warnedUnavailable.clear();
    core.warnedMocks.clear();
    core.syncCalls = core.asyncCalls = core.events = 0;
    core.syncCallsThisFrame = 0;
}

NativeBridge::Stats NativeBridge::getStats(){
    NativeBridgeCore& core = NativeBridgeCore::instance();
    Stats stats;
    {
        std::lock_guard<std::mutex> lock(core.callMutex);
        stats.pendingCalls = core.pending.size();
    }
    {
        std::lock_guard<std::mutex> lock(core.pluginMutex);
        stats.cppPlugins = core.plugins.size();
    }
    stats.handlers = core.handlers.size();
    stats.syncCalls = core.syncCalls;
    stats.asyncCalls = core.asyncCalls;
    stats.events = core.events;
    return stats;
}

// =============================================================================================
// internal

void NativeBridgeInternal::setMockProvider(NativeMockProvider* provider){
    NativeBridgeCore::instance().mocks = provider;
}

NativeCall NativeBridgeInternal::failedCall(const std::string& plugin, const std::string& method, NativeBridge::Completion done,
                                            AsyncOwner owner, const NativeError& error){
    // a call that starts nothing: the plugin is not resolved, `done` gets the error next frame
    NativeBridgeCore& core = NativeBridgeCore::instance();
    core.hookOwners();
    auto state = std::make_shared<NativeCallState>();
    state->id = core.nextCallId.fetch_add(1);
    state->plugin = plugin;
    state->method = method;
    state->done = std::move(done);
    state->owner = owner;
    NativeCall call(state);
    if (!AsyncOwners::alive(owner)){
        NativeBridgeCore::endNative(state);
        state->finished = true;
        state->done = nullptr;
        return call;
    }
    {
        std::lock_guard<std::mutex> lock(core.callMutex);
        core.pending[state->id] = state;
    }
    core.complete(state, nullptr, &error);
    return call;
}

void NativeBridgeInternal::setPlatformForTests(NativePlatform* platform){
    NativeBridgeCore& core = NativeBridgeCore::instance();
    core.platform = platform;
    core.platformOverridden = platform != nullptr;
    core.platformMade = platform != nullptr;
}
