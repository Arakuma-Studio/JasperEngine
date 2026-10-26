// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S28's test project (docs/crash-diagnostics.md): a development build that logs a few lines and
// then crashes when asked to, so a platform's crash path can be checked end to end. The web
// build crashes with ?jasperengine_debug_crash=<mode> (GATE=crash-diagnostics tools/build.sh
// web-gate after WEB_PROJECT=crash-diagnostics tools/build.sh web); desktop builds with
// JASPERENGINE_DEBUG_CRASH=<mode>@<frame>. lua/main.lua uses the Lua Crash API.

#include "JasperEngine.h"
#include "diag/CrashHandler.h"
using namespace jasperengine;

Scene scene;

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(1000, 480);
    Engine::setScene(&scene);

    Log::print("[gate] crash diagnostics: cpp init");
#if defined(__ANDROID__)
    // Android cannot inherit a shell environment. This fixture triggers the same frame-10
    // fault as JASPERENGINE_DEBUG_CRASH=segv@10, retaining debuggerd's original handler.
    Engine::onUpdate.add("crashDiagnostics", [] {
        static unsigned frame = 0;
        if (++frame == 10) {
            Log::print("last Android log line before deliberate crash");
            diag::crash::debugCrash("segv");
        }
    });
#endif
}
