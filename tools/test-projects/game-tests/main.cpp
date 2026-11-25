// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The project test runner's player check (docs/specs/project-test-runner.md AC12, AC13): a Development
// build of this project runs assets/tests/ (smoke.test.lua, listed in index.json as an export writes it)
// with --run-tests. The web reads the flags from the page query (WEB_PROJECT=game-tests DEVELOPMENT=1
// tools/build.sh web, then GATE=game-tests tools/build.sh web-gate: ?run-tests&test-filter=tag:smoke),
// Android from debug.jasperengine.args (ANDROID_PROJECT=game-tests tools/build.sh android, then
// tools/android-emulator-test.sh with --setprop "debug.jasperengine.args=--run-tests --test-filter tag:smoke"
// and android-expect.txt). Each case reloads the Main scene in a fresh Lua state.

#include "JasperEngine.h"
using namespace jasperengine;

Scene scene;

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(1000, 480);
    // a registered scene: a test case loads it again
    SceneManager::registerScene(1, "Main", []() { Engine::setScene(&scene); });
    SceneManager::loadScene(1);
}
