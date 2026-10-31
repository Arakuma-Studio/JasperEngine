// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Safe area (docs/superpowers/specs/2026-10-31-safe-area-design.md §9): Screen's mapping of the
// platform's pixel insets to the canvas for every scaling mode, the app's extra insets, the
// change event and its order, cutouts, the Safe Area component's anchors, its serialization and
// exported code, the editor's device profiles and their overlay, and the desktop simulation
// variable. The platforms' own code runs on devices (Android/Web compile checks).

#include <gtest/gtest.h>

#include "Engine.h"
#include "Screen.h"
#include "System.h"
#include "Scene.h"
#include "subsystem/UISystem.h"
#include "component/SafeAreaComponent.h"
#include "component/RectTransformComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"

#include "Catalog.h"
#include "Factory.h"
#include "Stream.h"
#include "project/ProjectPrefabInternal.h"
#include "util/ProjectUtils.h"
#include "window/DeviceProfiles.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace jasperengine {
    // TEST-ONLY seam (System.h befriends it): detach / restore the instance
    struct SafeAreaTestAccess {
        static System* swap(System* system) {
            System* previous = System::systemInstance;
            System::systemInstance = system;
            return previous;
        }
    };
}

using namespace jasperengine;

namespace {

    class ScreenSystem : public System {
    public:
        int width = 1000;
        int height = 2000;
        int getScreenWidth() override { return width; }
        int getScreenHeight() override { return height; }
    };

    // a System of a given screen, the Engine's canvas and Screen's safe area reset around each test
    class SafeAreaTest : public ::testing::Test {
    protected:
        ScreenSystem system;
        System* previous = nullptr;
        int previousCanvasWidth = 0;
        int previousCanvasHeight = 0;
        Scaling previousScaling = Scaling::FITWIDTH;

        void SetUp() override {
            previous = SafeAreaTestAccess::swap(&system);
            previousCanvasWidth = Engine::getPreferredCanvasWidth();
            previousCanvasHeight = Engine::getPreferredCanvasHeight();
            previousScaling = Engine::getScalingMode();
            Engine::clearAllSubscriptions(true);
            Screen::resetSafeAreaState();
            Screen::setPlatformSafeArea(EdgeInsets());
        }

        void TearDown() override {
            Engine::clearAllSubscriptions(true);
            Screen::setPlatformSafeArea(EdgeInsets());
            Screen::resetSafeAreaState();
            Engine::setCanvasSize(previousCanvasWidth, previousCanvasHeight);
            Engine::setScalingMode(previousScaling);
            Engine::systemViewChanged();
            SafeAreaTestAccess::swap(previous);
        }

        // the screen, the project canvas and its scaling, as an exported game starts with them
        void view(int width, int height, int canvasWidth, int canvasHeight, Scaling scaling) {
            system.width = width;
            system.height = height;
            Engine::setCanvasSize(canvasWidth, canvasHeight);
            Engine::setScalingMode(scaling);
            Engine::systemViewChanged();
        }
    };

    SafeAreaInput inputFor(int width, int height, int canvasWidth, int canvasHeight, Scaling scaling, EdgeInsets insets) {
        SafeAreaInput input;
        input.screenWidth = width;
        input.screenHeight = height;
        int cw = 0, ch = 0;
        Engine::computeViewLayout(width, height, canvasWidth, canvasHeight, scaling, cw, ch, input.viewRect);
        input.canvasWidth = (float)cw;
        input.canvasHeight = (float)ch;
        input.insetsPixels = insets;
        return input;
    }

    void expectRect(const Rect& rect, float x, float y, float w, float h) {
        EXPECT_NEAR(rect.getX(), x, 1e-3f);
        EXPECT_NEAR(rect.getY(), y, 1e-3f);
        EXPECT_NEAR(rect.getWidth(), w, 1e-3f);
        EXPECT_NEAR(rect.getHeight(), h, 1e-3f);
    }

    void expectInsets(const EdgeInsets& insets, float left, float top, float right, float bottom) {
        EXPECT_NEAR(insets.left, left, 1e-3f);
        EXPECT_NEAR(insets.top, top, 1e-3f);
        EXPECT_NEAR(insets.right, right, 1e-3f);
        EXPECT_NEAR(insets.bottom, bottom, 1e-3f);
    }

}

// ---- 1. mapping for each scaling mode ---------------------------------------------------------

TEST(SafeAreaMapping, FitWidthAndFitHeightScaleTheInsets) {
    // 1000x2000 pixels, a 500-wide canvas: half a canvas unit per pixel
    const EdgeInsets notch(0, 100, 0, 50);
    SafeAreaResult r = Screen::computeSafeArea(inputFor(1000, 2000, 500, 1000, Scaling::FITWIDTH, notch));
    expectRect(r.safeArea, 0, 50, 500, 925);
    expectInsets(r.safeAreaInsets, 0, 50, 0, 25);
    expectRect(r.safeAreaPixels, 0, 100, 1000, 1850);

    r = Screen::computeSafeArea(inputFor(1000, 2000, 400, 1000, Scaling::FITHEIGHT, notch));
    expectRect(r.safeArea, 0, 50, 500, 925);
}

