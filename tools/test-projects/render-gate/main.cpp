// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// BF3's test project: what a phone shows, checked against golden screenshots. One scene per run,
// picked by the Android property debug.jasperengine.scene (tools/android-emulator-test.sh --scene;
// JASPERENGINE_SCENE elsewhere):
//   lit3d   a lit 3D scene: coloured boxes and a sphere on a floor under a directional light
//   ui2d    2D and UI: a textured Image (the logo), coloured Polygons and a Text
//   htmlui  an HTML UI document: a gradient panel with rounded swatches, a bar, text, a button
//   titleart  Title Art Renderers (S81 AC19): a Realtime and a Glyph Atlas title (a plate, so the
//             Glyph Atlas one renders through the Realtime path) with the sweep at a fixed
//             position and the tint; debug.jasperengine.titleart_rgba8=1 forces the RGBA8 pixel path,
//             which must match the same golden
// tools/android-emulator-test.sh runs it once per scene (--scene) and compares the screen with
// golden/<scene>.png (--golden); ANDROID_PROJECT=render-gate tools/build.sh android builds it (see
// docs/testing.md, "Android emulator").

#include "JasperEngine.h"

#include <cstdlib>
#include <string>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

using namespace jasperengine;

Scene lit3dScene;
Camera sceneCamera(&lit3dScene);
Light sun(&lit3dScene);
Primitive ground(&lit3dScene);
Primitive redBox(&lit3dScene);
Primitive greenBox(&lit3dScene);
Primitive sphere(&lit3dScene);

Scene ui2dScene;
Image logo(&ui2dScene);
Polygon redQuad(&ui2dScene);
Polygon blueTriangle(&ui2dScene);
Text caption(&ui2dScene);

Scene htmluiScene;
UIDocument document(&htmluiScene);

Scene titleartScene;
Image realtimeTitle(&titleartScene);
Image atlasTitle(&titleartScene);

namespace {

    std::string selectedScene(){
    #if defined(__ANDROID__)
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.jasperengine.scene", value) > 0)
            return value;
    #endif
        const char* env = std::getenv("JASPERENGINE_SCENE");
        return env ? env : "lit3d";
    }

    void setupLit3d(){
        lit3dScene.setBackgroundColor(0.45f, 0.62f, 0.85f);

        sceneCamera.setType(CameraType::CAMERA_PERSPECTIVE);
        sceneCamera.setPosition(0.0f, 6.0f, 18.0f);
        sceneCamera.setTarget(Vector3(0.0f, 0.8f, 0.0f));
        lit3dScene.setCamera(&sceneCamera);

        ground.createPlane(12.0f, 12.0f);
        ground.setColor(0.55f, 0.55f, 0.5f);
        redBox.createBox(1.6f, 1.6f, 1.6f);
        redBox.setPosition(-1.6f, 0.8f, 0.0f);
        redBox.setColor(0.85f, 0.2f, 0.15f);
        greenBox.createBox(1.0f, 2.4f, 1.0f);
        greenBox.setPosition(1.8f, 1.2f, -1.0f);
        greenBox.setColor(0.2f, 0.7f, 0.3f);
        sphere.createSphere(0.9f);
        sphere.setPosition(0.4f, 0.9f, 1.6f);
        sphere.setColor(0.95f, 0.85f, 0.3f);

        sun.setType(LightType::DIRECTIONAL);
        sun.setDirection(-0.5f, -1.0f, -0.4f);
        sun.setIntensity(3.0f);
        // the shadow map's filtering differs by GPU: the golden compares the lighting
        sun.setShadows(false);

        Engine::setScene(&lit3dScene);
    }

    void setupUi2d(){
        ui2dScene.setBackgroundColor(0.12f, 0.12f, 0.16f);

        logo.setAnchorPreset(AnchorPreset::CENTER_TOP);
        logo.setPositionYOffset(120.0f);
        logo.setTexture("jasperengine.png");

        redQuad.addVertex(0.0f, 0.0f);
        redQuad.addVertex(300.0f, 0.0f);
        redQuad.addVertex(0.0f, 300.0f);
        redQuad.addVertex(300.0f, 300.0f);
        redQuad.setPosition(80.0f, 520.0f, 0.0f);
        redQuad.setColor(0.85f, 0.25f, 0.2f);

        blueTriangle.addVertex(150.0f, 0.0f);
        blueTriangle.addVertex(0.0f, 300.0f);
        blueTriangle.addVertex(300.0f, 300.0f);
        blueTriangle.setPosition(460.0f, 520.0f, 0.0f);
        blueTriangle.setColor(0.25f, 0.5f, 0.9f);

        caption.setText("JasperEngine 2D");
        caption.setFontSize(120);
        caption.setColor(0.95f, 0.9f, 0.75f);
        caption.setAnchorPreset(AnchorPreset::CENTER);
        caption.setPositionYOffset(260.0f);

        Engine::setScene(&ui2dScene);
    }

    void addTitle(Image& image, TitleArtRenderMode mode, float yOffset){
        image.setAnchorPreset(AnchorPreset::CENTER);
        image.setPositionYOffset(yOffset);
        image.setSize(768, 384);
        TitleArtRendererComponent title;
        title.title = AssetRef("titles/gate.titleart");
        title.renderMode = mode;
        title.prebakeInitialState = false;
        // the sweep at a fixed position (not Auto) and the tint read no time: every frame is the golden's
        title.sweepEnabled = true;
        title.sweepPosition = 0.45f;
        title.tintEnabled = true;
        title.tintColor = Vector4(0.2f, 0.4f, 1.0f, 1.0f);
        title.tintAmount = 0.3f;
        titleartScene.addComponent<TitleArtRendererComponent>(image.getEntity(), std::move(title));
        TitleArt art(&titleartScene, image.getEntity());
        const std::string label = mode == TitleArtRenderMode::REALTIME ? "realtime" : "glyph_atlas";
        art.getOnReady().add("gate", [label, &image](){
            TitleArt ready(&titleartScene, image.getEntity());
            Log::print("[render-gate] title %s ready %s", label.c_str(), ready.getPixelPath().c_str());
        });
    }

    void setupTitleart(){
        titleartScene.setBackgroundColor(0.1f, 0.11f, 0.14f);
        addTitle(realtimeTitle, TitleArtRenderMode::REALTIME, -300.0f);
        addTitle(atlasTitle, TitleArtRenderMode::GLYPH_ATLAS, 300.0f);
        Engine::setScene(&titleartScene);
    }

    void setupHtmlui(){
        htmluiScene.setBackgroundColor(0.08f, 0.09f, 0.12f);
        document.setFilename("ui/gate.html");
        Engine::setScene(&htmluiScene);
    }

}

JASPERENGINE_INIT void init(){
    // a phone held upright (the emulator's Pixel 6 is 1080x2400): every scene fills its width
    Engine::setCanvasSize(900, 2000);
    Engine::setScalingMode(Scaling::FITWIDTH);

    const std::string scene = selectedScene();
    if (scene == "ui2d"){
        setupUi2d();
    }else if (scene == "htmlui"){
        setupHtmlui();
    }else if (scene == "titleart"){
        setupTitleart();
    }else{
        setupLit3d();
    }
    Log::print("[render-gate] scene %s", scene == "ui2d" || scene == "htmlui" || scene == "titleart" ? scene.c_str() : "lit3d");
}
