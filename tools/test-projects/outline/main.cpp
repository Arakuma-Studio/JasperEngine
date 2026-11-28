// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S94's Outline on the web (docs/specs/S94-rendering-anim-gaps.md F-PB2, AC48): a red cube left of
// the canvas centre, outlined in green (Outline Visible, a world width), so the centre falls on its
// outline band. The swapchain is told it has no stencil (a WebGL canvas that did not grant one),
// so the camera draws through the offscreen target the Outline then needs. Checked in a browser
// (WEB_PROJECT=outline tools/build.sh web, GATE=outline tools/build.sh web-gate): the centre pixel
// is the outline's green, and no error line.

#include "JasperEngine.h"
#include "render/SystemRender.h"

using namespace jasperengine;

namespace {

    Scene scene;
    Camera sceneCamera(&scene);
    Primitive cube(&scene);
    int frames = 0;

}

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(640, 360);
    Engine::setScalingMode(Scaling::STRETCH);
    Engine::setScene(&scene);
    scene.setBackgroundColor(0.1f, 0.1f, 0.12f);

    // one world unit is one canvas pixel
    sceneCamera.setType(CameraType::CAMERA_ORTHO);
    sceneCamera.setOrtho(-320.0f, 320.0f, -180.0f, 180.0f, 1.0f, 1000.0f);
    sceneCamera.setPosition(0.0f, 0.0f, 500.0f);
    sceneCamera.setTarget(Vector3(0.0f, 0.0f, 0.0f));
    scene.setCamera(&sceneCamera);

    // x -110..-10; the band reaches about 17 units right of it (30 along the corner's normal)
    cube.createBox(100.0f, 100.0f, 100.0f);
    cube.setPosition(-60.0f, 0.0f, 0.0f);
    cube.setColor(1.0f, 0.0f, 0.0f);
    Outline outline(&scene, cube.getEntity());
    outline.setMode(OutlineMode::OUTLINE_VISIBLE);
    outline.setColor(Vector4(0.0f, 1.0f, 0.0f, 1.0f));
    outline.setWidthUnits(OutlineWidthUnits::WORLD);
    outline.setWidth(30.0f);

    Engine::onUpdate.add("outlineGate", [](){
        if (frames < 0){
            return;
        }
        // after the platform reported the canvas's stencil (its context exists by the first update)
        if (frames == 0){
            SystemRender::setSwapchainStencil(false);
            Log::print("[gate] outline: no swapchain stencil, the camera draws offscreen");
        }
        if (++frames == 30){
            frames = -1;
            Log::print("[gate] outline drawn");
        }
    });
}