TEST(SafeAreaMapping, NativeIsPixelsAndStretchScalesEachAxis) {
    const EdgeInsets insets(20, 100, 40, 50);
    SafeAreaResult r = Screen::computeSafeArea(inputFor(1000, 2000, 500, 1000, Scaling::NATIVE, insets));
    expectRect(r.safeArea, 20, 100, 940, 1850);

    // 500x800 stretched over 1000x2000: 0.5 across, 0.4 down
    r = Screen::computeSafeArea(inputFor(1000, 2000, 500, 800, Scaling::STRETCH, insets));
    expectRect(r.safeArea, 10, 40, 470, 740);
    expectInsets(r.safeAreaInsets, 10, 40, 20, 20);
}

TEST(SafeAreaMapping, LetterboxBarsAbsorbTheInsets) {
    // a square canvas on a tall screen: 500-pixel bars above and below the 1000x1000 view
    SafeAreaResult r = Screen::computeSafeArea(inputFor(1000, 2000, 500, 500, Scaling::LETTERBOX, EdgeInsets(0, 100, 0, 50)));
    expectRect(r.safeArea, 0, 0, 500, 500);
    EXPECT_TRUE(r.safeAreaInsets.isZero());
    // partly inside the view: only that part counts
    r = Screen::computeSafeArea(inputFor(1000, 2000, 500, 500, Scaling::LETTERBOX, EdgeInsets(0, 600, 0, 0)));
    expectRect(r.safeArea, 0, 50, 500, 450);
}

TEST(SafeAreaMapping, CropAddsTheCroppedPart) {
    // a square canvas cropped on a tall screen: only its middle quarter-width columns... the
    // view is 2000 wide, 500 pixels of it off each side
    SafeAreaResult r = Screen::computeSafeArea(inputFor(1000, 2000, 500, 500, Scaling::CROP, EdgeInsets()));
    expectRect(r.safeArea, 125, 0, 250, 500);
    expectInsets(r.safeAreaInsets, 125, 0, 125, 0);
    expectRect(r.safeAreaPixels, 0, 0, 1000, 2000);
}

TEST(SafeAreaMapping, InsetsCoveringTheScreenUseTheWholeScreen) {
    SafeAreaResult r = Screen::computeSafeArea(inputFor(1000, 2000, 500, 1000, Scaling::FITWIDTH, EdgeInsets(0, 1500, 0, 600)));
    EXPECT_TRUE(r.deviceRectEmpty);
    expectRect(r.safeArea, 0, 0, 500, 1000);
}

TEST(SafeAreaMapping, NoViewYetIsTheWholeCanvas) {
    SafeAreaInput input;
    input.canvasWidth = 800;
    input.canvasHeight = 600;
    input.insetsPixels = EdgeInsets(10, 10, 10, 10);
    SafeAreaResult r = Screen::computeSafeArea(input);
    expectRect(r.safeArea, 0, 0, 800, 600);
}

// ---- 2. extra insets --------------------------------------------------------------------------

TEST(SafeAreaMapping, ExtrasAddToTheDeviceInsets) {
    SafeAreaInput input = inputFor(1000, 2000, 500, 1000, Scaling::FITWIDTH, EdgeInsets(0, 100, 0, 50));
    input.extraInsets = EdgeInsets(0, 0, 0, 30);
    SafeAreaResult r = Screen::computeSafeArea(input);
    expectRect(r.deviceSafeArea, 0, 50, 500, 925);
    expectRect(r.safeArea, 0, 50, 500, 895);
    expectInsets(r.safeAreaInsets, 0, 50, 0, 55);
    expectInsets(r.deviceSafeAreaInsets, 0, 50, 0, 25);
    expectInsets(r.extraInsets, 0, 0, 0, 30);

    // pixels at the current scale; negatives are 0
    input.extraInsets = EdgeInsets(-5, 0, 0, 100);
    input.extraInPixels = true;
    r = Screen::computeSafeArea(input);
    expectInsets(r.extraInsets, 0, 0, 0, 50);
}

TEST(SafeAreaMapping, OversizeExtrasKeepOneByOne) {
    SafeAreaInput input = inputFor(1000, 2000, 500, 1000, Scaling::FITWIDTH, EdgeInsets());
    input.extraInsets = EdgeInsets(400, 0, 400, 0);
    SafeAreaResult r = Screen::computeSafeArea(input);
    EXPECT_TRUE(r.extrasTooLarge);
    expectRect(r.safeArea, 249.5f, 0, 1, 1000);
}

