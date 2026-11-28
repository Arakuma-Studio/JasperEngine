// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SaveGame's test project (docs/specs/savegame.md AC22, AC23): lua/main.lua writes slot1 the first
// time and reads it back on every later start. Checked in a browser (WEB_PROJECT=savegame
// tools/build.sh web, then GATE=savegame tools/build.sh web-gate: the page reloads and reads the
// slot from IndexedDB; GATE=savegame-quota: a 64 KiB site quota fails the larger write with
// "quota") and on an Android emulator (ANDROID_PROJECT=savegame tools/build.sh android, then
// tools/android-emulator-test.sh <apk> --kill-relaunch-after 'savegame: written'
// --expect-file tools/test-projects/savegame/android-expect.txt).

#include "JasperEngine.h"
using namespace jasperengine;

Scene scene;

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(1000, 480);
    Engine::setScene(&scene);
}
