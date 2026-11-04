// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S41's web test project (docs/specs/S41-pixel-perfect-camera.md AC13, R16): a 2D scene whose main
// camera has a Pixel Perfect Camera at 320 x 180, with a sprite at a half-pixel position. Once the
// layout is made it logs the pixel ratio and the base resolution; the gate
// (WEB_PROJECT=pixel-perfect tools/build.sh web, GATE=pixel-perfect tools/build.sh web-gate) runs it
// at a device pixel ratio of 2 and checks that the canvas backing store is its CSS size times the
// ratio and that the browser draws it pixelated, so the integer scale is over physical pixels.

#include "JasperEngine.h"

#include <vector>

using namespace jasperengine;

Scene scene;
Camera sceneCamera(&scene);
Sprite hero(&scene);

namespace {

    std::vector<uint8_t> heroPixels;
    bool logged = false;

}

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(320, 180);
    Engine::setScene(&scene);
    scene.setBackgroundColor(0.10f, 0.11f, 0.14f);

    sceneCamera.setType(CameraType::CAMERA_ORTHO);
    sceneCamera.activate();
    PixelPerfectCamera pixelPerfect(&scene, sceneCamera.getEntity());
    pixelPerfect.setReferenceResolution(Vector2(320.0f, 180.0f));

    // a 16 x 16 checker: any resampling of the image would blend its texels
    heroPixels.resize(16 * 16 * 4);
    for (int y = 0; y < 16; y++){
        for (int x = 0; x < 16; x++){
            uint8_t* p = &heroPixels[(size_t)(y * 16 + x) * 4];
            const bool light = ((x + y) & 1) == 0;
            p[0] = light ? 240 : 40;
            p[1] = light ? 200 : 60;
            p[2] = light ? 80 : 160;
            p[3] = 255;
        }
    }
    hero.setTexture("pixel-perfect|hero", TextureData(16, 16, 16 * 16 * 4, ColorFormat::RGBA, 4, heroPixels.data()));
    hero.setSize(16, 16);
    hero.setPosition(100.5f, 60.25f, 0.0f);

    Engine::onUpdate.add("pixelPerfectGate", [](){
        if (logged) return;
        PixelPerfectCamera camera(&scene, sceneCamera.getEntity());
        if (camera.getPixelRatio() < 1) return;
        const Vector2 base = camera.getBaseResolution();
        logged = true;
        Log::print("[gate] pixel_perfect k %d base %dx%d", camera.getPixelRatio(), (int)base.x, (int)base.y);
    });
}