TEST_F(SafeAreaTest, PixelExtrasFollowTheCanvasScale) {
    view(1000, 2000, 500, 1000, Scaling::FITWIDTH);
    Screen::setExtraInsets(EdgeInsets(0, 0, 0, 100), true);
    expectInsets(Screen::getExtraInsets(), 0, 0, 0, 50);
    // twice the pixels: half the canvas units
    view(2000, 4000, 500, 1000, Scaling::FITWIDTH);
    expectInsets(Screen::getExtraInsets(), 0, 0, 0, 25);
    expectRect(Screen::getSafeArea(), 0, 0, 500, 975);
    // canvas units do not
    Screen::setExtraInsets(EdgeInsets(0, 0, 0, 100));
    view(1000, 2000, 500, 1000, Scaling::FITWIDTH);
    expectInsets(Screen::getExtraInsets(), 0, 0, 0, 100);
    Screen::clearExtraInsets();
    EXPECT_TRUE(Screen::getExtraInsets().isZero());
}

// ---- 3. the event -----------------------------------------------------------------------------

TEST_F(SafeAreaTest, EventOncePerChangeAfterCanvasAndOrientation) {
    view(1000, 2000, 500, 1000, Scaling::FITWIDTH);
    std::vector<std::string> events;
    Engine::onViewChanged.add("test_canvas", [&events]() { events.push_back("canvas"); });
    Engine::onOrientationChanged.add("test_orientation", [&events](int) { events.push_back("orientation"); });
    Engine::onSafeAreaChanged.add("test_safe", [&events]() { events.push_back("safe"); });

    const uint64_t revision = Screen::getSafeAreaRevision();
    // a push between frames: the event at the start of the next frame's update
    Screen::setPlatformSafeArea(EdgeInsets(0, 100, 0, 50));
    EXPECT_TRUE(events.empty());
    EXPECT_GT(Screen::getSafeAreaRevision(), revision);
    Screen::dispatchSafeAreaEvent();
    EXPECT_EQ(events, (std::vector<std::string>{"safe"}));

    // the same values again: nothing
    events.clear();
    const uint64_t same = Screen::getSafeAreaRevision();
    Screen::setPlatformSafeArea(EdgeInsets(0, 100, 0, 50));
    Screen::dispatchSafeAreaEvent();
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(Screen::getSafeAreaRevision(), same);

    // a rotation: the new insets and size, one event after the canvas and orientation ones
    events.clear();
    Screen::setPlatformSafeArea(EdgeInsets(100, 0, 100, 30));
    system.width = 2000;
    system.height = 1000;
    Engine::systemViewChanged();
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.front(), "canvas");
    EXPECT_EQ(events.back(), "safe");
    EXPECT_EQ(std::count(events.begin(), events.end(), std::string("safe")), 1);
    Screen::dispatchSafeAreaEvent();
    EXPECT_EQ(std::count(events.begin(), events.end(), std::string("safe")), 1);
}

TEST_F(SafeAreaTest, ResetMakesTheNextValueABaseline) {
    view(1000, 2000, 500, 1000, Scaling::FITWIDTH);
    int events = 0;
    Engine::onSafeAreaChanged.add("test_safe", [&events]() { events++; });
    Screen::setExtraInsets(EdgeInsets(0, 10, 0, 0));
    Screen::resetSafeAreaState();
    EXPECT_TRUE(Screen::getExtraInsets().isZero());
    Screen::dispatchSafeAreaEvent();
    EXPECT_EQ(events, 0);
}

// ---- 4. cutouts -------------------------------------------------------------------------------

TEST(SafeAreaMapping, CutoutsMappedAndClipped) {
    SafeAreaInput input = inputFor(1000, 2000, 500, 1000, Scaling::FITWIDTH, EdgeInsets(0, 100, 0, 0));
    input.cutoutsPixels = {Rect(400, 0, 200, 100), Rect(900, -50, 200, 100), Rect(0, 2100, 10, 10)};
    SafeAreaResult r = Screen::computeSafeArea(input);
    ASSERT_EQ(r.cutouts.size(), 2u);
    expectRect(r.cutouts[0], 200, 0, 100, 50);
    expectRect(r.cutouts[1], 450, 0, 50, 25);
}

// ---- 8. the desktop simulation variable -------------------------------------------------------

TEST(SafeAreaSimulation, ParsesTopRightBottomLeft) {
    EdgeInsets insets;
    ASSERT_TRUE(Screen::parseSimulatedSafeArea("47,0,34,0", insets));
    expectInsets(insets, 0, 47, 0, 34);
    ASSERT_TRUE(Screen::parseSimulatedSafeArea(" 10 , 20,30, 40 ", insets));
    expectInsets(insets, 40, 10, 20, 30);
    EXPECT_FALSE(Screen::parseSimulatedSafeArea("10,20,30", insets));
    EXPECT_FALSE(Screen::parseSimulatedSafeArea("10,20,30,40,50", insets));
    EXPECT_FALSE(Screen::parseSimulatedSafeArea("10,-2,30,40", insets));
    EXPECT_FALSE(Screen::parseSimulatedSafeArea("1.5,2,3,4", insets));
    EXPECT_FALSE(Screen::parseSimulatedSafeArea("", insets));
    EXPECT_FALSE(Screen::parseSimulatedSafeArea(nullptr, insets));
}

