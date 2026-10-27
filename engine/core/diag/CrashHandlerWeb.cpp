// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The crash handler's web backend (CrashHandler.h): the browser owns crashes there, so it
// hands the page-level hooks of engine/platform/emscripten/crash_pre.js (linked with --pre-js)
// what they report with: the build hash, the product, and Project Settings > Player > Crash
// Handling > Web > Show Crash Overlay. The browser owns the main thread too: no watchdog.

#if defined(__EMSCRIPTEN__)

#include "detail/CrashState.h"

#include <emscripten.h>

namespace jasperengine::diag::crash::detail {

    bool installPlatform(const Config& config) {
        const std::string& hash = buildId();
        const int armed = EM_ASM_INT({
            var hooks = typeof Module != "undefined" && Module.jasperengineCrash;
            if (!hooks) return 0;
            hooks.configure({ overlay: $0 != 0, buildHash: UTF8ToString($1), product: UTF8ToString($2) });
            return 1;
        }, config.webOverlay ? 1 : 0, hash.c_str(), config.product ? config.product : "JasperEngine");
        return armed != 0;
    }

    void prepareThreadPlatform() {
    }

    bool platformDebugCrash(std::string_view) {
        return false;
    }

}

#endif
