// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S95's test project (REQ-15, AC-17): an HTML UI document whose script keeps a counter only in the
// DOM. The view goes and comes back: on Android the context goes with LOW_MEMORY in the background
// (tools/android-emulator-test.sh --view-loss: Home, a trim-memory call, the launcher again); in a
// browser the WebGL context is lost and restored by hand. After the return the script logs the
// counter the DOM still holds and how many times the document loaded: the same document, not one
// loaded again from its file, and the page still shows 7.
//   ANDROID_PROJECT=app-lifecycle tools/build.sh android
//   tools/android-emulator-test.sh build/android-app-lifecycle/app-debug.apk --view-loss --ordered \
//       --expect-file tools/test-projects/app-lifecycle/android-expect.txt --once 'SVG plugin initialised'
//   WEB_PROJECT=app-lifecycle tools/build.sh web, tools/web/serve.py --root build/web-app-lifecycle, then
//   in the page's console: c = document.querySelector('#canvas'); x = c.getContext('webgl2')
//   .getExtension('WEBGL_lose_context'); x.loseContext(); and once it is reported lost, x.restoreContext()
// (not a web gate: the gate's chrome-headless-shell stops answering after any WebGL context loss of
// this page, even with the engine's handler reduced to setting a flag)

#include "JasperEngine.h"
#include "component/ScriptComponent.h"
#include "script/LuaBinding.h"

using namespace jasperengine;

Scene scene;

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(1280, 720);
    Engine::setScene(&scene);
    scene.setBackgroundColor(0.1f, 0.1f, 0.12f);

    UIDocument* document = new UIDocument(&scene);
    document->setFilename("lifecycle.html");
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.path = "LifecycleProbe.lua";
    entry.className = "LifecycleProbe";
    entry.enabled = true;
    ScriptComponent scripts;
    scripts.scripts.push_back(entry);
    document->addComponent<ScriptComponent>(scripts);
    LuaBinding::initializeLuaScripts(&scene);
}
