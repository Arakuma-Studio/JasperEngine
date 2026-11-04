// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 AC5: the terrain operation engine in wasm. The same op sequences TerrainGolden.* pins on the
// native core suite run here, and the digest of their hashes goes to the console for the gate to
// compare (WEB_PROJECT=terrain-edit tools/build.sh web, GATE=terrain-edit tools/build.sh web-gate).

#include "JasperEngine.h"

#include "../../../engine/tests/core/terrain/TerrainGoldenSequences.h"

#include <cstdio>

using namespace jasperengine;

Scene scene;
Camera sceneCamera(&scene);

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(320, 180);

    const uint64_t digest = terraingolden::digestOf(terraingolden::runAll());
    char text[64];
    std::snprintf(text, sizeof(text), "[gate] terrain-edit %016llx", static_cast<unsigned long long>(digest));
    Log::print("%s", text);

    // The console holds the result the gate compares; the canvas only says the engine ran to here
    scene.setBackgroundColor(0.157f, 0.784f, 0.353f, 1.0f);
    sceneCamera.activate();
    Engine::setScene(&scene);
}
