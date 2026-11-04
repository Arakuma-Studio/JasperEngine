// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Canvas Scaler (docs/superpowers/specs/2026-10-31-ui-scaling-text-design.md §7.1, §14): Unity's
// formulas, the scene's result in a view, UI layout and pointer coordinates in UI units, the HTML
// UI documents' layout size, serialization of the project and scene settings and the code the
// exports generate.

#include <gtest/gtest.h>
#include "camera/CameraType.h"

#include "Engine.h"
#include "Scene.h"
#include "System.h"
#include "util/CanvasScaler.h"
#include "util/CanvasScalerRef.h"
#include "component/CameraComponent.h"
#include "component/RectTransformComponent.h"
#include "component/ImageComponent.h"
#include "component/UIComponent.h"
#include "component/Transform.h"
#include "subsystem/UISystem.h"
#include "subsystem/UIDocumentSystem.h"

#include "Catalog.h"
#include "Factory.h"
#include "Stream.h"
#include "mcp/McpValues.h"

#include <cmath>

namespace jasperengine {
    // TEST-ONLY seam (System.h befriends it): detach / restore the instance
    struct SystemTestAccess {
        static System* swap(System* system) {
            System* previous = System::systemInstance;
            System::systemInstance = system;
            return previous;
        }
    };
}

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    CanvasScalerSettings scaleWithScreenSize(float width, float height, ScreenMatchMode matchMode, float match = 0.0f) {
        CanvasScalerSettings settings;
        settings.mode = UIScaleMode::ScaleWithScreenSize;
        settings.referenceResolution = Vector2(width, height);
        settings.screenMatchMode = matchMode;
        settings.matchWidthOrHeight = match;
        return settings;
    }

    // a screen of a given size and dpi
    class ScreenSystem : public System {
    public:
        int width = 2400;
        int height = 1080;
        float dpi = 0.0f;
        int getScreenWidth() override { return width; }
        int getScreenHeight() override { return height; }
        float getScreenDpi() override { return dpi; }
    };

    // a scene's camera as a loaded UI scene has one (a bare Scene has none before load())
    Entity addCamera(Scene& scene, CameraType type = CameraType::CAMERA_UI){
        Entity camera = scene.createEntity();
        scene.addComponent<Transform>(camera, Transform());
        CameraComponent component;
        component.type = type;
        scene.addComponent<CameraComponent>(camera, component);
        scene.setCamera(camera);
        return camera;
    }

    // Installs a ScreenSystem and puts the Engine's display settings back after the test
    class CanvasScalerViewTest : public ::testing::Test {
    protected:
        ScreenSystem system;
        System* previous = nullptr;

        void SetUp() override {
            previous = SystemTestAccess::swap(&system);
            Engine::setCanvasSize(1920, 1080);
            Engine::setScalingMode(Scaling::FITHEIGHT);
            Engine::setDefaultCanvasScaler(CanvasScalerSettings());
        }

        void TearDown() override {
            Engine::setDefaultCanvasScaler(CanvasScalerSettings());
            Engine::setScalingMode(Scaling::FITWIDTH);
            Engine::setCanvasSize(1000, 480);
            system.width = 0;
            system.height = 0;
            Engine::systemViewChanged();
            SystemTestAccess::swap(previous);
        }

        void resize(int width, int height) {
            system.width = width;
            system.height = height;
            Engine::systemViewChanged();
        }
    };

}

// ---- formulas (Unity's CanvasScaler) ------------------------------------------------------------

