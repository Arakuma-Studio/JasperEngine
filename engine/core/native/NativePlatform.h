// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_NATIVEPLATFORM_H
#define JASPERENGINE_NATIVEPLATFORM_H

// Internal: the platform layer of the native bridge (NativeBridge.h). One implementation per
// platform, compiled like the HTTP backends (engine/CMakeLists.txt JASPERENGINE_NATIVE_PLATFORM):
//   Android  platform/android/NativePlatformAndroid.cpp (Java/Kotlin static methods, JNI)
//   Apple    platform/apple/NativePlatformApple.mm (Objective-C/Swift class methods)
//   Web      platform/emscripten/NativePlatformWeb.cpp (JavaScript objects, EM_JS)
//   others   core/native/NativePlatformNone.cpp (none: C++ plugins and Lua mocks only)
// NativeBridge calls it on the main thread only. Completions of async calls come back from
// any thread through NativeBridge::completeFromPlatform, events through NativeBridge::emit,
// engine pause requests through NativeBridge::pauseEngine/resumeEngine and game-thread tasks
// through MainThreadQueue::post. JSON crosses the language boundary as UTF-8 text
// (NativeJson::dump / parse), parsed once on the receiving side.

#include "native/NativeBridge.h"

#include <cstdint>
#include <string>

namespace jasperengine {

    class NativePlatform {
    public:
        virtual ~NativePlatform() = default;

        // "Android", "iOS", "macOS", "Web" (messages)
        virtual const char* name() const = 0;
        // method "": the plugin exists here (a class / object of that name). Cheap after the
        // first lookup of a name (cached).
        virtual bool has(const std::string& plugin, const std::string& method) = 0;
        // Sync, on the engine thread. The result, or null with `err` set (no_method, bad_args,
        // exception, unsupported_type, invalid_utf8, async_only, not_ready).
        virtual NativeJson call(const std::string& plugin, const std::string& method, const NativeJson& args, NativeError& err) = 0;
        // Starts an async call; returns at once. The platform completes it later, exactly once,
        // with NativeBridge::completeFromPlatform(id, ...) (from any thread). background: a
        // worker thread instead of the UI/main thread.
        virtual void callAsync(uint64_t id, const std::string& plugin, const std::string& method, const NativeJson& args, bool background) = 0;
        // The call `id` was cancelled (by the caller, its owner, a timeout, Play stop): native
        // code sees isCancelled() from now on. A completion that still comes is ignored.
        virtual void cancel(uint64_t id) = 0;
        // Play stop / engine shutdown (after every pending call was cancelled)
        virtual void shutdown() {}
    };

    // The platform's layer, or null where there is none (Linux, Windows). Made once, on the
    // first use, on the main thread.
    NativePlatform* createNativePlatform();

    // The Lua side of the bridge (NativeClassesLua.cpp): Lua mocks, the last resolution step
    class NativeMockProvider {
    public:
        virtual ~NativeMockProvider() = default;
        // loads <luaDir>/native_mocks/<plugin>.lua once when the plugin has no mock yet
        virtual bool has(const std::string& plugin, const std::string& method) = 0;
        virtual NativeJson call(const std::string& plugin, const std::string& method, const NativeJson& args, NativeError& err) = 0;
        // runs the mock function now (main thread) and completes `call`: with its return
        // value, or later when it returned Native.pending(); the callback runs next frame
        // either way
        virtual void callAsync(const NativeCall& call) = 0;
        // Play stop / shutdown: mocks set by Native.setMock and the module cache go
        virtual void reset() = 0;
    };

    namespace NativeBridgeInternal {
        // the Lua layer installs itself (LuaBinding::registerNativeClasses); null removes it
        JASPERENGINE_API void setMockProvider(NativeMockProvider* provider);
        // an async call that fails with `error` in the next frame without starting anything
        // (Native.callAsync with arguments JSON cannot hold)
        JASPERENGINE_API NativeCall failedCall(const std::string& plugin, const std::string& method, NativeBridge::Completion done,
                                               AsyncOwner owner, const NativeError& error);
        // tests: a platform layer instead of createNativePlatform()'s (null: none); the old one
        // is not deleted
        JASPERENGINE_API void setPlatformForTests(NativePlatform* platform);
    }

}

#endif // JASPERENGINE_NATIVEPLATFORM_H