// ---- 5. the Safe Area component ---------------------------------------------------------------

namespace {

    Entity uiElement(Scene& scene, Entity parent, unsigned int width, unsigned int height) {
        Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, Transform());
        RectTransformComponent layout;
        layout.width = width;
        layout.height = height;
        scene.addComponent<RectTransformComponent>(entity, layout);
        if (parent != NULL_ENTITY) scene.addEntityChild(parent, entity, false);
        return entity;
    }

}

TEST_F(SafeAreaTest, ComponentAnchorsTheRootToTheSafeArea) {
    view(1000, 2000, 500, 1000, Scaling::FITWIDTH);
    Screen::setPlatformSafeArea(EdgeInsets(20, 100, 40, 50));
    Scene scene;
    Entity panel = uiElement(scene, NULL_ENTITY, 10, 10);
    scene.addComponent<SafeAreaComponent>(panel, SafeAreaComponent());
    scene.getSystem<UISystem>()->update(0);

    const RectTransformComponent& layout = scene.getComponent<RectTransformComponent>(panel);
    EXPECT_TRUE(layout.usingAnchors);
    EXPECT_EQ(layout.anchorPreset, AnchorPreset::NONE);
    EXPECT_NEAR(layout.anchorPointLeft, 10.0f / 500.0f, 1e-5f);
    EXPECT_NEAR(layout.anchorPointTop, 50.0f / 1000.0f, 1e-5f);
    EXPECT_NEAR(layout.anchorPointRight, 480.0f / 500.0f, 1e-5f);
    EXPECT_NEAR(layout.anchorPointBottom, 975.0f / 1000.0f, 1e-5f);
    EXPECT_EQ(layout.width, 470u);
    EXPECT_EQ(layout.height, 925u);
    const Transform& transform = scene.getComponent<Transform>(panel);
    EXPECT_NEAR(transform.position.x, 10.0f, 1e-4f);
    EXPECT_NEAR(transform.position.y, 50.0f, 1e-4f);

    // edges turned off stay at the reference's edges; extras ignored on request
    SafeAreaComponent& safe = scene.getComponent<SafeAreaComponent>(panel);
    safe.left = false;
    safe.bottom = false;
    safe.includeExtraInsets = false;
    Screen::setExtraInsets(EdgeInsets(0, 30, 0, 0));
    scene.getSystem<UISystem>()->update(0);
    EXPECT_EQ(layout.anchorPointLeft, 0.0f);
    EXPECT_EQ(layout.anchorPointBottom, 1.0f);
    EXPECT_NEAR(layout.anchorPointTop, 50.0f / 1000.0f, 1e-5f);
    safe.includeExtraInsets = true;
    scene.getSystem<UISystem>()->update(0);
    EXPECT_NEAR(layout.anchorPointTop, 80.0f / 1000.0f, 1e-5f);

    // a Screen change shows in the next update
    Screen::clearExtraInsets();
    Screen::setPlatformSafeArea(EdgeInsets());
    scene.getSystem<UISystem>()->update(0);
    EXPECT_EQ(layout.anchorPointTop, 0.0f);
    EXPECT_EQ(layout.anchorPointRight, 1.0f);
    EXPECT_EQ(safe.appliedRevision, Screen::getSafeAreaRevision());
}

TEST_F(SafeAreaTest, ComponentUnderASmallerParentClamps) {
    view(1000, 2000, 500, 1000, Scaling::FITWIDTH);
    Screen::setPlatformSafeArea(EdgeInsets(20, 100, 40, 50));
    Scene scene;
    // the left half of the canvas
    Entity half = uiElement(scene, NULL_ENTITY, 250, 1000);
    Entity panel = uiElement(scene, half, 10, 10);
    scene.addComponent<SafeAreaComponent>(panel, SafeAreaComponent());
    scene.getSystem<UISystem>()->update(0);

    const RectTransformComponent& layout = scene.getComponent<RectTransformComponent>(panel);
    EXPECT_NEAR(layout.anchorPointLeft, 10.0f / 250.0f, 1e-5f);
    // the safe area's right edge is outside the parent: its edge
    EXPECT_EQ(layout.anchorPointRight, 1.0f);
    EXPECT_NEAR(layout.anchorPointTop, 0.05f, 1e-5f);

    // a nested Safe Area inside the safe panel is already safe: 0..1 (rule 7.7)
    Screen::setPlatformSafeArea(EdgeInsets(0, 100, 0, 0));
    Scene nested;
    Entity outer = uiElement(nested, NULL_ENTITY, 10, 10);
    nested.addComponent<SafeAreaComponent>(outer, SafeAreaComponent());
    Entity inner = uiElement(nested, outer, 10, 10);
    nested.addComponent<SafeAreaComponent>(inner, SafeAreaComponent());
    nested.getSystem<UISystem>()->update(0);
    // the inner one reads the outer's world rect: placed by this update, its matrix by the next
    Transform& outerTransform = nested.getComponent<Transform>(outer);
    outerTransform.modelMatrix = Matrix4::translateMatrix(outerTransform.position);
    nested.getSystem<UISystem>()->update(0);
    const RectTransformComponent& innerLayout = nested.getComponent<RectTransformComponent>(inner);
    EXPECT_NEAR(innerLayout.anchorPointTop, 0.0f, 1e-5f);
    EXPECT_NEAR(innerLayout.anchorPointBottom, 1.0f, 1e-5f);
}

