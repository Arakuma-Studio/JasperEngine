// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CrashHook.h"
#include "CrashGuard.h"

#include "Log.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <typeinfo>
#include <unordered_map>

#if defined(__GNUG__)
#include <cxxabi.h>
#endif

using namespace jasperengine;

std::atomic<bool> jasperengine::detail::crashHookActive{false};

namespace je_crash_hook {}
using namespace je_crash_hook;

namespace je_crash_hook {

    struct HookState {
        std::mutex mutex;
        std::shared_ptr<CrashHandler> handler;
    };

    HookState& hookState() {
        static HookState* state = new HookState();
        return *state;
    }

}

void CrashHook::set(CrashHandler handler) {
    HookState& state = hookState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.handler = handler ? std::make_shared<CrashHandler>(std::move(handler)) : nullptr;
    detail::crashHookActive.store(state.handler != nullptr, std::memory_order_release);
}

bool CrashHook::report(const std::string& tag, const std::string& errorInfo) {
    HookState& state = hookState();
    std::shared_ptr<CrashHandler> handler;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        handler = state.handler;
    }
    if (!handler) return false;
    (*handler)(tag, errorInfo);
    return true;
}

bool CrashHook::runGuarded(void (*function)(void*), void* context, std::string& errorInfo) {
    CrashInfo info{};
    std::string exception;
    bool caught = false;
    // the catch is inside the guard: a throw never crosses it
    const bool ok = callWithCrashGuard([function, context, &exception, &caught]() {
        caught = !runCatching(function, context, exception);
    }, &info);
    if (!ok) {
        errorInfo = std::string(info.name ? info.name : "UNKNOWN") + " (" + (info.description ? info.description : "") +
                    ") [code=" + std::to_string(info.code) + "]";
        return false;
    }
    if (caught) {
        errorInfo = exception;
        return false;
    }
    return true;
}

bool CrashHook::runCatching(void (*function)(void*), void* context, std::string& errorInfo) {
    try {
        function(context);
        return true;
    } catch (...) {
        errorInfo = describeCurrentException();
        return false;
    }
}

std::string CrashHook::describeCurrentException() {
    std::exception_ptr current = std::current_exception();
    if (!current) {
        return "C++ exception of unknown type";
    }
    try {
        std::rethrow_exception(current);
    } catch (const std::exception& e) {
        std::string type = typeid(e).name();
#if defined(__GNUG__)
        int status = 0;
        char* demangled = abi::__cxa_demangle(type.c_str(), nullptr, nullptr, &status);
        if (status == 0 && demangled) {
            type = demangled;
        }
        std::free(demangled);
#endif
        // MSVC: "class std::runtime_error"
        for (const char* prefix : {"class ", "struct "}) {
            if (type.rfind(prefix, 0) == 0) type.erase(0, std::char_traits<char>::length(prefix));
        }
        return "C++ exception " + type + ": " + e.what();
    } catch (...) {
        return "C++ exception of unknown type";
    }
}

void CrashHook::logFailure(const std::string& tag) {
    // a callback failing every frame logs once a second (ScriptMessages' rate)
    // (events PhysicsSystem calls from Jolt threads fail there)
    static std::mutex mutex;
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastLogged;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto last = lastLogged.find(tag);
        if (last != lastLogged.end() && now - last->second < std::chrono::seconds(1)) {
            return;
        }
        lastLogged[tag] = now;
    }
    Log::error("%s failed: %s", tag.c_str(), describeCurrentException().c_str());
}
