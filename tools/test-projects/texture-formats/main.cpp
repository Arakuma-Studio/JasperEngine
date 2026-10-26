// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S14's test project: cooked textures through the runtime of a platform. assets/je-cooked/ holds
// the desktop, mobile, android_etc2 and web cooks of source/ (make_assets.sh), as an export with
// the ETC2 fallback ships them; the build loads its family's (ETC1S / UASTC transcoded at load on
// the web, ASTC on Android, ETC2 on an Android GPU without ASTC such as the emulator's
// SwiftShader, BC on desktop), falls back where the GPU cannot sample a format, and draws them. A solid
// texture fills the canvas centre: the gate checks its colour (WEB_PROJECT=texture-formats
// tools/build.sh web, GATE=texture-formats tools/build.sh web-gate; ANDROID_PROJECT=texture-formats
// tools/build.sh android with android-expect.txt).

#include "JasperEngine.h"
#include "subsystem/RenderSystem.h"
#include "texture/CookedTextures.h"

#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;

Scene scene;
Camera camera(&scene);
Primitive solid(&scene);
std::vector<std::unique_ptr<Primitive>> tiles;
bool reported = false;

namespace {
    const char* const kTiles[] = {"textures/photo.png", "textures/normal.png", "textures/sprite.png", "textures/sky.hdr"};
}

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(320, 240);
    scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);

    // the canvas centre: a cooked solid colour (200, 60, 30)
    solid.createPlane(2.0f, 2.0f);
    solid.setTexture("textures/solid.png");
    solid.setReceiveLights(false);

    // one corner each: colour, normal map, sprite with alpha, HDR
    const float corners[4][2] = {{-1.6f, -1.1f}, {1.6f, -1.1f}, {-1.6f, 1.1f}, {1.6f, 1.1f}};
    for (int i = 0; i < 4; i++){
        tiles.push_back(std::make_unique<Primitive>(&scene));
        tiles.back()->createPlane(0.9f, 0.9f);
        tiles.back()->setPosition(corners[i][0], 0.0f, corners[i][1]);
        tiles.back()->setTexture(kTiles[i]);
        tiles.back()->setReceiveLights(false);
    }

    camera.setType(CameraType::CAMERA_ORTHO);
    camera.setOrtho(-2.0f, 2.0f, -1.5f, 1.5f, 0.1f, 100.0f);
    camera.setPosition(0.0f, 10.0f, 0.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.setUp(0.0f, 0.0f, -1.0f);
    camera.activate();

    Engine::setScene(&scene);
    Engine::onUpdate.add("textureFormatsTest", [](){
        if (reported || !scene.getSystem<RenderSystem>()->isAllLoaded())
            return;
        reported = true;
        int cooked = 0;
        for (const char* path : kTiles){
            Texture texture(path);
            texture.load();
            Log::print("[gate] %s %s %dx%d stored %dx%d", path, texture.getGpuFormat().c_str(), texture.getWidth(), texture.getHeight(),
                       texture.getStoredWidth(), texture.getStoredHeight());
            cooked += texture.isCompressed() ? 1 : 0;
        }
        Log::print("[gate] texture family %s", CookedTextures::activeFamily().c_str());
        Log::print("[gate] textures loaded %d compressed %s", (int)(sizeof(kTiles) / sizeof(kTiles[0])), cooked > 0 ? "yes" : "no");
    });
}
