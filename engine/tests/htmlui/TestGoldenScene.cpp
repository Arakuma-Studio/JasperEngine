// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Full-path goldens of documents in their scenes: the letterboxed view rect, the order of
// two documents (sortOrder), :hover/:active from engine pointer input, documents in front
// of component UI, a document per scene layer, and the RmlUi debugger.

#include "TestGoldenCommon.h"

#include "HtmlUI.h"
#include "HtmlUIContext.h"
#include "Input.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/ui/Image.h"
#include "object/ui/Text.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UIDocumentSystem.h"

#include <memory>

using namespace jasperengine;
using namespace jasperengine::test;

TEST_F(FullPathGoldenTest, Letterbox){
    // a 640x360 canvas letterboxed on an 800x600 screen: view rect (0,75) 800x450
    GpuEnvironment::setView(800, 600, 640, 360, Scaling::LETTERBOX);
    ASSERT_EQ(Engine::getCanvasWidth(), 640);
    ASSERT_EQ(Engine::getCanvasHeight(), 360);

    SceneFixture fixture;
    fixture.addDocument("golden/letterbox.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    Screen screen;
    ASSERT_TRUE(capture(screen));
    ASSERT_EQ(screen.width, 800);
    ASSERT_EQ(screen.height, 600);
    EXPECT_FLOAT_EQ(fixture.getScene().getSystem<UIDocumentSystem>()->getContext()->getPixelRatio(), 1.25f);

    // the bars above and below the view rect are untouched
    EXPECT_TRUE(screen.inkOnBackground({0, 0, 800, 75}, 4).empty());
    EXPECT_TRUE(screen.inkOnBackground({0, 525, 800, 600}, 4).empty());
    // the body fills the view rect exactly
    EXPECT_TRUE(screen.is(400, 76, 0x30, 0x60, 0xc0));
    EXPECT_TRUE(screen.is(400, 523, 0x30, 0x60, 0xc0));
    EXPECT_TRUE(screen.is(400, 250, 0x30, 0x60, 0xc0));
    // 40 px corner marks at 1.25: 50 screen pixels from each corner of the view rect
    EXPECT_TRUE(screen.is(1, 76, 255, 0, 0));
    EXPECT_TRUE(screen.is(48, 123, 255, 0, 0));
    EXPECT_TRUE(screen.is(52, 100, 0x30, 0x60, 0xc0));
    EXPECT_TRUE(screen.is(798, 76, 0, 255, 0));
    EXPECT_TRUE(screen.is(752, 123, 0, 255, 0));
    EXPECT_TRUE(screen.is(1, 523, 255, 255, 0));
    EXPECT_TRUE(screen.is(48, 477, 255, 255, 0));
    EXPECT_TRUE(screen.is(798, 523, 255, 0, 255));
    EXPECT_TRUE(screen.is(752, 477, 255, 0, 255));
    // the label at (200,160) canvas = (250,275) screen
    EXPECT_FALSE(screen.ink({250, 270, 450, 330}, 0x30, 0x60, 0xc0).empty());

    EXPECT_SCREEN_GOLDEN(screen, "letterbox");
}

TEST_F(FullPathGoldenTest, SortOrder){
    SceneFixture fixture;
    UIDocument a = fixture.addDocument("golden/sort_a.html", 0);
    UIDocument b = fixture.addDocument("golden/sort_b.html", 1);
    ASSERT_TRUE(fixture.waitUntilLoaded());

    // a (100,80)-(360,240) red, b (260,140)-(520,300) blue; overlap (260,140)-(360,240)
    Screen first;
    ASSERT_TRUE(capture(first));
    EXPECT_TRUE(first.is(150, 120, 0xe0, 0x30, 0x30));
    EXPECT_TRUE(first.is(450, 270, 0x30, 0x30, 0xe0));
    EXPECT_TRUE(first.is(310, 190, 0x30, 0x30, 0xe0));
    EXPECT_SCREEN_GOLDEN(first, "sort_order_a");

    // swapped: a on top
    a.setSortOrder(1);
    b.setSortOrder(0);
    Screen second;
    ASSERT_TRUE(capture(second, 2));
    EXPECT_TRUE(second.is(150, 120, 0xe0, 0x30, 0x30));
    EXPECT_TRUE(second.is(450, 270, 0x30, 0x30, 0xe0));
    EXPECT_TRUE(second.is(310, 190, 0xe0, 0x30, 0x30));
    EXPECT_SCREEN_GOLDEN(second, "sort_order_b");
}

TEST_F(FullPathGoldenTest, HoverStates){
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("golden/hover.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    UIElement button = document.getElementById("button");
    ASSERT_TRUE(button.isValid());

    // the button: border box (240,156)-(404,208), 2 px white border
    const float x = 320, y = 182;

    Screen normal;
    ASSERT_TRUE(capture(normal));
    EXPECT_TRUE(normal.is(250, 165, 0x80, 0x80, 0x80));
    EXPECT_TRUE(normal.is(241, 182, 255, 255, 255));
    EXPECT_GT(normal.countColor({244, 160, 400, 204}, 0, 0, 0, 40), 30u);   // the black label
    EXPECT_SCREEN_GOLDEN(normal, "hover_normal");

    Engine::systemMouseMove(x, y, 0);
    Screen hover;
    ASSERT_TRUE(capture(hover, 2));
    EXPECT_TRUE(button.isPseudoClassSet("hover"));
    EXPECT_TRUE(hover.is(250, 165, 0x00, 0xff, 0x00));
    EXPECT_SCREEN_GOLDEN(hover, "hover_hover");

    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, x, y, 0);
    Screen active;
    ASSERT_TRUE(capture(active, 2));
    EXPECT_TRUE(button.isPseudoClassSet("active"));
    EXPECT_TRUE(active.is(250, 165, 0x00, 0x00, 0xff));
    EXPECT_GT(active.countColor({244, 160, 400, 204}, 255, 255, 255, 40), 30u);   // the white label
    EXPECT_SCREEN_GOLDEN(active, "hover_active");

    // release and leave: back to the normal image
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, x, y, 0);
    Engine::systemMouseMove(20, 20, 0);
    Screen released;
    ASSERT_TRUE(capture(released, 2));
    EXPECT_FALSE(button.isPseudoClassSet("hover"));
    EXPECT_SCREEN_GOLDEN(released, "hover_normal");
}