TEST(CanvasScaler, MatchWidthOrHeightBlendsInLogSpace) {
    // 2400x1080 against 1920x1080: width ratio 1.25, height ratio 1
    CanvasScalerResult width = CanvasScaler::compute(scaleWithScreenSize(1920, 1080, ScreenMatchMode::MatchWidthOrHeight, 0.0f), 2400, 1080, 2400, 1080, 0);
    CanvasScalerResult half = CanvasScaler::compute(scaleWithScreenSize(1920, 1080, ScreenMatchMode::MatchWidthOrHeight, 0.5f), 2400, 1080, 2400, 1080, 0);
    CanvasScalerResult height = CanvasScaler::compute(scaleWithScreenSize(1920, 1080, ScreenMatchMode::MatchWidthOrHeight, 1.0f), 2400, 1080, 2400, 1080, 0);

    EXPECT_NEAR(width.scaleFactor, 1.25f, 1e-5f);
    EXPECT_NEAR(half.scaleFactor, std::sqrt(1.25f), 1e-5f);   // 1.118
    EXPECT_NEAR(height.scaleFactor, 1.0f, 1e-5f);
    EXPECT_NEAR(width.uiWidth, 1920.0f, 1e-3f);
    EXPECT_NEAR(width.uiHeight, 864.0f, 1e-3f);
    EXPECT_NEAR(height.uiWidth, 2400.0f, 1e-3f);
    EXPECT_NEAR(height.uiHeight, 1080.0f, 1e-3f);
    EXPECT_TRUE(width.scaled);

    // match is clamped
    CanvasScalerResult over = CanvasScaler::compute(scaleWithScreenSize(1920, 1080, ScreenMatchMode::MatchWidthOrHeight, 3.0f), 2400, 1080, 2400, 1080, 0);
    EXPECT_NEAR(over.scaleFactor, 1.0f, 1e-5f);
}

TEST(CanvasScaler, ExpandNeverMakesTheUISmallerThanTheReference) {
    // 4:3: matches the width
    CanvasScalerResult tablet = CanvasScaler::compute(scaleWithScreenSize(1920, 1080, ScreenMatchMode::Expand), 1440, 1080, 1440, 1080, 0);
    EXPECT_NEAR(tablet.scaleFactor, 0.75f, 1e-5f);
    EXPECT_NEAR(tablet.uiWidth, 1920.0f, 1e-3f);
    EXPECT_NEAR(tablet.uiHeight, 1440.0f, 1e-3f);

    // 20:9: matches the height
    CanvasScalerResult phone = CanvasScaler::compute(scaleWithScreenSize(1920, 1080, ScreenMatchMode::Expand), 2400, 1080, 2400, 1080, 0);
    EXPECT_NEAR(phone.scaleFactor, 1.0f, 1e-5f);
    EXPECT_NEAR(phone.uiWidth, 2400.0f, 1e-3f);
    EXPECT_NEAR(phone.uiHeight, 1080.0f, 1e-3f);

    // any aspect: both sides at least the reference
    for (float aspect : {0.4f, 0.75f, 1.0f, 1.777f, 2.2f, 3.0f}) {
        CanvasScalerResult r = CanvasScaler::compute(scaleWithScreenSize(1920, 1080, ScreenMatchMode::Expand), 1000.0f * aspect, 1000.0f, 1, 1, 0);
        EXPECT_GE(r.uiWidth, 1920.0f - 1e-2f);
        EXPECT_GE(r.uiHeight, 1080.0f - 1e-2f);
    }
}

TEST(CanvasScaler, ShrinkNeverMakesTheUILargerThanTheReference) {
    CanvasScalerResult tablet = CanvasScaler::compute(scaleWithScreenSize(1920, 1080, ScreenMatchMode::Shrink), 1440, 1080, 1440, 1080, 0);
    EXPECT_NEAR(tablet.scaleFactor, 1.0f, 1e-5f);
    EXPECT_NEAR(tablet.uiWidth, 1440.0f, 1e-3f);
    EXPECT_NEAR(tablet.uiHeight, 1080.0f, 1e-3f);

    CanvasScalerResult phone = CanvasScaler::compute(scaleWithScreenSize(1920, 1080, ScreenMatchMode::Shrink), 2400, 1080, 2400, 1080, 0);
    EXPECT_NEAR(phone.scaleFactor, 1.25f, 1e-5f);
    EXPECT_NEAR(phone.uiWidth, 1920.0f, 1e-3f);
    EXPECT_NEAR(phone.uiHeight, 864.0f, 1e-3f);
}

