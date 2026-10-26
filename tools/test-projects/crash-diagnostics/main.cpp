// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S28's test project (docs/crash-diagnostics.md): a development build that logs a few lines and
// then crashes when asked to, so a platform's crash path can be checked end to end. The web
// build crashes with ?jasperengine_debug_crash=<mode> (GATE=crash-diagnostics tools/build.sh
// web-gate after WEB_PROJECT=crash-diagnostics tools/build.sh web); desktop builds with
// JASPERENGINE_DEBUG_CRASH=<mode>@<frame>. lua/main.lua uses the Lua Crash API.

#include "JasperEngine.h"
using namespace jasperengine;

Scene scene;

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(1000, 480);
    Engine::setScene(&scene);

    Log::print("[gate] crash diagnostics: cpp init");
}