TEST_F(FullPathGoldenTest, ComponentUIUnder){
    SceneFixture fixture;

    // component UI first: a red image and a white text line
    Image& image = fixture.add<Image>();
    image.setPosition(50, 50);
    image.setSize(300, 200);
    image.setColor(1.0f, 0.0f, 0.0f, 1.0f);

    Text& text = fixture.add<Text>();
    text.setText("Component UI text");
    text.setFontSize(32);
    text.setColor(1.0f, 1.0f, 1.0f, 1.0f);
    text.setPosition(60, 300);

    fixture.addDocument("golden/component_ui_under.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    Screen screen;
    ASSERT_TRUE(capture(screen));

    // the image where the document has nothing, the opaque panel over its right part
    EXPECT_TRUE(screen.is(100, 75, 255, 0, 0));
    EXPECT_TRUE(screen.is(120, 200, 255, 0, 0));
    EXPECT_TRUE(screen.is(300, 150, 0x20, 0xa0, 0x40));
    EXPECT_TRUE(screen.is(450, 150, 0x20, 0xa0, 0x40));
    EXPECT_TRUE(screen.is(370, 80, goldenBackground, goldenBackground, goldenBackground));
    // the quarter-alpha glass over the background: 64 + 30 * 191 / 255
    EXPECT_TRUE(screen.is(590, 320, 86, 86, 86, 3));
    // the text under the glass still shows (white text stays white under white glass)
    EXPECT_GT(screen.countColor({60, 300, 400, 340}, 255, 255, 255, 12), 100u);

    EXPECT_SCREEN_GOLDEN(screen, "component_ui_under");
}

TEST_F(FullPathGoldenTest, TwoScenes){
    SceneFixture fixture;
    fixture.addDocument("golden/scene_main.html");

    // a second scene drawn as a layer over the main one (it loads the color buffer)
    std::unique_ptr<Scene> layer(new Scene());
    std::unique_ptr<Camera> layerCamera(new Camera(layer.get()));
    layerCamera->setType(CameraType::CAMERA_UI);
    layerCamera->activate();
    std::unique_ptr<UIDocument> layerDocument(new UIDocument(layer.get()));
    layerDocument->setFilename("golden/scene_layer.html");
    Engine::addSceneLayer(layer.get());

    Scene* layerScene = layer.get();
    Scene* mainScene = &fixture.getScene();
    const bool loaded = GpuEnvironment::runFrames([mainScene, layerScene](){
        for (Scene* scene : {mainScene, layerScene}){
            if (!scene->getSystem<RenderSystem>()->isAllLoaded() || !scene->getSystem<UIDocumentSystem>()->isAllLoaded())
                return false;
        }
        return true;
    });

    Screen screen;
    if (loaded){
        EXPECT_TRUE(capture(screen));
    }

    Engine::removeScene(layer.get());
    layerDocument.reset();
    layerCamera.reset();
    layer.reset();
    ASSERT_TRUE(loaded);

    // one context per scene; the layer scene's document is in front of the main scene's
    EXPECT_TRUE(screen.is(150, 120, 0xe0, 0x80, 0x20));
    EXPECT_TRUE(screen.is(450, 270, 0x20, 0xa0, 0xe0));
    EXPECT_TRUE(screen.is(310, 190, 0x20, 0xa0, 0xe0));
    EXPECT_TRUE(screen.isBackground(50, 320));

    EXPECT_SCREEN_GOLDEN(screen, "two_scenes");
}

TEST_F(FullPathGoldenTest, Debugger){
    SceneFixture fixture;
    fixture.addDocument("golden/debugger.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    auto system = fixture.getScene().getSystem<UIDocumentSystem>();
    system->setDebuggerVisible(true);
    EXPECT_EQ((void*)HtmlUI::getDebuggerContext(), (void*)system->getContext());

    Screen screen;
    ASSERT_TRUE(capture(screen, 4));

    // the debugger's menu bar at the top, drawn in its embedded font
    InkBox menu = screen.inkOnBackground({0, 0, 640, 40});
    EXPECT_GT(menu.count, 500u);
    EXPECT_GT(menu.width(), 200);
    // the inspected document under it
    EXPECT_TRUE(screen.is(590, 310, 0x30, 0x60, 0xc0));

    // it shows live values: a looser comparison
    CompareOptions options;
    options.pixelThreshold = 48;
    options.maxBadFraction = 0.01;
    options.maxMeanDiff = 3.0;
    EXPECT_SCREEN_GOLDEN_OPTIONS(screen, "debugger", options);

    system->setDebuggerVisible(false);
    Screen hidden;
    ASSERT_TRUE(capture(hidden, 2));
    EXPECT_TRUE(hidden.inkOnBackground({0, 0, 640, 40}).empty());
}