TEST(CanvasScaler, ConstantPixelSizeAndPhysicalSize) {
    CanvasScalerSettings pixel;
    pixel.mode = UIScaleMode::ConstantPixelSize;
    pixel.scaleFactor = 2.0f;
    CanvasScalerResult doubled = CanvasScaler::compute(pixel, 2400, 1080, 2400, 1080, 0);
    EXPECT_FLOAT_EQ(doubled.scaleFactor, 2.0f);
    EXPECT_FLOAT_EQ(doubled.uiWidth, 1200.0f);
    EXPECT_FLOAT_EQ(doubled.uiHeight, 540.0f);

    pixel.scaleFactor = 0.0f;   // never below 0.01
    EXPECT_FLOAT_EQ(CanvasScaler::compute(pixel, 100, 100, 100, 100, 0).scaleFactor, 0.01f);

    CanvasScalerSettings physical;
    physical.mode = UIScaleMode::ConstantPhysicalSize;
    physical.physicalUnit = PhysicalUnit::Points;
    // an iPhone's 326 dpi: 4.528 pixels per point
    EXPECT_NEAR(CanvasScaler::compute(physical, 750, 1334, 750, 1334, 326.0f).scaleFactor, 326.0f / 72.0f, 1e-4f);
    EXPECT_NEAR(CanvasScaler::compute(physical, 750, 1334, 750, 1334, 326.0f).scaleFactor, 4.528f, 1e-3f);

    // unknown dpi: the fallback
    physical.fallbackScreenDPI = 144.0f;
    EXPECT_NEAR(CanvasScaler::compute(physical, 750, 1334, 750, 1334, 0.0f).scaleFactor, 2.0f, 1e-5f);

    physical.physicalUnit = PhysicalUnit::Inches;
    EXPECT_NEAR(CanvasScaler::compute(physical, 750, 1334, 750, 1334, 300.0f).scaleFactor, 300.0f, 1e-3f);
    physical.physicalUnit = PhysicalUnit::Centimeters;
    EXPECT_NEAR(CanvasScaler::compute(physical, 750, 1334, 750, 1334, 254.0f).scaleFactor, 100.0f, 1e-3f);
    physical.physicalUnit = PhysicalUnit::Millimeters;
    EXPECT_NEAR(CanvasScaler::compute(physical, 750, 1334, 750, 1334, 254.0f).scaleFactor, 10.0f, 1e-3f);
    physical.physicalUnit = PhysicalUnit::Picas;
    EXPECT_NEAR(CanvasScaler::compute(physical, 750, 1334, 750, 1334, 72.0f).scaleFactor, 12.0f, 1e-3f);
}

TEST(CanvasScaler, InvalidInputsKeepTheLastResult) {
    const CanvasScalerSettings settings = scaleWithScreenSize(1920, 1080, ScreenMatchMode::Expand);
    const CanvasScalerResult good = CanvasScaler::compute(settings, 2400, 1080, 2400, 1080, 0);

    CanvasScalerResult kept = CanvasScaler::compute(settings, 0, 1080, 2400, 1080, 0, good);
    EXPECT_FLOAT_EQ(kept.uiWidth, good.uiWidth);
    EXPECT_FLOAT_EQ(kept.scaleFactor, good.scaleFactor);
    kept = CanvasScaler::compute(settings, NAN, 1080, 2400, 1080, 0, good);
    EXPECT_FLOAT_EQ(kept.uiWidth, good.uiWidth);
    kept = CanvasScaler::compute(scaleWithScreenSize(0, 1080, ScreenMatchMode::Expand), 2400, 1080, 2400, 1080, 0, good);
    EXPECT_FLOAT_EQ(kept.uiWidth, good.uiWidth);
}

TEST(CanvasScaler, FollowGameCanvasIsTheGameCanvas) {
    // a STRETCH game canvas: another factor per axis
    CanvasScalerResult result = CanvasScaler::compute(CanvasScalerSettings(), 2400, 1080, 1280, 720, 0);
    EXPECT_FALSE(result.scaled);
    EXPECT_FLOAT_EQ(result.uiWidth, 1280.0f);
    EXPECT_FLOAT_EQ(result.uiHeight, 720.0f);
    EXPECT_FLOAT_EQ(result.scaleFactor, 2400.0f / 1280.0f);
    EXPECT_FLOAT_EQ(result.scaleFactorY, 1080.0f / 720.0f);
}

