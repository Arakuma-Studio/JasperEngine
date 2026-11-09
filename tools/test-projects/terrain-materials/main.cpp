// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T3 AC23: a painted terrain at Low with a hole in the middle, on WebGL2 and Android
// (WEB_PROJECT=terrain-materials tools/build.sh web, GATE=terrain-materials tools/build.sh web-gate).
// The camera looks straight down at a hole over nothing, so the canvas centre is the sky colour seen
// through it, and 'painted ground' a colour of the layer. A texelFetch that reads wrong, or a variant
// that fails to compile on GLES3, draws ground or nothing at the centre.

#include "JasperEngine.h"

#include "terrain/TerrainQuality.h"

#include <vector>

using namespace jasperengine;

Scene scene;
Camera sceneCamera(&scene);
Light sun(&scene);
Terrain terrain(&scene);

static std::vector<unsigned char> heights(33 * 33, 64);
static std::vector<unsigned char> holes(33 * 33, 255);

JASPERENGINE_INIT void init(){
    jasperengine::terraincore::TerrainQualitySettings::setGlobalOverride(jasperengine::terraincore::TerrainQuality::LOW);
    for (int y = 12; y < 21; y++){
        for (int x = 12; x < 21; x++){
            holes[static_cast<size_t>(y) * 33 + x] = 0;
        }
    }
    Engine::setCanvasSize(640, 360);
    scene.setBackgroundColor(1.0f, 0.0f, 1.0f, 1.0f);
    sceneCamera.setType(CameraType::CAMERA_PERSPECTIVE);
    sceneCamera.setPosition(0.0f, 60.0f, 0.01f);
    sceneCamera.setTarget(0.0f, 0.0f, 0.0f);
    sceneCamera.setFarClip(1000.0f);
    sceneCamera.activate();
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.3f, -1.0f, -0.2f);
    sun.setIntensity(2.0f);
    sun.setShadows(false);
    terrain.setSize(64.0f);
    terrain.setMaxHeight(8.0f);
    terrain.setTextureLayer(0, "layer.png");
    terrain.setBlendMap(0, "blend.png");
    TerrainComponent& component = scene.getComponent<TerrainComponent>(terrain.getEntity());
    component.heightMap.setData("terrain-materials-height", TextureData(33, 33, static_cast<unsigned int>(heights.size()), ColorFormat::RED, 1, heights.data()));
    component.heightMap.setReleaseDataAfterLoad(false);
    component.holeMap.setData("terrain-materials-holes", TextureData(33, 33, static_cast<unsigned int>(holes.size()), ColorFormat::RED, 1, holes.data()));
    component.needUpdateTerrain = true;
    terrain.createTerrain();
    Engine::setScene(&scene);
    Log::print("[gate] terrain-materials");
}
