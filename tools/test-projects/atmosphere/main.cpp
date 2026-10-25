// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S12's test project: the AtmosphereGpu.HeightFogValley scene (engine/tests/core/render/
// AtmosphereGpuTest.cpp) with its fog volumetric and Volumetric Clouds over it, so the web build
// runs every atmosphere pass in WebGL2 (WEB_PROJECT=atmosphere tools/build.sh web,
// GATE=atmosphere tools/build.sh web-gate). The canvas centre is fogged ground, whose colour the
// volumetric fog leaves as the analytic fog made it: the gate's baseline is the glcore golden's
// centre pixel (engine/tests/core/golden/atmosphere_valley.png).

#include "JasperEngine.h"

#include <memory>
#include <vector>

using namespace jasperengine;

Scene scene;
Camera camera(&scene);
Light sun(&scene);
Fog fog(&scene);
VolumetricClouds clouds(&scene);
Primitive ground(&scene);
std::vector<std::unique_ptr<Primitive>> buildings;

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(640, 360);
    scene.setBackgroundColor(0.42f, 0.58f, 0.82f, 1.0f);

    ground.createPlane(1200.0f, 1200.0f);
    ground.setColor(0.35f, 0.42f, 0.30f);
    for (int i = 0; i < 8; i++){
        const float z = -15.0f - 30.0f * (float)i;
        const float x = (i % 2 ? -1.0f : 1.0f) * (6.0f + 3.0f * (float)i);
        const float height = 8.0f + 4.0f * (float)i;
        buildings.push_back(std::make_unique<Primitive>(&scene));
        buildings.back()->createBox(5.0f, height, 5.0f);
        buildings.back()->setPosition(x, height * 0.5f, z);
        buildings.back()->setColor(0.7f, 0.66f, 0.6f);
    }
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.35f, -0.25f, -1.0f);
    sun.setColor(1.0f, 0.86f, 0.7f);
    sun.setIntensity(2.0f);
    sun.setShadows(false);

    fog.setType(FogType::HEIGHT);
    fog.setColor(0.62f, 0.68f, 0.78f);
    fog.setAttenuationDistance(90.0f);
    fog.setBaseHeight(0.0f);
    fog.setMaximumHeight(30.0f);
    fog.setDirectionalInscattering(0.5f);
    fog.setAnisotropy(0.6f);
    fog.setMaxFogDistance(3000.0f);
    fog.setVolumetric(true);

    clouds.setPreset(CloudsPreset::CLOUDY);
    clouds.setWindSpeed(0.0f);

    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.setPosition(0.0f, 8.0f, 30.0f);
    camera.setTarget(0.0f, 4.0f, -100.0f);
    camera.setFarClip(3000.0f);
    camera.activate();

    Engine::setScene(&scene);
    Log::print("[gate] atmosphere quality %s", AtmosphereTiers::getName(Engine::getAtmosphereQuality()));
}