TEST_F(SafeAreaTest, ComponentInAYUpSceneMirrors) {
    // a 2D camera: layout's "top" (smaller y) is the screen's bottom, so the notch at the
    // screen's top is at the layout's bottom
    view(1000, 2000, 500, 1000, Scaling::FITWIDTH);
    Screen::setPlatformSafeArea(EdgeInsets(0, 100, 0, 50));
    Scene scene;
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_ORTHO);
    scene.setCamera(&camera);
    Entity panel = uiElement(scene, NULL_ENTITY, 10, 10);
    scene.addComponent<SafeAreaComponent>(panel, SafeAreaComponent());
    scene.getSystem<UISystem>()->update(0);
    const RectTransformComponent& layout = scene.getComponent<RectTransformComponent>(panel);
    EXPECT_NEAR(layout.anchorPointTop, 25.0f / 1000.0f, 1e-5f) << "the home indicator (screen bottom)";
    EXPECT_NEAR(layout.anchorPointBottom, 950.0f / 1000.0f, 1e-5f) << "the notch (screen top)";
    // the Top edge off: the screen's top (layout bottom) stays at the edge
    scene.getComponent<SafeAreaComponent>(panel).top = false;
    scene.getSystem<UISystem>()->update(0);
    EXPECT_EQ(layout.anchorPointBottom, 1.0f);
    EXPECT_NEAR(layout.anchorPointTop, 0.025f, 1e-5f);
}

TEST_F(SafeAreaTest, InactiveComponentKeepsItsLastAnchors) {
    view(1000, 2000, 500, 1000, Scaling::FITWIDTH);
    Screen::setPlatformSafeArea(EdgeInsets(0, 100, 0, 0));
    Scene scene;
    Entity panel = uiElement(scene, NULL_ENTITY, 10, 10);
    scene.addComponent<SafeAreaComponent>(panel, SafeAreaComponent());
    scene.getSystem<UISystem>()->update(0);
    const RectTransformComponent& layout = scene.getComponent<RectTransformComponent>(panel);
    EXPECT_NEAR(layout.anchorPointTop, 0.05f, 1e-5f);
    // a disabled component
    scene.getComponent<SafeAreaComponent>(panel).enabled = false;
    Screen::setPlatformSafeArea(EdgeInsets(0, 200, 0, 0));
    scene.getSystem<UISystem>()->update(0);
    EXPECT_NEAR(layout.anchorPointTop, 0.05f, 1e-5f);
    // an inactive GameObject
    scene.getComponent<SafeAreaComponent>(panel).enabled = true;
    scene.setEntityActive(panel, false);
    scene.getSystem<UISystem>()->update(0);
    EXPECT_NEAR(layout.anchorPointTop, 0.05f, 1e-5f);
    scene.setEntityActive(panel, true);
    scene.getSystem<UISystem>()->update(0);
    EXPECT_NEAR(layout.anchorPointTop, 0.1f, 1e-5f);
}

TEST_F(SafeAreaTest, EditorReferenceScalesToTheProjectCanvas) {
    // edit mode: the Engine's view is the Scene view; UI is laid out for the 500x1000 canvas as a
    // Play in a 1000x2000 Game view would
    view(300, 300, 500, 1000, Scaling::FITWIDTH);
    Screen::setPlatformSafeArea(EdgeInsets(0, 100, 0, 50));
    Screen::setEditorReference(500, 1000, 1000, 2000, Scaling::FITWIDTH);
    EXPECT_TRUE(Screen::hasEditorReference());
    expectRect(Screen::getSafeArea(), 0, 50, 500, 925);
    EXPECT_EQ(Screen::getSafeAreaCanvasWidth(), 500.0f);
    // a Game view of another aspect: the same fractions of the canvas
    Screen::setEditorReference(500, 1000, 500, 1000, Scaling::STRETCH);
    expectRect(Screen::getSafeArea(), 0, 100, 500, 850);
    Screen::clearEditorReference();
    EXPECT_FALSE(Screen::hasEditorReference());
}

// ---- 6. serialization, prefabs and the exported code -------------------------------------------