TEST(CanvasScaler, NamesRoundTrip) {
    for (UIScaleMode mode : {UIScaleMode::FollowGameCanvas, UIScaleMode::ConstantPixelSize, UIScaleMode::ScaleWithScreenSize,
                             UIScaleMode::ConstantPhysicalSize}) {
        UIScaleMode parsed = UIScaleMode::FollowGameCanvas;
        EXPECT_TRUE(CanvasScaler::parseMode(CanvasScaler::modeName(mode), parsed));
        EXPECT_EQ(parsed, mode);
    }
    ScreenMatchMode match;
    EXPECT_TRUE(CanvasScaler::parseMatchMode("expand", match));
    EXPECT_EQ(match, ScreenMatchMode::Expand);
    PhysicalUnit unit;
    EXPECT_TRUE(CanvasScaler::parseUnit("picas", unit));
    EXPECT_EQ(unit, PhysicalUnit::Picas);
    UIScaleMode untouched = UIScaleMode::ConstantPixelSize;
    EXPECT_FALSE(CanvasScaler::parseMode("bogus", untouched));
    EXPECT_EQ(untouched, UIScaleMode::ConstantPixelSize);
}

// ---- the scene in a view ----------------------------------------------------------------------

TEST_F(CanvasScalerViewTest, AUIScenesCameraAndLayoutSpanTheUISize) {
    Scene scene;   // its default camera is a UI camera
    addCamera(scene);
    scene.setCanvasScaler(scaleWithScreenSize(1920, 1080, ScreenMatchMode::Expand));
    scene.setCanvasScalerUseProjectDefault(false);

    resize(1440, 1080);
    scene.updateCameraSize();

    EXPECT_TRUE(scene.isUIScaled());
    EXPECT_NEAR(scene.getCanvasScale().uiWidth, 1920.0f, 1e-3f);
    EXPECT_NEAR(scene.getCanvasScale().uiHeight, 1440.0f, 1e-3f);

    Vector2 canvas = scene.getSystem<UISystem>()->getCanvasSize();
    EXPECT_NEAR(canvas.x, 1920.0f, 1e-3f);
    EXPECT_NEAR(canvas.y, 1440.0f, 1e-3f);

    CameraComponent& camera = scene.getComponent<CameraComponent>(scene.getCamera());
    EXPECT_NEAR(camera.rightClip, 1920.0f, 1e-3f);
    EXPECT_NEAR(camera.topClip, 1440.0f, 1e-3f);

    // the game canvas (FITHEIGHT 1080 high) maps onto the UI size
    EXPECT_EQ(Engine::getCanvasHeight(), 1080);
    EXPECT_EQ(Engine::getCanvasWidth(), 1440);
    Vector2 center = scene.canvasToUI(Vector2(720.0f, 540.0f));
    EXPECT_NEAR(center.x, 960.0f, 1e-3f);
    EXPECT_NEAR(center.y, 720.0f, 1e-3f);
    Vector2 back = scene.uiToCanvas(center);
    EXPECT_NEAR(back.x, 720.0f, 1e-3f);
    EXPECT_NEAR(back.y, 540.0f, 1e-3f);

    // 20:9
    resize(2400, 1080);
    scene.updateCameraSize();
    EXPECT_NEAR(scene.getCanvasScale().uiWidth, 2400.0f, 1e-3f);
    EXPECT_NEAR(scene.getCanvasScale().uiHeight, 1080.0f, 1e-3f);

    // an explicit reference size (the editor's) wins
    scene.getSystem<UISystem>()->setAnchorReferenceSize(1000, 500);
    EXPECT_NEAR(scene.getSystem<UISystem>()->getCanvasSize().x, 1000.0f, 1e-3f);
    scene.getSystem<UISystem>()->clearAnchorReferenceSize();
}

