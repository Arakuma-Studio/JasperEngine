// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_NATIVEBRIDGE_H
#define JASPERENGINE_NATIVEBRIDGE_H

#include "Export.h"
#include "json.hpp"
#include "script/AsyncOwner.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    // The native bridge (Lua: the `Native` class): games call native code of the platform they
    // run on by a plugin name and a method name, with JSON values as arguments and results.
    // A plugin is, in this order (the first that exists answers, everywhere):
    //   1. a C++ plugin registered here (JASPERENGINE_NATIVE_PLUGIN in plugins/native/cpp or any
    //      C++ code; every platform),
    //   2. a platform plugin: a Java/Kotlin class (Android), an Objective-C/Swift class (iOS,
    //      macOS), a JavaScript object (Web), reached by reflection (core/native/NativePlatform.h),
    //   3. a Lua mock (Native.setMock or <luaDir>/native_mocks/<Plugin>.lua),
    // else the call fails with "not_found" (or "unavailable" when the plugin exists for another
    // platform only). Every callback and event reaches the game on the main thread, at the
    // MainThreadQueue delivery point (right before Engine::onUpdate). Errors are values.
    // The guide: docs/mcp/guide/native-bridge.md.

    // A JSON value (engine/libs/json): null, boolean, number (integers stay integers), string,
    // array, object
    using NativeJson = nlohmann::json;

    // Error codes (NativeError::code): not_found, no_method, bad_args, exception,
    // unsupported_type, invalid_utf8, cancelled, timeout, async_only, not_ready, unavailable.
    // A C++ plugin method throws one to fail: throw NativeError("bad_args", "level must be > 0").
    struct NativeError {
        std::string code;
        std::string message;

        NativeError() = default;
        NativeError(std::string code, std::string message): code(std::move(code)), message(std::move(message)) {}
        bool isSet() const { return !code.empty(); }
    };

    struct NativeCallState;

    // One asynchronous call: what an async method receives (and Native.callAsync returns to
    // Lua). A copyable handle on shared state. The native side completes it once, from any
    // thread, at any time: the first resolve/reject wins, later ones are ignored with a
    // warning. The result reaches the caller's callback in the next frame, on the main thread.
    class JASPERENGINE_API NativeCall {
    public:
        NativeCall() = default;

        bool isValid() const { return state != nullptr; }
        // unique per process, from 1
        uint64_t id() const;
        const std::string& plugin() const;
        const std::string& method() const;
        // the call's arguments: a JSON array
        const NativeJson& args() const;

        // Any thread. False when the call was already completed, cancelled or timed out.
        bool resolve(NativeJson value) const;
        bool reject(const std::string& code, const std::string& message) const;

        // The caller cancelled it, its owner went, it timed out or Play stopped: the result
        // will not be used (native code may stop its work)
        bool isCancelled() const;
        // resolved, rejected or cancelled (native side view: complete() was called or no
        // longer can be)
        bool isDone() const;

        // Main thread, the caller's side
        // The callback ran (or the call was cancelled): result() and error() are final
        bool isDelivered() const;
        // the result once delivered (null otherwise or on failure)
        const NativeJson& result() const;
        // the error once delivered ("" code when it succeeded)
        const NativeError& error() const;
        // Idempotent: the callback will not run; native code sees isCancelled()
        void cancel() const;

        explicit NativeCall(std::shared_ptr<NativeCallState> state): state(std::move(state)) {}
        const std::shared_ptr<NativeCallState>& getState() const { return state; }

    private:
        std::shared_ptr<NativeCallState> state;
    };

    // A C++ plugin: named methods. Sync methods get the arguments (a JSON array) and return a
    // JSON value (throw NativeError to fail; any other exception becomes "exception"). Async
    // methods get the NativeCall and complete it later, from any thread.
    class JASPERENGINE_API NativePlugin {
    public:
        using Sync = std::function<NativeJson(const NativeJson& args)>;
        using Async = std::function<void(NativeCall call)>;

        explicit NativePlugin(std::string name);

        // a method of that name replaces the earlier one (sync or async)
        NativePlugin& method(const std::string& name, Sync fn);
        NativePlugin& asyncMethod(const std::string& name, Async fn);

        const std::string& getName() const { return name; }
        bool has(const std::string& methodName) const;
        bool isAsync(const std::string& methodName) const;
        std::vector<std::string> getMethodNames() const;

    private:
        friend struct NativeBridgeCore;
        struct Method {
            Sync sync;
            Async async;
        };
        std::string name;
        std::map<std::string, Method> methods;
    };

    class JASPERENGINE_API NativeBridge {
    public:
        using Completion = std::function<void(const NativeJson& result, const NativeError* error)>;
        using EventHandler = std::function<void(const NativeJson& payload)>;

        // ---- C++ plugins (any thread; usually static initializers)

        // A new empty plugin under `name`, replacing one registered before
        static NativePlugin& registerPlugin(const std::string& name);
        static void unregisterPlugin(const std::string& name);
        static bool hasCppPlugin(const std::string& name);
        // Every C++ plugin goes (the editor, before the Play library unloads: their functions
        // are its code). Waits for the background calls that run plugin code.
        static void clearCppPlugins();

        // ---- calls (the main thread)

        // Sync: the result, or null with `err` set
        static NativeJson call(const std::string& plugin, const std::string& method, const NativeJson& args, NativeError& err);
        // Async: `done` runs once on the main thread at the delivery point (not when cancelled
        // or the owner went first). `background`: the method runs on a worker (Android/Apple/C++
        // thread pools) instead of the platform's UI thread (C++: the next frame on the main
        // thread). timeout > 0: after that many seconds (real time) the call fails with
        // "timeout" and native code sees isCancelled().
        static NativeCall callAsync(const std::string& plugin, const std::string& method, NativeJson args,
                                    Completion done, bool background = false, AsyncOwner owner = {}, double timeout = 0.0);
        // An implementation of the plugin exists (C++, platform or mock); with a method name,
        // that method exists
        static bool has(const std::string& plugin, const std::string& method = "");

        // ---- events (native -> game)

        // Any thread: the handlers of (plugin, event) run in the next frame on the main
        // thread, in registration order. Emitted while the engine is paused: delivered after
        // the resume, in order.
        static void emit(const std::string& plugin, const std::string& event, NativeJson payload);
        // Main thread. An id > 0; with an owner the handler goes with it.
        static uint64_t on(const std::string& plugin, const std::string& event, EventHandler handler, AsyncOwner owner = {});
        // unknown handlerId: no-op
        static void off(uint64_t handlerId);

        // ---- engine pause while a full-screen native view is shown (ads); any thread

        // Counted: nested pauses need as many resumes. Applied at the start of the next frame
        // (Engine::systemPause / systemResume: audio paused, Engine.onPause / onResume fire).
        static void pauseEngine();
        static void resumeEngine();
        static int getPauseCount();

        // Any thread: `task` runs on the engine thread at the start of the next frame (paused
        // frames too: a resumeEngine from it works), before the delivery point. Play stop drops
        // the queued ones.
        static void postToGameThread(std::function<void()> task);

        // The platform plugins' init hooks (Apple +jasperInit of classes not run yet): app start
        // (Engine::systemInit) and editor Play start (code the Play library just loaded)
        static void initPlugins();

        // ---- the platform layer (any thread)

        // A platform call started by callAsync ended (id = NativeCall::id()); an unknown or
        // cancelled id is ignored. error null or with an empty code: success.
        static void completeFromPlatform(uint64_t callId, NativeJson result, const NativeError* error);

        // ---- editor / builds

        // Plugin names implemented for other platforms only (from plugins/native) -> what to
        // say about them ("Java plugins run only on Android"): a call here fails with
        // "unavailable" instead of "not_found", and logs once per plugin
        static void setPlatformOnlyPlugins(std::map<std::string, std::string> plugins);
        // Shipped games (the generated main of a Compile build, the prebuilt player; never the
        // editor's Play): a Lua mock answering for a plugin of setPlatformOnlyPlugins logs one
        // warning per plugin, since it stands in for an SDK the platform does not have
        static void setMockWarnings(bool on);

        // Engine::runFrame, every frame (paused too), the main thread: pause requests and
        // timeouts
        static void update();
        // Play stop / engine shutdown: pending calls end without their callbacks (native sees
        // isCancelled), handlers go, the pause count resets, mocks are dropped. C++ plugins stay
        // (clearCppPlugins drops them).
        static void shutdown();

        struct Stats {
            size_t pendingCalls = 0;
            size_t handlers = 0;
            size_t cppPlugins = 0;
            uint64_t syncCalls = 0;      // since the last shutdown
            uint64_t asyncCalls = 0;
            uint64_t events = 0;         // delivered
        };
        static Stats getStats();
    };

    // Registers a C++ plugin at static initialization (JASPERENGINE_NATIVE_PLUGIN)
    struct JASPERENGINE_API NativePluginRegistrar {
        NativePluginRegistrar(const char* name, void (*setup)(NativePlugin& plugin));
    };

}

// A C++ plugin named NAME, registered when its code loads (every export, and the editor's Play
// library for plugins/native/cpp):
//   JASPERENGINE_NATIVE_PLUGIN(Steam) {
//       plugin.method("userName", [](const jasperengine::NativeJson& args) { return jasperengine::NativeJson("me"); });
//       plugin.asyncMethod("login", [](jasperengine::NativeCall call) { call.resolve({{"ok", true}}); });
//   }
#define JASPERENGINE_NATIVE_PLUGIN(NAME) \
    static void jasperNativePlugin_##NAME(::jasperengine::NativePlugin& plugin); \
    static const ::jasperengine::NativePluginRegistrar jasperNativePluginRegistrar_##NAME(#NAME, &jasperNativePlugin_##NAME); \
    static void jasperNativePlugin_##NAME(::jasperengine::NativePlugin& plugin)

#endif // JASPERENGINE_NATIVEBRIDGE_H
