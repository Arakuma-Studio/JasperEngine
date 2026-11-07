// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The platform-lifecycle test project (docs/superpowers/specs/2026-11-04-platform-lifecycle-design.md):
// a looping tone that starts with the scene, so the page's audio waits for a user gesture, and
// lua/main.lua logging every key the engine gets. Checked in a browser with Chrome's desktop autoplay
// policy (WEB_PROJECT=web-keys-audio tools/build.sh web, GATE=web-keys-audio tools/build.sh web-gate):
// the first key press starts the audio, and Meta, ContextMenu and the legacy key codes map.

#include "JasperEngine.h"
using namespace jasperengine;

Scene scene;

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(1000, 480);
    Engine::setScene(&scene);

    static AudioSource tone(&scene);
    tone.loadSound("tone.wav");
    tone.setLooping(true);
    tone.play();
}