TEST_F(CanvasScalerViewTest, FollowGameCanvasChangesNothing) {
    Scene scene;
    addCamera(scene);
    resize(2400, 1080);
    scene.updateCameraSize();

    EXPECT_FALSE(scene.isUIScaled());
    EXPECT_FALSE(scene.isDocumentScaled());
    Vector2 canvas = scene.getSystem<UISystem>()->getCanvasSize();
    EXPECT_FLOAT_EQ(canvas.x, (float)Engine::getCanvasWidth());
    EXPECT_FLOAT_EQ(canvas.y, (float)Engine::getCanvasHeight());
    EXPECT_FLOAT_EQ(scene.getSystem<UISystem>()->getTextRasterScale(), 1.0f);
    Vector2 point = scene.canvasToUI(Vector2(10.0f, 20.0f));
    EXPECT_FLOAT_EQ(point.x, 10.0f);
    EXPECT_FLOAT_EQ(point.y, 20.0f);
}

TEST_F(CanvasScalerViewTest, TheProjectDefaultAppliesUnlessTheSceneOverrides) {
    Scene scene;
    addCamera(scene);
    Engine::setDefaultCanvasScaler(scaleWithScreenSize(1920, 1080, ScreenMatchMode::Expand));
    resize(1440, 1080);
    scene.updateCameraSize();
    EXPECT_TRUE(scene.isCanvasScalerUseProjectDefault());
    EXPECT_NEAR(scene.getCanvasScale().uiHeight, 1440.0f, 1e-3f);

    CanvasScalerSettings own;
    own.mode = UIScaleMode::ConstantPixelSize;
    own.scaleFactor = 2.0f;
    scene.setCanvasScaler(own);
    scene.updateCameraSize();
    EXPECT_NEAR(scene.getCanvasScale().uiHeight, 1440.0f, 1e-3f);   // still the default
    scene.setCanvasScalerUseProjectDefault(false);
    scene.updateCameraSize();
    EXPECT_NEAR(scene.getCanvasScale().uiHeight, 540.0f, 1e-3f);
    EXPECT_FLOAT_EQ(scene.getSystem<UISystem>()->getTextRasterScale(), 2.0f);
}

TEST_F(CanvasScalerViewTest, ScriptsEditTheSceneFromTheDefault) {
    Scene scene;
    addCamera(scene);
    Engine::setDefaultCanvasScaler(scaleWithScreenSize(1280, 720, ScreenMatchMode::Expand));
    resize(1440, 1080);
    scene.updateCameraSize();

    CanvasScalerRef ref(&scene);
    EXPECT_TRUE(ref.isUseProjectDefault());
    EXPECT_EQ(ref.getUIScaleMode(), UIScaleMode::ScaleWithScreenSize);
    EXPECT_FLOAT_EQ(ref.getReferenceResolution().x, 1280.0f);

    // a field set on a scene using the default overrides from that default
    ref.setScreenMatchMode(ScreenMatchMode::Shrink);
    EXPECT_FALSE(ref.isUseProjectDefault());
    EXPECT_FLOAT_EQ(scene.getCanvasScaler().referenceResolution.x, 1280.0f);
    EXPECT_EQ(scene.getCanvasScaler().screenMatchMode, ScreenMatchMode::Shrink);
    scene.updateCameraSize();
    // Shrink at 1440x1080 against 1280x720: max(1.125, 1.5)
    EXPECT_NEAR(ref.getCanvasSize().x, 960.0f, 1e-3f);
    EXPECT_NEAR(ref.getCanvasSize().y, 720.0f, 1e-3f);
    EXPECT_NEAR(ref.getAppliedScaleFactor(), 1.5f, 1e-4f);
}

