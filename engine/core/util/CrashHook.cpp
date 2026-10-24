// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CrashHook.h"
#include "CrashGuard.h"

#include <atomic>
#include <memory>
#include <mutex>

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
    const bool ok = callWithCrashGuard([function, context]() { function(context); }, &info);
    if (!ok) {
        errorInfo = std::string(info.name ? info.name : "UNKNOWN") + " (" + (info.description ? info.description : "") +
                    ") [code=" + std::to_string(info.code) + "]";
    }
    return ok;
}
