// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S93 R2's web test project: lua/main.lua logs every mouse press and release the game gets, with
// its canvas position. Checked in a browser (WEB_PROJECT=web-pointer tools/build.sh web,
// GATE=web-pointer tools/build.sh web-gate): the canvas is smaller than the gate's window, so the
// gate drags presses off it; their releases reach the game clamped to the canvas, once each.

#include "JasperEngine.h"
using namespace jasperengine;

Scene scene;

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(640, 360);
    Engine::setScene(&scene);
}
