// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// PCSS in WebGL2 (draw-perf spec, "Follow-ups (2026-11-07)"): the ShadowPcssGlesGpu scene
// (engine/tests/core/render/ShadowPcssGlesGpuTest.cpp), a pole's shadow under a wide sun at the
// Ultra shadow tier, drawn by the directional-only Lit with the raw-depth view of the shadow atlas
// beside its comparison view (WEB_PROJECT=pcss tools/build.sh web, GATE=pcss tools/build.sh
// web-gate). The canvas centre is the floor beside the far half of the shadow, inside PCSS's
// penumbra and outside the Vogel disk's: the gate's baseline is the glcore test's Ultra pixel.

#include "JasperEngine.h"
#include "render/LightingTiers.h"

using namespace jasperengine;

Scene scene;
Camera sceneCamera(&scene);
Light sun(&scene);
Primitive floorBox(&scene);
Primitive pole(&scene);

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(640, 360);
    scene.setBackgroundColor(0.42f, 0.58f, 0.82f, 1.0f);
    scene.setShadowQuality(ShadowQuality::ULTRA);
    // the object lists: a directional-only variant is never the tiled form
    LightingTiers::setLightMode(LightMode::PER_OBJECT);

    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.6f, -1.0f, -0.3f);
    sun.setIntensity(2.0f);
    sun.setShadows(true);
    sun.setShadowType(ShadowType::SOFT);
    sun.setShadowAngularDiameter(8.0f);
    sun.setShadowMapSize(512);
    sun.setShadowDistance(20.0f);

    floorBox.createBox(12.0f, 0.2f, 12.0f);
    floorBox.setColor(0.85f, 0.85f, 0.85f);
    floorBox.setPosition(0.0f, -0.1f, 0.0f);
    pole.createBox(0.5f, 6.0f, 0.5f);
    pole.setColor(0.8f, 0.4f, 0.2f);
    pole.setPosition(0.0f, 3.0f, 0.0f);

    sceneCamera.setType(CameraType::CAMERA_PERSPECTIVE);
    sceneCamera.setPosition(-0.5f, 4.5f, 2.5f);
    sceneCamera.setTarget(-2.74f, 0.0f, -2.02f);
    sceneCamera.activate();

    Engine::setScene(&scene);
    Log::print("[gate] pcss");
}
