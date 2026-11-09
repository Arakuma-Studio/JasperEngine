// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5 AC1, AC4: foliage in wasm with one thread. The fixture's hash goes to the console for the gate to
// compare with the constant the native suite pins, and a camera jump fills a new ring within the frame
// budget (WEB_PROJECT=foliage tools/build.sh web, GATE=foliage tools/build.sh web-gate). The terrain is a real
// one, built and drawn by the engine; its foliage is assets/cross.obj. The camera looks up, so the canvas
// centre stays the background.

#include "JasperEngine.h"

#include "../../../engine/tests/core/terrain/FoliageGoldenFixture.h"

#include "component/TerrainComponent.h"
#include "subsystem/FoliageSystem.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr int MAP = 256;

    Scene scene;
    Camera sceneCamera(&scene);
    Terrain ground(&scene);
    Entity terrainEntity = NULL_ENTITY;
    FoliageSystem* foliage = nullptr;
    std::vector<double> frames;
    int phase = 0;
    int ticks = 0;

    void giveMap(Texture& texture, const char* id, const std::vector<uint8_t>& bytes, ColorFormat format) {
        unsigned char* raw = static_cast<unsigned char*>(std::malloc(bytes.size()));
        std::copy(bytes.begin(), bytes.end(), raw);
        TextureData data(MAP, MAP, static_cast<unsigned int>(bytes.size()), format, 1, raw);
        data.setDataOwned(false);
        texture.setData(id, data);
        texture.getData().setDataOwned(true);
    }

    void update() {
        if (phase > 1) return;
        // the engine builds the terrain from its heightmap first; until then the foliage waits for it
        if (!scene.getComponent<TerrainComponent>(terrainEntity).heightMapLoaded) return;
        const auto start = std::chrono::steady_clock::now();
        foliage->update(1.0 / 60.0);
        frames.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        ticks++;
        if (foliage->hasPendingUpdates() && ticks < 20000) return;
        if (phase == 0) {
            // the jump: a new ring, none of the old cells in it
            sceneCamera.setPosition(100.0f, 40.0f, 100.0f);
            scene.getComponent<Transform>(sceneCamera.getEntity()).worldPosition = Vector3(100.0f, 40.0f, 100.0f);
            frames.clear();
            ticks = 0;
            phase = 1;
            return;
        }
        std::sort(frames.begin(), frames.end());
        const double p99 = frames.empty() ? 0.0 : frames[static_cast<size_t>((frames.size() - 1) * 0.99)];
        const bool complete = !foliage->hasPendingUpdates();
        Log::print("%s", complete ? "[gate] foliage cells complete" : "[gate] foliage cells pending");
        if (p99 <= 4.0) {
            Log::print("[gate] foliage p99 within 4 ms");
        } else {
            Log::print("[gate] foliage p99 %.2f ms", p99);
        }
        phase = 2;
    }

}

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(320, 180);

    const uint64_t hash = foliagegolden::hashFixture();
    char text[64];
    std::snprintf(text, sizeof(text), "[gate] foliage %016llx", static_cast<unsigned long long>(hash));
    Log::print("%s", text);

    terrainEntity = ground.getEntity();
    TerrainComponent& terrain = scene.getComponent<TerrainComponent>(terrainEntity);
    terrain.terrainSize = 256.0f;
    terrain.maxHeight = 40.0f;
    giveMap(terrain.heightMap, "foliage-gate-height", foliagegolden::fixture().height, ColorFormat::RED16);
    TerrainFoliageLayer layer;
    layer.meshPath = AssetRef("cross.obj");
    layer.density = 2.0f;
    layer.maxSlope = 90.0f;
    layer.drawDistance = 80.0f;
    layer.seed = 11;
    giveMap(layer.densityMap, "foliage-gate-density", foliagegolden::fixture().density, ColorFormat::RED);
    terrain.foliageLayers.push_back(layer);
    foliage = scene.getSystem<FoliageSystem>().get();

    // The console holds the results the gate compares; the canvas only says the engine ran to here
    scene.setBackgroundColor(0.157f, 0.784f, 0.353f, 1.0f);
    sceneCamera.setPosition(0.0f, 40.0f, 0.0f);
    sceneCamera.setTarget(0.0f, 140.0f, -200.0f);
    sceneCamera.setFarClip(1000.0f);
    sceneCamera.activate();
    Engine::setScene(&scene);
    Engine::onUpdate.add("foliageGate", update);
}
