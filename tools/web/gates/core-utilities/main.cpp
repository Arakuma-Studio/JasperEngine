// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The web gate's S09 project (tools/build.sh web WEB_PROJECT=core-utilities, GATE=core-utilities):
// the C++ log reaches the browser console with its level, synchronously and in order; lua/main.lua
// does the same from Lua and with a Timer.

#include "JasperEngine.h"
using namespace jasperengine;

Scene scene;

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(1000, 480);
    Engine::setScene(&scene);

    Log::print("[gate] cpp info 2");
    Log::warn("[gate] cpp warning 3");
    Log::error("[gate] cpp error 4");
    JE_LOG_WARN(logCategoryRender(), "[gate] category warning 5");
}