TEST_F(CanvasScalerViewTest, StretchAnchorsFillTheUISizeAndPointersArriveInUIUnits) {
    Scene scene;
    addCamera(scene);
    scene.setCanvasScaler(scaleWithScreenSize(1920, 1080, ScreenMatchMode::Expand));
    scene.setCanvasScalerUseProjectDefault(false);
    resize(1440, 1080);
    scene.updateCameraSize();

    // a full-stretch panel, and a 200x100 image centred in UI units
    Entity panel = scene.createEntity();
    scene.addComponent<Transform>(panel, Transform());
    RectTransformComponent fill;
    fill.usingAnchors = true;
    fill.anchorPreset = AnchorPreset::FULL_LAYOUT;
    scene.addComponent<RectTransformComponent>(panel, fill);

    Entity button = scene.createEntity();
    scene.addComponent<Transform>(button, Transform());
    scene.addComponent<UIComponent>(button, UIComponent());
    scene.addComponent<ImageComponent>(button, ImageComponent());
    RectTransformComponent centred;
    centred.usingAnchors = true;
    centred.anchorPreset = AnchorPreset::CENTER;
    centred.width = 200;
    centred.height = 100;
    scene.addComponent<RectTransformComponent>(button, centred);

    scene.update(0.0);
    scene.update(0.0);

    RectTransformComponent& panelLayout = scene.getComponent<RectTransformComponent>(panel);
    EXPECT_NEAR(panelLayout.width, 1920.0f, 1.0f);
    EXPECT_NEAR(panelLayout.height, 1440.0f, 1.0f);

    // the view's centre in game canvas pixels (1440x1080) is the UI's centre (960, 720)
    UISystem* ui = scene.getSystem<UISystem>().get();
    EXPECT_TRUE(ui->eventOnPointerDown(720.0f, 540.0f));
    ui->eventOnPointerUp(720.0f, 540.0f);
    // its corner in canvas pixels is outside the image
    EXPECT_FALSE(ui->eventOnPointerDown(10.0f, 10.0f));
    ui->eventOnPointerUp(10.0f, 10.0f);
}

TEST_F(CanvasScalerViewTest, ScreenDocumentsLayOutAtTheUISize) {
    Scene scene;
    addCamera(scene);
    scene.setCanvasScaler(scaleWithScreenSize(1920, 1080, ScreenMatchMode::Expand));
    scene.setCanvasScalerUseProjectDefault(false);
    resize(1440, 1080);
    scene.updateCameraSize();
    EXPECT_TRUE(scene.isDocumentScaled());

    // a 2D scene's documents scale too, still covering the game canvas
    Scene world;
    addCamera(world, CameraType::CAMERA_ORTHO);
    CameraComponent& camera = world.getComponent<CameraComponent>(world.getCamera());
    world.setCanvasScaler(scaleWithScreenSize(1920, 1080, ScreenMatchMode::Expand));
    world.setCanvasScalerUseProjectDefault(false);
    world.updateCameraSize();
    EXPECT_FALSE(world.isUIScaled());
    EXPECT_TRUE(world.isDocumentScaled());
    // the 2D camera keeps the game canvas
    EXPECT_NEAR(camera.rightClip, (float)Engine::getCanvasWidth(), 1e-3f);
}

// ---- serialization, MCP values and generated code -------------------------------------------

TEST(CanvasScalerSettingsIO, ProjectAndSceneRoundTripAndDefaultsAreNotWritten) {
    const CanvasScalerSettings defaults;
    YAML::Node empty = Stream::encodeCanvasScaler(defaults);
    EXPECT_EQ(empty.size(), 0u);

    CanvasScalerSettings settings = scaleWithScreenSize(1920, 1080, ScreenMatchMode::Expand);
    settings.matchWidthOrHeight = 0.25f;
    settings.physicalUnit = PhysicalUnit::Millimeters;
    YAML::Node node = Stream::encodeCanvasScaler(settings);
    EXPECT_EQ(node["mode"].as<std::string>(), "scale_with_screen_size");
    EXPECT_EQ(node["screenMatchMode"].as<std::string>(), "expand");
    EXPECT_FALSE(node["fallbackScreenDPI"]);
    EXPECT_EQ(Stream::decodeCanvasScaler(node), settings);
    // absent: the defaults
    EXPECT_EQ(Stream::decodeCanvasScaler(YAML::Node()), defaults);

    // a scene of before the Canvas Scaler writes no canvasScaler key
    Scene scene;
    YAML::Node plain = Stream::encodeScene(&scene);
    EXPECT_FALSE(plain["canvasScaler"]);

    scene.setCanvasScaler(settings);
    scene.setCanvasScalerUseProjectDefault(false);
    YAML::Node written = Stream::encodeScene(&scene);
    ASSERT_TRUE(written["canvasScaler"]);
    EXPECT_FALSE(written["canvasScaler"]["useProjectDefault"].as<bool>());

    Scene loaded;
    Stream::decodeScene(&loaded, written);
    EXPECT_EQ(loaded.getCanvasScaler(), settings);
    EXPECT_FALSE(loaded.isCanvasScalerUseProjectDefault());

    // decoding a scene without the key resets a reused scene (play restore)
    Stream::decodeScene(&loaded, plain);
    EXPECT_EQ(loaded.getCanvasScaler(), defaults);
    EXPECT_TRUE(loaded.isCanvasScalerUseProjectDefault());
}