using namespace jasperengine::editor;

TEST(SafeAreaComponent, CatalogNamesAndProperties) {
    const ComponentType type = ComponentType::SafeAreaComponent;
    EXPECT_EQ(LastComponentType, type) << "appended at the end: prefab override masks use the ordinal";
    EXPECT_EQ(Catalog::getComponentName(type, true), "safearea");
    EXPECT_EQ(Catalog::getComponentDisplayName(type), "Safe Area");
    EXPECT_EQ(Catalog::getComponentType("safearea"), type);
    EXPECT_EQ(Catalog::getComponentType("SafeAreaComponent"), type);
    EXPECT_TRUE(Catalog::hasEnabledProperty(type));

    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<SafeAreaComponent>(entity, {});
    SafeAreaComponent& safe = scene.getComponent<SafeAreaComponent>(entity);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "left").ref, &safe.left);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "includeExtraInsets").ref, &safe.includeExtraInsets);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "enabled").ref, &safe.enabled);
    EXPECT_FALSE(Catalog::findProperty(&scene, entity, type, "appliedRevision").ref) << "runtime, not a property";
    const std::vector<ComponentType> found = Catalog::findComponents(&scene, entity);
    EXPECT_NE(std::find(found.begin(), found.end(), type), found.end());
}

TEST(SafeAreaComponent, StreamWritesOnlyNonDefaults) {
    EXPECT_EQ(Stream::encodeSafeAreaComponent(SafeAreaComponent()).size(), 0u);
    SafeAreaComponent authored;
    authored.left = false;
    authored.includeExtraInsets = false;
    YAML::Node node = Stream::encodeSafeAreaComponent(authored);
    EXPECT_EQ(node.size(), 2u);
    EXPECT_FALSE(node["top"].IsDefined());
    const SafeAreaComponent decoded = Stream::decodeSafeAreaComponent(node);
    EXPECT_FALSE(decoded.left);
    EXPECT_TRUE(decoded.top && decoded.right && decoded.bottom && decoded.enabled);
    EXPECT_FALSE(decoded.includeExtraInsets);

    // decoded over an existing one: what the YAML leaves out is the default again
    SafeAreaComponent existing;
    existing.top = false;
    existing.appliedRevision = 42;
    const SafeAreaComponent over = Stream::decodeSafeAreaComponent(node, &existing);
    EXPECT_TRUE(over.top);
    EXPECT_EQ(over.appliedRevision, 42u);

    // a whole entity: the component and its key round trip
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<RectTransformComponent>(entity, {});
    scene.addComponent<SafeAreaComponent>(entity, authored);
    YAML::Node components = Stream::encodeComponents(entity, &scene, scene.getSignature(entity));
    ASSERT_TRUE(components["safearea"].IsDefined());
    Scene other;
    const Entity copy = other.createUserEntity();
    Stream::decodeComponents(copy, NULL_ENTITY, &other, components);
    ASSERT_TRUE(other.findComponent<SafeAreaComponent>(copy));
    EXPECT_FALSE(other.getComponent<SafeAreaComponent>(copy).left);
    EXPECT_FALSE(other.getComponent<SafeAreaComponent>(copy).includeExtraInsets);

    // a default one is `safearea: {}` and comes back
    Scene plain;
    const Entity plainEntity = plain.createUserEntity();
    plain.addComponent<Transform>(plainEntity, {});
    plain.addComponent<SafeAreaComponent>(plainEntity, {});
    YAML::Node plainComponents = Stream::encodeComponents(plainEntity, &plain, plain.getSignature(plainEntity));
    Scene plainCopy;
    const Entity plainCopyEntity = plainCopy.createUserEntity();
    Stream::decodeComponents(plainCopyEntity, NULL_ENTITY, &plainCopy, YAML::Load(YAML::Dump(plainComponents)));
    EXPECT_TRUE(plainCopy.findComponent<SafeAreaComponent>(plainCopyEntity));

    // removed and added back (undo of Remove Component)
    std::vector<Entity> entities{entity};
    const YAML::Node removed = ProjectUtils::removeEntityComponent(&scene, entity, ComponentType::SafeAreaComponent, entities, true);
    EXPECT_FALSE(scene.findComponent<SafeAreaComponent>(entity));
    ProjectUtils::addEntityComponent(&scene, entity, ComponentType::SafeAreaComponent, entities, removed);
    ASSERT_TRUE(scene.findComponent<SafeAreaComponent>(entity));
    EXPECT_FALSE(scene.getComponent<SafeAreaComponent>(entity).left);
}

