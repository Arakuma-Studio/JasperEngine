// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Full-path goldens of documents in their scenes: the letterboxed view rect, the order of
// two documents (sortOrder), :hover/:active from engine pointer input, documents in front
// of component UI, a document per scene layer, and the RmlUi debugger. S92 (HtmlUIScene.*):
// where the screen documents composite: after post at the output resolution (Screen Space -
// Overlay) or in the camera pass (Screen Space - Camera), and in screen space under any camera.

#include "TestGoldenCommon.h"

#include "HtmlUI.h"
#include "HtmlUIContext.h"
#include "RmlRenderer.h"
#include "Input.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/PixelPerfectCamera.h"
#include "object/ui/Image.h"
#include "object/ui/Text.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UIDocumentSystem.h"
#include "component/UIDocumentComponent.h"
#include "render/FramebufferRender.h"
#include "component/PixelPerfectCameraComponent.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::test;

TEST_F(FullPathGoldenTest, Letterbox){
    // a 640x360 canvas letterboxed on an 800x600 screen: view rect (0,75) 800x450
    GpuEnvironment::setView(800, 600, 640, 360, Scaling::LETTERBOX);
    ASSERT_EQ(Engine::getCanvasWidth(), 640);
    ASSERT_EQ(Engine::getCanvasHeight(), 360);

    SceneFixture fixture;
    fixture.addDocument("golden/letterbox.rml");
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
    UIDocument a = fixture.addDocument("golden/sort_a.rml", 0);
    UIDocument b = fixture.addDocument("golden/sort_b.rml", 1);
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
    UIDocument document = fixture.addDocument("golden/hover.rml");
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

    fixture.addDocument("golden/component_ui_under.rml");
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
    fixture.addDocument("golden/scene_main.rml");

    // a second scene drawn as a layer over the main one (it loads the color buffer)
    std::unique_ptr<Scene> layer(new Scene());
    std::unique_ptr<Camera> layerCamera(new Camera(layer.get()));
    layerCamera->setType(CameraType::CAMERA_UI);
    layerCamera->activate();
    std::unique_ptr<UIDocument> layerDocument(new UIDocument(layer.get()));
    layerDocument->setFilename("golden/scene_layer.rml");
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
    fixture.addDocument("golden/debugger.rml");
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

// ---------------------------------------------------------------------------------------
// S92 (AC13, AC14): the composite stage of the scene's screen documents
// ---------------------------------------------------------------------------------------

using HtmlUIScene = FullPathGoldenTest;

namespace {

    void overlayEditPost(Scene& scene, const std::function<void(PostProcessingSettings&)>& edit){
        PostProcessingSettings settings = scene.getPostProcessing();
        edit(settings);
        scene.setPostProcessing(settings);
    }

    size_t overlayPixelsChanged(const Screen& a, const Screen& b, int tolerance = 0){
        if (a.width != b.width || a.height != b.height)
            return (size_t)-1;
        size_t count = 0;
        for (size_t i = 0; i < a.rgba.size(); i += 4){
            for (size_t c = 0; c < 3; c++){
                if (std::abs((int)a.rgba[i + c] - (int)b.rgba[i + c]) > tolerance){
                    count++;
                    break;
                }
            }
        }
        return count;
    }

    // the probes of overlay_detail.rml at the output resolution (640 x 360, one canvas px a pixel)
    void expectOverlayDetail(const Screen& screen){
        EXPECT_TRUE(screen.is(100, 100, 255, 255, 255, 0));
        EXPECT_TRUE(screen.is(101, 100, 0x30, 0x60, 0xc0, 0));
        EXPECT_TRUE(screen.is(103, 100, 255, 255, 255, 0));
        EXPECT_TRUE(screen.is(300, 80, 255, 255, 0, 0));
        EXPECT_TRUE(screen.is(300, 81, 0x30, 0x60, 0xc0, 0));
        EXPECT_TRUE(screen.is(0, 0, 255, 0, 0, 0));
        EXPECT_TRUE(screen.is(639, 359, 0, 255, 0, 0));
    }

}

// R13: render_scale below 1 draws the scene at half size and the uber pass scales it up; the
// documents composite in that pass, at the output resolution: the same pixels as at scale 1
TEST_F(HtmlUIScene, OverlayIgnoresRenderScale){
    SceneFixture fixture;
    fixture.addDocument("golden/overlay_detail.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    ASSERT_EQ(fixture.getScene().getHtmlUiRenderMode(), UIRenderMode::OVERLAY) << "Screen Space - Overlay is the default";
    Screen full;
    ASSERT_TRUE(capture(full));
    expectOverlayDetail(full);

    overlayEditPost(fixture.getScene(), [](PostProcessingSettings& post){ post.renderScale = 0.5f; });
    ASSERT_TRUE(fixture.waitUntilLoaded());
    EXPECT_FLOAT_EQ(fixture.getScene().getSystem<RenderSystem>()->getAppliedRenderScale(), 0.5f) << "the scene draws at half size";

    Screen half;
    ASSERT_TRUE(capture(half));
    expectOverlayDetail(half);
    EXPECT_EQ(overlayPixelsChanged(full, half), 0u);
}

// R13: FXAA and CRT (scanlines, mask, curvature, vignette) run in the uber pass before the
// documents: an opaque document shows exactly as without post-processing
TEST_F(HtmlUIScene, OverlayIgnoresFxaaAndCrt){
    SceneFixture fixture;
    fixture.addDocument("golden/overlay_detail.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Screen plain;
    ASSERT_TRUE(capture(plain));

    overlayEditPost(fixture.getScene(), [](PostProcessingSettings& post){
        post.fxaaEnabled = true;
        post.crtEnabled = true;
    });
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Screen post;
    ASSERT_TRUE(capture(post));
    expectOverlayDetail(post);
    EXPECT_EQ(overlayPixelsChanged(plain, post), 0u);

    // the effects run: Screen Space - Camera puts the document under them
    fixture.getScene().setHtmlUiRenderMode(UIRenderMode::CAMERA);
    Screen filtered;
    ASSERT_TRUE(capture(filtered, 2));
    EXPECT_GT(overlayPixelsChanged(plain, filtered, 8), 1000u);
}

// R14: Screen Space - Camera composites in the camera pass, as before S92: at render_scale 0.5
// the document is drawn at half size and scaled up with the scene (the golden is that image,
// captured on the engine before S92)
TEST_F(HtmlUIScene, CameraModeKeepsOldPlacement){
    SceneFixture fixture;
    fixture.getScene().setHtmlUiRenderMode(UIRenderMode::CAMERA);
    fixture.addDocument("golden/overlay_detail.rml");
    overlayEditPost(fixture.getScene(), [](PostProcessingSettings& post){ post.renderScale = 0.5f; });
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Screen scaled;
    ASSERT_TRUE(capture(scaled));
    // the one-pixel lines are scaled down and up with the scene: no longer sharp
    EXPECT_FALSE(scaled.is(101, 100, 0x30, 0x60, 0xc0, 0) && scaled.is(100, 100, 255, 255, 255, 0));
    EXPECT_SCREEN_GOLDEN(scaled, "overlay_camera_mode_scaled");

    fixture.getScene().setHtmlUiRenderMode(UIRenderMode::OVERLAY);
    Screen overlay;
    ASSERT_TRUE(capture(overlay, 2));
    expectOverlayDetail(overlay);
    EXPECT_GT(overlayPixelsChanged(scaled, overlay, 8), 100u);
}

// R13: a Pixel Perfect Camera draws its base image into the fixed-resolution target, and its blit
// scales it up by a whole number and draws the bars; the documents composite in that blit, at the
// screen's resolution over the image's viewport (before S92 they were drawn into the base image and
// scaled up with it: every 2 x 2 block of screen pixels one colour)
TEST_F(HtmlUIScene, PixelPerfectBarsCoveredByOverlay){
    GpuEnvironment::setView(800, 600, 320, 180, Scaling::LETTERBOX);
    SceneFixture fixture;
    PixelPerfectCamera pixelPerfect(&fixture.getScene(), fixture.getCamera().getEntity());
    pixelPerfect.setReferenceResolution(Vector2(320.0f, 180.0f));
    pixelPerfect.setLetterboxColor(Vector4(1.0f, 0.0f, 1.0f, 1.0f));
    fixture.addDocument("golden/pixel_perfect_ui.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    ASSERT_EQ(fixture.getScene().getComponent<PixelPerfectCameraComponent>(fixture.getCamera().getEntity()).layout.scale, 2)
        << "the 320 x 180 base image at 2x in 800 x 600: bars around it";

    Screen screen;
    ASSERT_TRUE(capture(screen));
    const Rect view = Engine::getViewRect();
    const int x0 = (int)view.getX(), y0 = (int)view.getY();
    const int x1 = (int)(view.getX() + view.getWidth()) - 1, y1 = (int)(view.getY() + view.getHeight()) - 1;
    EXPECT_EQ(x1 - x0 + 1, 640);
    EXPECT_EQ(y1 - y0 + 1, 360);
    // the document over the whole viewport, the bars in the letterbox colour around it
    EXPECT_TRUE(screen.is(x0, y0, 255, 0, 0, 0));
    EXPECT_TRUE(screen.is(x1, y1, 0, 255, 0, 0));
    EXPECT_TRUE(screen.is(x0 - 1, y0 + 100, 255, 0, 255, 0));
    EXPECT_EQ(screen.countColor({x0, y0, x1 + 1, y1 + 1}, 255, 0, 255, 8), 0u);
    // the text and the rotated edge at the screen's resolution: 2 x 2 blocks that are not one colour
    size_t mixed = 0;
    for (int y = y0; y + 1 <= y1; y += 2){
        for (int x = x0; x + 1 <= x1; x += 2){
            const uint8_t* a = screen.at(x, y);
            for (const uint8_t* b : {screen.at(x + 1, y), screen.at(x, y + 1), screen.at(x + 1, y + 1)}){
                if (std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) > 24){
                    mixed++;
                    break;
                }
            }
        }
    }
    EXPECT_GT(mixed, 200u);
}

// R15 (ledger R57): a game's orthographic camera 22 world units high shows the documents in
// screen space; before S92 it drew the canvas rectangle (0,0)-(640,360) of world space through
// the camera, almost all of it outside the view
TEST_F(HtmlUIScene, OrthoCameraFillsViewport){
    SceneFixture fixture;
    Camera& camera = fixture.getCamera();
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.setAutoResize(false);
    camera.setOrtho(-19.5555f, 19.5555f, -11.0f, 11.0f, -100.0f, 100.0f);
    fixture.addDocument("golden/overlay_detail.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Screen screen;
    ASSERT_TRUE(capture(screen));
    expectOverlayDetail(screen);
    EXPECT_TRUE(screen.is(320, 300, 0x30, 0x60, 0xc0, 0));
}

// R15: a UI camera panned away from the canvas still shows the documents at screen pixels
TEST_F(HtmlUIScene, UiCameraPannedFillsViewport){
    SceneFixture fixture;
    fixture.getCamera().setPosition(200.0f, 120.0f, fixture.getCamera().getPosition().z);
    fixture.addDocument("golden/overlay_detail.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Screen screen;
    ASSERT_TRUE(capture(screen));
    expectOverlayDetail(screen);
}

// R15 is Screen Space - Overlay's: a Screen Space - Camera scene (an older scene, which its
// migration gives that mode) keeps the placement it had before S92 in a game view, the canvas
// through its UI camera, so a camera panned by (200, 120) shows 440 x 240 px of the 640 x 360
// document, not all of it at screen pixels
TEST_F(HtmlUIScene, CameraModeKeepsTheDocumentsInTheCanvasOfItsCamera){
    // the bounding box of the document's background (#3060c0) on the screen: width and height
    auto background = [](const Screen& screen){
        int x0 = screen.width, y0 = screen.height, x1 = -1, y1 = -1;
        for (int y = 0; y < screen.height; y++)
            for (int x = 0; x < screen.width; x++)
                if (screen.is(x, y, 0x30, 0x60, 0xc0, 2)){
                    x0 = std::min(x0, x); y0 = std::min(y0, y);
                    x1 = std::max(x1, x); y1 = std::max(y1, y);
                }
        return x1 < 0 ? std::pair<int, int>(0, 0) : std::pair<int, int>(x1 - x0 + 1, y1 - y0 + 1);
    };
    SceneFixture fixture;
    fixture.getScene().setHtmlUiRenderMode(UIRenderMode::CAMERA);
    fixture.addDocument("golden/overlay_detail.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Screen still;
    ASSERT_TRUE(capture(still));
    ASSERT_EQ(background(still), std::make_pair(640, 360)) << "the canvas the unmoved camera shows is the view";

    // a pan: the position and the target move together
    Camera& camera = fixture.getCamera();
    const Vector3 position = camera.getPosition(), target = camera.getTarget();
    camera.setPosition(position.x + 200.0f, position.y + 120.0f, position.z);
    camera.setTarget(target.x + 200.0f, target.y + 120.0f, target.z);
    Screen panned;
    ASSERT_TRUE(capture(panned, 2));
    EXPECT_EQ(background(panned), std::make_pair(440, 240)) << "the documents did not move with the camera (R15 filled the viewport)";

    // the same camera in Screen Space - Overlay: the viewport (UiCameraPannedFillsViewport)
    fixture.getScene().setHtmlUiRenderMode(UIRenderMode::OVERLAY);
    Screen overlay;
    ASSERT_TRUE(capture(overlay, 2));
    EXPECT_EQ(background(overlay), std::make_pair(640, 360));
    expectOverlayDetail(overlay);
}

// R17-R19: the scene's MSAA count drives its screen context; a texture-mode document's own
// count (else the scene's) drives its context; 0 is the project's
TEST_F(HtmlUIScene, MsaaFollowsSceneAndDocument){
    SceneFixture fixture;
    fixture.addDocument("golden/overlay_detail.rml");
    UIDocument texture = fixture.addDocument("golden/sort_a.rml");
    UIDocumentComponent& component = texture.getComponent<UIDocumentComponent>();
    component.renderMode = UIDocumentRenderMode::TEXTURE;
    component.textureWidth = 128;
    component.textureHeight = 128;
    ASSERT_TRUE(fixture.waitUntilLoaded());
    UIDocumentSystem& documents = *fixture.getScene().getSystem<UIDocumentSystem>();
    HtmlUIContext* screen = documents.getContext();
    HtmlUIContext* own = documents.getTextureContext(texture.getEntity());
    ASSERT_NE(screen, nullptr);
    ASSERT_NE(own, nullptr);
    const int project = UIDocumentSystem::getPreferredSampleCount();
    EXPECT_EQ(screen->getRequestedSampleCount(), project);
    EXPECT_EQ(own->getRequestedSampleCount(), project);

    fixture.getScene().setHtmlUiMsaaSamples(1);
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(screen->getRequestedSampleCount(), 1);
    EXPECT_EQ(screen->getRenderTarget()->getSampleCount(), 1) << "the next frame makes the target again";
    EXPECT_EQ(own->getRequestedSampleCount(), 1) << "Scene Default";

    component.msaaSamples = 2;
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(own->getRequestedSampleCount(), 2);
    EXPECT_EQ(own->getSampleCount(), std::max(FramebufferRender::getMaxMsaaSamples(2), 1));
    EXPECT_EQ(screen->getRequestedSampleCount(), 1);

    fixture.getScene().setHtmlUiMsaaSamples(0);
    component.msaaSamples = 0;
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(screen->getRequestedSampleCount(), project);
    EXPECT_EQ(own->getRequestedSampleCount(), project);
}

// R16: Auto is one sample on phones and the web, four on desktop
TEST(HtmlUIContext, AutoSampleCountPerPlatform){
    EXPECT_EQ(HtmlUIContext::autoSampleCount(Platform::Android), 1);
    EXPECT_EQ(HtmlUIContext::autoSampleCount(Platform::iOS), 1);
    EXPECT_EQ(HtmlUIContext::autoSampleCount(Platform::Web), 1);
    EXPECT_EQ(HtmlUIContext::autoSampleCount(Platform::Windows), 4);
    EXPECT_EQ(HtmlUIContext::autoSampleCount(Platform::Linux), 4);
    EXPECT_EQ(HtmlUIContext::autoSampleCount(Platform::MacOS), 4);

    HtmlUIContext context("auto-samples");
    const int initial = UIDocumentSystem::getPreferredSampleCount();
    UIDocumentSystem::setPreferredSampleCount(0);
    EXPECT_EQ(context.getRequestedSampleCount(), HtmlUIContext::autoSampleCount(Engine::getPlatform()));
    context.setSampleCountOverride(2);
    EXPECT_EQ(context.getRequestedSampleCount(), 2);
    context.setSampleCountOverride(3);
    EXPECT_EQ(context.getSampleCountOverride(), 2) << "a count goes to the one below it";
    context.setSampleCountOverride(0);
    UIDocumentSystem::setPreferredSampleCount(8);
    EXPECT_EQ(context.getRequestedSampleCount(), 8);
    UIDocumentSystem::setPreferredSampleCount(initial);
}
