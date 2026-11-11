// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S81's test project (docs/specs/S81-title-art-runtime.md AC19): a Realtime Title Art Renderer on
// an Image renders its document (assets/titles/gate.titleart, a plate shape: no font) through the
// titleart module, time sliced within the frame budget, and then renders it again with an Exposed
// colour changed from C++ (TitleArt::setParameter). Each ready image logs a line; the canvas
// centre is the plate. The web build (WEB_PROJECT=title-art tools/build.sh web, GATE=title-art
// tools/build.sh web-gate) and Android (ANDROID_PROJECT=title-art tools/build.sh android) check
// the same lines.

#include "JasperEngine.h"

using namespace jasperengine;

Scene scene;
Image logo(&scene);

namespace {

    int readyCount = 0;

}

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(512, 256);
    Engine::setScene(&scene);
    scene.setBackgroundColor(0.1f, 0.11f, 0.14f);

    logo.setAnchorPreset(AnchorPreset::CENTER);
    logo.setSize(512, 256);
    TitleArtRendererComponent title;
    title.title = AssetRef("titles/gate.titleart");
    title.renderMode = TitleArtRenderMode::REALTIME;
    title.prebakeInitialState = false;
    scene.addComponent<TitleArtRendererComponent>(logo.getEntity(), std::move(title));

    TitleArt art(&scene, logo.getEntity());
    art.getOnReady().add("gate", [](){
        readyCount++;
        TitleArt ready(&scene, logo.getEntity());
        Log::print("[title-art] ready %d %s", readyCount, ready.getPixelPath().c_str());
        if (readyCount == 1){
            // a Realtime title takes a new value: rendered again within the budget (R9, R12)
            std::string error;
            if (!ready.setParameter("plate_color", std::string("#20c0ff"), &error)) Log::error("[title-art] %s", error.c_str());
        }
    });
    art.getOnFailed().add("gate", [](const std::string& message){
        Log::error("[title-art] failed: %s", message.c_str());
    });
}
