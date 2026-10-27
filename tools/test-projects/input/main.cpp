// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S22's test project (docs/specs/S22-gameplay.md AC18): lua/main.lua asks for a captured pointer
// and logs the pointer lock state as it changes, the frame's touches with their phases and each
// finger's getTouchDown. Checked in a browser (WEB_PROJECT=input tools/build.sh web,
// GATE=input tools/build.sh web-gate: PENDING until the gate's click, LOCKED after it) and on an
// Android emulator (ANDROID_PROJECT=input tools/build.sh android, then tools/android-emulator-test.sh
// with --two-finger-tap and tools/test-projects/input/android-expect.txt).

#include "JasperEngine.h"
using namespace jasperengine;

Scene scene;

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(1000, 480);
    Engine::setScene(&scene);
}
