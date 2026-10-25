// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S09's test project: the log of C++ and of Lua (lua/main.lua, with a Timer and a coroutine)
// reaches the platform's log with its level and category, in order. Checked in a browser
// (WEB_PROJECT=core-utilities tools/build.sh web, GATE=core-utilities tools/build.sh web-gate) and
// on an Android emulator (ANDROID_PROJECT=core-utilities tools/build.sh android, then
// tools/android-emulator-test.sh with tools/test-projects/core-utilities/android-expect.txt).

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