TEST(CanvasScalerSettingsIO, McpObjectsPatchTheirFields) {
    CanvasScalerSettings settings;
    std::string error;
    ASSERT_TRUE(mcp::McpValues::canvasScalerFromJson(mcp::Json{{"mode", "scale_with_screen_size"}, {"reference", {1920, 1080}}, {"match_mode", "expand"}},
                                                settings, error)) << error;
    EXPECT_EQ(settings.mode, UIScaleMode::ScaleWithScreenSize);
    EXPECT_EQ(settings.screenMatchMode, ScreenMatchMode::Expand);
    EXPECT_FLOAT_EQ(settings.referenceResolution.y, 1080.0f);

    ASSERT_TRUE(mcp::McpValues::canvasScalerFromJson(mcp::Json{{"match", 0.5}}, settings, error));
    EXPECT_FLOAT_EQ(settings.matchWidthOrHeight, 0.5f);
    EXPECT_EQ(settings.screenMatchMode, ScreenMatchMode::Expand);   // kept

    const CanvasScalerSettings before = settings;
    EXPECT_FALSE(mcp::McpValues::canvasScalerFromJson(mcp::Json{{"match", 2}}, settings, error));
    EXPECT_FALSE(mcp::McpValues::canvasScalerFromJson(mcp::Json{{"mode", "fit"}}, settings, error));
    EXPECT_NE(error.find("follow_game_canvas"), std::string::npos);
    EXPECT_FALSE(mcp::McpValues::canvasScalerFromJson(mcp::Json{{"refrence", {1, 1}}}, settings, error));
    EXPECT_NE(error.find("unknown key"), std::string::npos);
    EXPECT_EQ(settings, before);   // a failed patch changes nothing

    mcp::Json json = mcp::McpValues::canvasScalerToJson(settings);
    CanvasScalerSettings parsed;
    ASSERT_TRUE(mcp::McpValues::canvasScalerFromJson(json, parsed, error)) << error;
    EXPECT_EQ(parsed, settings);
}

TEST(CanvasScalerSettingsIO, GeneratedCodeHasOnlyTheChangedFields) {
    EXPECT_EQ(Factory::formatCanvasScaler(CanvasScalerSettings()), "[](){ CanvasScalerSettings s; return s; }()");
    const std::string code = Factory::formatCanvasScaler(scaleWithScreenSize(1920, 1080, ScreenMatchMode::Expand));
    EXPECT_NE(code.find("s.mode = UIScaleMode::ScaleWithScreenSize;"), std::string::npos);
    EXPECT_NE(code.find("s.screenMatchMode = ScreenMatchMode::Expand;"), std::string::npos);
    EXPECT_EQ(code.find("referenceResolution"), std::string::npos);   // the default 1920x1080
    EXPECT_EQ(code.find("fallbackScreenDPI"), std::string::npos);
}

TEST(CanvasScalerSettingsIO, TheSceneSettingIsOneCatalogProperty) {
    Scene scene;
    SceneCanvasScaler value = Catalog::getSceneProperty<SceneCanvasScaler>(&scene, "canvas_scaler");
    EXPECT_TRUE(value.useProjectDefault);
    EXPECT_EQ(value, Catalog::getScenePropertyDefault<SceneCanvasScaler>("canvas_scaler"));

    value.useProjectDefault = false;
    value.settings.mode = UIScaleMode::ConstantPixelSize;
    Catalog::setSceneProperty<SceneCanvasScaler>(&scene, "canvas_scaler", value);
    EXPECT_FALSE(scene.isCanvasScalerUseProjectDefault());
    EXPECT_EQ(scene.getCanvasScaler().mode, UIScaleMode::ConstantPixelSize);
}