TEST(SafeAreaComponent, PrefabInstanceOverride) {
    const ComponentType type = ComponentType::SafeAreaComponent;
    auto make = [](Scene& scene, const SafeAreaComponent& safe) {
        const Entity entity = scene.createUserEntity();
        scene.addComponent<Transform>(entity, {});
        scene.addComponent<SafeAreaComponent>(entity, safe);
        return entity;
    };
    Scene asset;
    const Entity assetRoot = make(asset, SafeAreaComponent());
    Scene instance;
    SafeAreaComponent noBottom;
    noBottom.bottom = false;
    const Entity instanceRoot = make(instance, noBottom);
    EXPECT_EQ(prefabinternal::diffComponentProperties(&asset, assetRoot, &instance, instanceRoot, type),
              std::vector<std::string>{"bottom"});

    prefabinternal::ScratchEntity values;
    values.takeComponents(&instance, instanceRoot, Catalog::componentTypeToSignature(&instance, type));
    Scene loaded;
    const Entity loadedRoot = make(loaded, SafeAreaComponent());
    std::set<std::string> names{"bottom"};
    prefabinternal::copyProperties(values, &loaded, loadedRoot, type, names, "test");
    EXPECT_FALSE(loaded.getComponent<SafeAreaComponent>(loadedRoot).bottom);
    EXPECT_TRUE(loaded.getComponent<SafeAreaComponent>(loadedRoot).top);
}

namespace {

    // what Factory writes for the authored component below (it compiles in an export: the same
    // statements, written here)
    void emittedSafeArea(Scene* scene, Entity entity) {
        SafeAreaComponent safearea;
        safearea.top = false;
        safearea.includeExtraInsets = false;
        scene->addComponent<SafeAreaComponent>(entity, safearea);
    }

    const char* const EMITTED_SAFE_AREA =
        "SafeAreaComponent safearea;\n"
        "safearea.top = false;\n"
        "safearea.includeExtraInsets = false;\n"
        "scene->addComponent<SafeAreaComponent>(entity, safearea);\n";

}

TEST(SafeAreaComponent, FactoryEmitsTheNonDefaults) {
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    SafeAreaComponent authored;
    authored.top = false;
    authored.includeExtraInsets = false;
    scene.addComponent<SafeAreaComponent>(entity, authored);
    EXPECT_EQ(Factory::createComponent(0, &scene, entity, ComponentType::SafeAreaComponent, "", "scene", "entity"), EMITTED_SAFE_AREA);

    Scene built;
    const Entity builtEntity = built.createUserEntity();
    emittedSafeArea(&built, builtEntity);
    EXPECT_FALSE(built.getComponent<SafeAreaComponent>(builtEntity).top);
    EXPECT_FALSE(built.getComponent<SafeAreaComponent>(builtEntity).includeExtraInsets);
    EXPECT_TRUE(built.getComponent<SafeAreaComponent>(builtEntity).left);
}

// ---- 7. device profiles and the overlay --------------------------------------------------------

TEST(SafeAreaDevices, ProfileInsetsForEveryOrientation) {
    constexpr ScreenOrientation P = ScreenOrientation::Portrait;
    constexpr ScreenOrientation LL = ScreenOrientation::LandscapeLeft;
    constexpr ScreenOrientation LR = ScreenOrientation::LandscapeRight;
    constexpr ScreenOrientation PU = ScreenOrientation::PortraitUpsideDown;
    struct Expected { const char* id; EdgeInsets portrait, landscapeLeft, landscapeRight; };
    const Expected table[] = {
        {"iphone_15_pro", EdgeInsets(0, 177, 0, 102), EdgeInsets(177, 0, 177, 63), EdgeInsets(177, 0, 177, 63)},
        {"iphone_15_pro_max", EdgeInsets(0, 177, 0, 102), EdgeInsets(177, 0, 177, 63), EdgeInsets(177, 0, 177, 63)},
        {"iphone_13", EdgeInsets(0, 141, 0, 102), EdgeInsets(141, 0, 141, 63), EdgeInsets(141, 0, 141, 63)},
        {"iphone_x", EdgeInsets(0, 132, 0, 102), EdgeInsets(132, 0, 132, 63), EdgeInsets(132, 0, 132, 63)},
        {"iphone_se", EdgeInsets(), EdgeInsets(), EdgeInsets()},
        {"ipad_pro_11", EdgeInsets(0, 0, 0, 40), EdgeInsets(0, 0, 0, 40), EdgeInsets(0, 0, 0, 40)},
        {"pixel_8", EdgeInsets(0, 128, 0, 0), EdgeInsets(128, 0, 0, 0), EdgeInsets(0, 0, 128, 0)},
        {"pixel_3_xl", EdgeInsets(0, 171, 0, 0), EdgeInsets(171, 0, 0, 0), EdgeInsets(0, 0, 171, 0)},
        {"galaxy_s23", EdgeInsets(0, 110, 0, 0), EdgeInsets(110, 0, 0, 0), EdgeInsets(0, 0, 110, 0)},
    };
    for (const Expected& expected : table) {
        const DeviceProfile* profile = findDeviceProfile(expected.id);
        ASSERT_TRUE(profile) << expected.id;
        EXPECT_EQ(profileInsets(*profile, P), expected.portrait) << expected.id;
        EXPECT_EQ(profileInsets(*profile, LL), expected.landscapeLeft) << expected.id;
        EXPECT_EQ(profileInsets(*profile, LR), expected.landscapeRight) << expected.id;
        int w = 0, h = 0;
        profileSize(*profile, LL, w, h);
        EXPECT_EQ(w, profile->height);
        EXPECT_EQ(h, profile->width);
    }
    // Android upside down: the cutout at the bottom
    EXPECT_EQ(profileInsets(*findDeviceProfile("pixel_8"), PU), EdgeInsets(0, 0, 0, 128));
    // the cutout turns with the screen: the Pixel 8's punch hole (portrait top) on the left in LandscapeLeft
    const Rect hole = profileCutout(*findDeviceProfile("pixel_8"), LL);
    EXPECT_EQ(hole.getX(), 44.0f);
    EXPECT_EQ(hole.getY(), 1080.0f - 498.0f - 84.0f);
    EXPECT_EQ(profileCutout(*findDeviceProfile("pixel_8"), LR).getX(), 2400.0f - 44.0f - 84.0f);

    // the Custom device turns its portrait insets
    CustomDeviceSettings custom;
    custom.width = 1000;
    custom.height = 2000;
    custom.top = 80;
    custom.bottom = 30;
    setCustomDevice(custom);
    const DeviceProfile* profile = findDeviceProfile("custom");
    ASSERT_TRUE(profile);
    EXPECT_EQ(profileInsets(*profile, P), EdgeInsets(0, 80, 0, 30));
    EXPECT_EQ(profileInsets(*profile, LL), EdgeInsets(80, 0, 30, 0));
    EXPECT_EQ(profileInsets(*profile, LR), EdgeInsets(30, 0, 80, 0));
    EXPECT_EQ(profileInsets(*profile, PU), EdgeInsets(0, 30, 0, 80));
    setCustomDevice(CustomDeviceSettings());
}

