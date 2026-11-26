// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S22's test project (docs/specs/S22-gameplay.md AC18): lua/main.lua asks for a captured pointer
// and logs the pointer lock state as it changes, the frame's touches with their phases, each
// finger's getTouchDown and each input scheme switch (input-device-awareness AC18). Checked in a browser (WEB_PROJECT=input tools/build.sh web,
// GATE=input tools/build.sh web-gate: PENDING until the gate's click, LOCKED after it; its tap
// switches the scheme to Touch, its key back to Keyboard&Mouse) and on an
// Android emulator (ANDROID_PROJECT=input tools/build.sh android, then tools/android-emulator-test.sh
// with --two-finger-tap and tools/test-projects/input/android-expect.txt).
//
// text-entry-gamepad-cursor-back-button AC7, AC22: an HTML UI form (assets/form/form.html, its
// script lua/FormUI.lua) with an e-mail field (enterkeyhint send, maxlength 5), focused at the
// scheme's second switch. The web gate then reads the IME textarea's inputmode and enterkeyhint and
// presses Escape (the document's back listener logs "[gate] back"); the emulator run
// (--ime "[gate] form focused", android-ime-expect.txt) reads the field's EditorInfo from dumpsys, types past the limit
// ("[gate] value abcde (5)") and presses Enter ("[gate] submit").

#include "JasperEngine.h"
#include "component/ScriptComponent.h"
#include "script/LuaBinding.h"

using namespace jasperengine;

Scene scene;
UIDocument form(&scene);

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(1000, 480);
    Engine::setScene(&scene);

    form.setFilename("form/form.html");
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.path = "FormUI.lua";
    entry.className = "FormUI";
    entry.enabled = true;
    ScriptComponent scripts;
    scripts.scripts.push_back(entry);
    form.addComponent<ScriptComponent>(scripts);
    LuaBinding::initializeLuaScripts(&scene);
}