TEST(SafeAreaDevices, RasterizedOverlay) {
    // an iPhone 13 shrunk to 200 pixels across: corners, notch and home indicator over white
    const DeviceProfile* profile = findDeviceProfile("iphone_13");
    ASSERT_TRUE(profile);
    const int w = 200, h = 433;
    std::vector<uint8_t> image((size_t)w * h * 4, 255);
    rasterizeDeviceOverlay(image.data(), w, h, *profile, ScreenOrientation::Portrait, false);
    auto pixel = [&](int x, int y) { return &image[((size_t)y * w + x) * 4]; };
    EXPECT_EQ(pixel(0, 0)[0], 0) << "outside the rounded corner";
    EXPECT_EQ(pixel(w - 1, h - 1)[0], 0);
    EXPECT_EQ(pixel(w / 2, h / 2)[0], 255) << "the middle of the screen";
    EXPECT_EQ(pixel(w / 2, 4)[0], 0) << "the notch";
    EXPECT_EQ(pixel(4, h / 2)[0], 255) << "the left edge between the corners";
    // the home indicator: light gray at 60% over white, 8 points above the bottom
    const float s = (float)w / (float)profile->width;
    const int barY = (int)std::round(h - (8.0f + 2.5f) * 3.0f * s);
    const uint8_t* bar = pixel(w / 2, barY);
    EXPECT_LT(bar[0], 255);
    EXPECT_GT(bar[0], 200);
    EXPECT_EQ(bar[0], bar[1]);
    EXPECT_EQ(bar[1], bar[2]);

    // the outline: green on the safe rectangle's edge
    std::vector<uint8_t> outlined((size_t)w * h * 4, 255);
    const Rect safe(10, 20, 100, 200);
    rasterizeDeviceOverlay(outlined.data(), w, h, *profile, ScreenOrientation::Portrait, true, &safe, false);
    const uint8_t* edge = &outlined[((size_t)20 * w + 10) * 4];
    EXPECT_EQ(edge[0], 0);
    EXPECT_EQ(edge[1], 255);
    EXPECT_EQ(outlined[((size_t)0 * w + 0) * 4], 255) << "no screen drawn with screen = false";

    // a punch hole turned with the device: on the left in LandscapeLeft
    const DeviceProfile* pixel8 = findDeviceProfile("pixel_8");
    const int lw = 400, lh = 180;
    std::vector<uint8_t> landscape((size_t)lw * lh * 4, 255);
    rasterizeDeviceOverlay(landscape.data(), lw, lh, *pixel8, ScreenOrientation::LandscapeLeft, false);
    const float ls = (float)lw / 2400.0f;
    const int holeX = (int)((44.0f + 42.0f) * ls);
    const int holeY = (int)((1080.0f - 498.0f - 42.0f) * ls);
    EXPECT_EQ(landscape[((size_t)holeY * lw + holeX) * 4], 0);
    EXPECT_EQ(landscape[((size_t)holeY * lw + (lw - holeX)) * 4], 255);
}
