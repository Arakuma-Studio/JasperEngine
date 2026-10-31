// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT
//
// The component UI's multi-pointer EventSystem (ui-multitouch spec 14): per-pointer presses of
// Buttons, the legacy callbacks only on the pressed element, ExecuteHierarchy of the script
// callbacks and the RegisterEvent collision rule, the drag sequence, the cancel paths, the
// On-Screen Stick maths (Joystick Pack), the Touch Area next to a stick on another finger, and
// Input.isPointerOverUI per pointer.

#include "ScriptTestHarness.h"

#include "subsystem/UISystem.h"
#include "input/InputActions.h"
#include "component/RectTransformComponent.h"
#include "component/UIComponent.h"
#include "component/ImageComponent.h"
#include "component/ButtonComponent.h"
#include "component/OnScreenControlComponents.h"
#include "object/Camera.h"

#include <cmath>
#include <climits>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::scripttest;

namespace {

    class UIMultiPointerTest : public ScriptTest {
    protected:
        Scene* scene = nullptr;
        std::unique_ptr<Camera> camera;

        void SetUp() override {
            ScriptTest::SetUp();
            InputActions::loadDefaults();
            InputActions::resetRuntimeState();
            scene = newScene();
            // texts need the scene's camera (Scene::load makes it only with a view)
            camera = std::make_unique<Camera>(scene);
            scene->setCamera(camera->getEntity());
            Engine::setScene(scene);
        }

        void TearDown() override {
            camera.reset();
            if (HasFailure()) {
                for (const std::string& line : system.logLines) std::printf("log: %s\n", line.c_str());
            }
            InputActions::resetRuntimeState();
            ScriptTest::TearDown();
        }

        UISystem& ui() { return *scene->getSystem<UISystem>(); }

        // a UI element at (x, y), size w x h (top-left, y down), hit by pointers (an Image)
        Entity element(const std::string& name, float x, float y, unsigned int w, unsigned int h, Entity parent = NULL_ENTITY) {
            const Entity e = entity(scene, name, parent);
            scene->addComponent<RectTransformComponent>(e, {});
            scene->addComponent<UIComponent>(e, {});
            scene->addComponent<ImageComponent>(e, {});
            RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(e);
            layout.width = w;
            layout.height = h;
            Transform& transform = scene->getComponent<Transform>(e);
            transform.position = Vector3(x, y, 0);
            transform.needUpdate = true;
            return e;
        }

        Entity button(const std::string& name, float x, float y) {
            const Entity e = element(name, x, y, 100, 100);
            scene->addComponent<ButtonComponent>(e, {});
            // its label, as GameObject > UI > Button makes it
            ui().createButtonObjects(e, scene->getComponent<ButtonComponent>(e));
            return e;
        }

        // the scripts (init, onEnable) and the world positions of the elements
        void start() {
            LuaBinding::initializeLuaScripts(scene);
            frame();
        }

        bool down(int pointer, float x, float y) { return ui().eventOnPointerDown(pointer, D_MOUSE_BUTTON_LEFT, x, y, pointer >= 0); }
        bool move(int pointer, float x, float y) { return ui().eventOnPointerMove(pointer, x, y, pointer >= 0); }
        bool up(int pointer, float x, float y) { return ui().eventOnPointerUp(pointer, D_MOUSE_BUTTON_LEFT, x, y, pointer >= 0); }

        // legacy (x, y) subscriptions counting into a map
        std::map<std::string, int> counts;
        void count(FunctionSubscribe<void(float, float)>& event, const std::string& key) {
            event.add(key, std::function<void(float, float)>([this, key](float, float) { counts[key]++; }));
        }
        void count(FunctionSubscribe<void()>& event, const std::string& key) {
            event.add(key, std::function<void()>([this, key]() { counts[key]++; }));
        }

        Entity stick(OnScreenStickMode mode, Entity& background, Entity& handle) {
            const Entity root = element("Stick", 0, 300, 300, 300);
            background = element("Background", 100, 100, 100, 100, root);   // centre (150, 450), radius 50
            handle = element("Handle", 25, 25, 50, 50, background);
            OnScreenStickComponent component;
            component.mode = mode;
            component.background = background;
            component.handle = handle;
            scene->addComponent<OnScreenStickComponent>(root, component);
            return root;
        }

        OnScreenStickComponent& stickOf(Entity e) { return scene->getComponent<OnScreenStickComponent>(e); }
    };

}

// two fingers press two Buttons; lifting one releases only its Button, and the legacy
// onPointerUp fires only on the element that finger pressed (before: on every element)
TEST_F(UIMultiPointerTest, TwoPointersPressTwoButtonsIndependently) {
    const Entity a = button("A", 0, 0);
    const Entity b = button("B", 200, 0);
    count(scene->getComponent<ButtonComponent>(a).onRelease, "releaseA");
    count(scene->getComponent<ButtonComponent>(b).onRelease, "releaseB");
    count(scene->getComponent<UIComponent>(a).onPointerUp, "upA");
    count(scene->getComponent<UIComponent>(b).onPointerUp, "upB");
    start();

    EXPECT_TRUE(down(0, 50, 50));
    EXPECT_TRUE(down(1, 250, 50));
    EXPECT_TRUE(scene->getComponent<ButtonComponent>(a).pressed);
    EXPECT_TRUE(scene->getComponent<ButtonComponent>(b).pressed);

    EXPECT_TRUE(up(0, 50, 50));
    EXPECT_FALSE(scene->getComponent<ButtonComponent>(a).pressed);
    EXPECT_TRUE(scene->getComponent<ButtonComponent>(b).pressed) << "lifting finger 0 keeps finger 1's button";
    EXPECT_EQ(counts["releaseA"], 1);
    EXPECT_EQ(counts["releaseB"], 0);
    EXPECT_EQ(counts["upA"], 1);
    EXPECT_EQ(counts["upB"], 0) << "the legacy up goes to the pressed element only";

    EXPECT_TRUE(up(1, 250, 50));
    EXPECT_EQ(counts["releaseB"], 1);
    EXPECT_EQ(counts["upB"], 1);
}

// two fingers on one Button: one onPress, one onRelease, a click each
TEST_F(UIMultiPointerTest, SameButtonTwoFingersPressOnceClickTwice) {
    const Entity a = button("A", 0, 0);
    ButtonComponent& component = scene->getComponent<ButtonComponent>(a);
    count(component.onPress, "press");
    count(component.onRelease, "release");
    count(scene->getComponent<UIComponent>(a).onClick, "click");
    start();

    down(0, 20, 20);
    down(1, 80, 80);
    EXPECT_EQ(counts["press"], 1);
    up(0, 20, 20);
    EXPECT_TRUE(scene->getComponent<ButtonComponent>(a).pressed) << "pressed while a finger holds it";
    EXPECT_EQ(counts["release"], 0);
    up(1, 80, 80);
    EXPECT_FALSE(scene->getComponent<ButtonComponent>(a).pressed);
    EXPECT_EQ(counts["release"], 1);
    EXPECT_EQ(counts["click"], 2);
}

// ExecuteHierarchy: the parent's script gets the child's down; its click needs the release over it
TEST_F(UIMultiPointerTest, ExecuteHierarchyAndPointerEventData) {
    writeScript("Handler",
        "local M = {}\n"
        "function M:onPointerDown(e) log[#log + 1] = 'down' .. e.pointerId .. ':' .. e.target.name .. ':' .. e.pointerEnter.name end\n"
        "function M:onPointerUp(e) log[#log + 1] = 'up' .. tostring(e.canceled) end\n"
        "function M:onPointerClick(e) log[#log + 1] = 'click' .. e.clickCount end\n"
        "function M:onPointerEnter(e) log[#log + 1] = 'enter' end\n"
        "function M:onPointerExit(e) log[#log + 1] = 'exit' end\n"
        "return M\n");
    const Entity parent = element("Parent", 0, 0, 200, 200);
    element("Child", 10, 10, 50, 50, parent);
    addScript(scene, parent, "Handler");
    start();

    down(3, 20, 20);
    EXPECT_EQ(takeLog(), "enter down3:Parent:Child") << "a finger enters, then presses (D5)";
    up(3, 20, 20);
    EXPECT_EQ(takeLog(), "upfalse click1 exit") << "a finger leaves with its release";
    down(3, 20, 20);
    up(3, 20, 20);
    EXPECT_EQ(takeLog(), "enter down3:Parent:Child upfalse click2 exit") << "a second click within 0.3 s counts";
}

// D4's collision rule: a method subscribed to the UIComponent event with RegisterEvent is called
// with (x, y) only, never also with a PointerEventData
TEST_F(UIMultiPointerTest, RegisterEventMethodIsNotCalledTwice) {
    writeScript("Legacy",
        "local M = {}\n"
        "function M:init() RegisterEvent(self, Image(self.scene, self.entity):getUIComponent().onPointerDown, 'onPointerDown') end\n"
        "function M:onPointerDown(a, b) log[#log + 1] = type(a) end\n"
        "return M\n");
    const Entity e = element("E", 0, 0, 100, 100);
    addScript(scene, e, "Legacy");
    start();

    down(0, 50, 50);
    EXPECT_EQ(takeLog(), "number") << "the old pattern gets (x, y) once";
    up(0, 50, 50);
}

// the drag sequence: potential drag, begin past the threshold, drag, drop on the slot under the
// finger, end; no click after a drag; useDragThreshold = false begins at once
TEST_F(UIMultiPointerTest, DragSequenceDropsOnTheSlotUnderThePointer) {
    writeScript("Item",
        "local M = {}\n"
        "function M:onInitializePotentialDrag(e) log[#log + 1] = 'potential'; if self.now then e.useDragThreshold = false end end\n"
        "function M:onBeginDrag(e) log[#log + 1] = 'begin' end\n"
        "function M:onDrag(e) log[#log + 1] = 'drag' end\n"
        "function M:onEndDrag(e) log[#log + 1] = 'end' end\n"
        "function M:onPointerClick(e) log[#log + 1] = 'click' end\n"
        "return M\n");
    writeScript("Slot",
        "local M = {}\n"
        "function M:onDrop(e) log[#log + 1] = 'drop:' .. self.scene:getEntityName(self.entity) .. ':' .. e.pointerDrag.name end\n"
        "return M\n");
    const Entity item = element("Item", 0, 0, 100, 100);
    const Entity slot = element("Slot", 200, 0, 100, 100);
    addScript(scene, item, "Item");
    addScript(scene, slot, "Slot");
    start();

    down(0, 50, 50);
    EXPECT_EQ(takeLog(), "potential");
    move(0, 51, 50);
    EXPECT_EQ(takeLog(), "") << "below the drag threshold";
    move(0, 250, 50);
    EXPECT_EQ(takeLog(), "begin drag");
    up(0, 250, 50);
    EXPECT_EQ(takeLog(), "drop:Slot:Item end") << "no click after a drag";

    lua("for _, s in ipairs(GameObject.findObjectsOfType('Item')) do s.now = true end");
    down(0, 50, 50);
    move(0, 51, 50);
    EXPECT_EQ(takeLog(), "potential begin drag") << "useDragThreshold = false begins at the first move";
    up(0, 51, 50);
    EXPECT_EQ(takeLog(), "end");
}

// cancels: a touch cancel ends the press without a click; a pressed element deactivated is
// cancelled (canceled = true); a scene leaving the layers drops the states without callbacks
TEST_F(UIMultiPointerTest, CancelPaths) {
    writeScript("Handler",
        "local M = {}\n"
        "function M:onPointerDown(e) log[#log + 1] = 'down' end\n"
        "function M:onPointerUp(e) log[#log + 1] = 'up' .. tostring(e.canceled) end\n"
        "function M:onPointerClick(e) log[#log + 1] = 'click' end\n"
        "return M\n");
    const Entity e = button("E", 0, 0);
    addScript(scene, e, "Handler");
    start();

    down(0, 50, 50);
    EXPECT_TRUE(ui().eventOnPointerCancel(0));
    EXPECT_EQ(takeLog(), "down uptrue") << "a cancel is an up with canceled, no click";
    EXPECT_FALSE(scene->getComponent<ButtonComponent>(e).pressed);

    // the element's UIComponent disabled while pressed: cancelled, the script told
    down(0, 50, 50);
    scene->getComponent<UIComponent>(e).enabled = false;
    frame();
    EXPECT_EQ(takeLog(), "down uptrue") << "disabled while pressed: cancelled";
    EXPECT_FALSE(scene->getComponent<ButtonComponent>(e).pressed);
    EXPECT_FALSE(ui().isPointerCaptured(0));
    scene->getComponent<UIComponent>(e).enabled = true;
    frame();
    clearLog();

    // deactivated while pressed: cancelled; an inactive GameObject's scripts get no messages (Unity)
    down(0, 50, 50);
    scene->setEntityActive(e, false);
    frame();
    EXPECT_EQ(takeLog(), "down");
    EXPECT_FALSE(scene->getComponent<ButtonComponent>(e).pressed);
    EXPECT_FALSE(ui().isPointerCaptured(0));
    scene->setEntityActive(e, true);
    frame();
    clearLog();

    down(0, 50, 50);
    ui().resetButtonStates();
    EXPECT_EQ(takeLog(), "down") << "a scene leaving: no callbacks into it";
    EXPECT_TRUE(ui().getPointers().empty());
    EXPECT_FALSE(scene->getComponent<ButtonComponent>(e).pressed);
}

// the right mouse button reaches the script callbacks (button right), not the built-in widgets
TEST_F(UIMultiPointerTest, RightButtonReachesScriptsOnly) {
    writeScript("Handler",
        "local M = {}\n"
        "function M:onPointerDown(e) log[#log + 1] = 'down' .. e.pointerId .. ':' .. e.button end\n"
        "return M\n");
    const Entity e = button("E", 0, 0);
    addScript(scene, e, "Handler");
    start();

    EXPECT_TRUE(ui().eventOnPointerDown(-2, D_MOUSE_BUTTON_RIGHT, 50, 50, false));
    EXPECT_EQ(takeLog(), "down-2:" + std::to_string(D_MOUSE_BUTTON_RIGHT));
    EXPECT_FALSE(scene->getComponent<ButtonComponent>(e).pressed) << "built-in widgets: the left button only";
    ui().eventOnPointerUp(-2, D_MOUSE_BUTTON_RIGHT, 50, 50, false);
}

// Joystick Pack's maths: dead zone, normalisation, axis options
TEST_F(UIMultiPointerTest, StickInputMaths) {
    OnScreenStickComponent stick;
    struct Case { float deadZone; OnScreenStickAxes axes; Vector2 in; Vector2 out; };
    const Case cases[] = {
        {0.0f, OnScreenStickAxes::BOTH, Vector2(0.5f, 0.0f), Vector2(0.5f, 0.0f)},
        {0.2f, OnScreenStickAxes::BOTH, Vector2(0.1f, 0.1f), Vector2(0.0f, 0.0f)},
        {0.2f, OnScreenStickAxes::BOTH, Vector2(0.2f, 0.0f), Vector2(0.0f, 0.0f)},
        {0.0f, OnScreenStickAxes::BOTH, Vector2(3.0f, 4.0f), Vector2(0.6f, 0.8f)},
        {0.0f, OnScreenStickAxes::HORIZONTAL, Vector2(0.3f, 0.9f), Vector2(0.3f, 0.0f)},
        {0.0f, OnScreenStickAxes::VERTICAL, Vector2(0.3f, -2.0f), Vector2(0.0f, -1.0f)},
    };
    for (const Case& c : cases) {
        stick.deadZone = c.deadZone;
        stick.axes = c.axes;
        const Vector2 result = UISystem::stickInput(stick, c.in);
        EXPECT_NEAR(result.x, c.out.x, 1e-5f) << c.in.x << "," << c.in.y;
        EXPECT_NEAR(result.y, c.out.y, 1e-5f) << c.in.x << "," << c.in.y;
    }
}

// a Fixed stick writes Horizontal / Vertical (y up), snaps at 22.5 / 67.5 degrees, the knob
// follows input * radius * handleRange, and release clears the actions
TEST_F(UIMultiPointerTest, FixedStickFeedsActionsAndSnaps) {
    Entity background, handle;
    const Entity root = stick(OnScreenStickMode::FIXED, background, handle);
    stickOf(root).handleRange = 0.5f;
    start();

    EXPECT_TRUE(down(0, 175, 450));   // half a radius right of the centre
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.5f, 1e-4f);
    EXPECT_NEAR(InputActions::getAxis("Vertical"), 0.0f, 1e-4f);
    move(0, 150, 350);               // 2 radii up: normalised
    EXPECT_NEAR(InputActions::getAxis("Vertical"), 1.0f, 1e-4f) << "y is up-positive";
    EXPECT_NEAR(stickOf(root).value.y, 1.0f, 1e-4f);
    const Transform& knob = scene->getComponent<Transform>(handle);
    EXPECT_NEAR(knob.position.y, 25.0f - 25.0f, 1e-3f) << "knob: input * radius * handleRange (1 * 50 * 0.5 up)";

    stickOf(root).snapX = true;
    stickOf(root).snapY = true;
    move(0, 150 + 40, 450 - 10);     // about 76 degrees from up: x snaps to 1, y to 0
    EXPECT_NEAR(stickOf(root).value.x, 1.0f, 1e-4f);
    EXPECT_NEAR(stickOf(root).value.y, 0.0f, 1e-4f);
    move(0, 150 + 5, 450 - 40);      // about 7 degrees: x snaps to 0, y to 1
    EXPECT_NEAR(stickOf(root).value.x, 0.0f, 1e-4f);
    EXPECT_NEAR(stickOf(root).value.y, 1.0f, 1e-4f);

    up(0, 155, 410);
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.0f, 1e-4f);
    EXPECT_NEAR(InputActions::getAxis("Vertical"), 0.0f, 1e-4f);
    EXPECT_FALSE(stickOf(root).pressed);
    EXPECT_NEAR(scene->getComponent<Transform>(handle).position.y, 25.0f, 1e-3f) << "the knob comes back";
}

// Floating: the base appears under the finger (shown while held) and goes back on release;
// Dynamic: it follows the finger past moveThreshold; a second finger on a held stick is ignored
TEST_F(UIMultiPointerTest, FloatingAndDynamicSticks) {
    Entity background, handle;
    const Entity root = stick(OnScreenStickMode::FLOATING, background, handle);
    start();
    EXPECT_FALSE(scene->getComponent<Transform>(background).visible) << "a released Floating base is hidden";

    down(0, 60, 380);   // area (0, 300) 300x300; the base (100 wide) clamps inside it
    EXPECT_NEAR(stickOf(root).center.x, 60.0f, 1e-3f);
    EXPECT_NEAR(stickOf(root).center.y, 380.0f, 1e-3f);
    EXPECT_NEAR(scene->getComponent<Transform>(background).position.x, 10.0f, 1e-3f) << "centre 60 = 0 + 10 + 50";
    frame();
    EXPECT_TRUE(scene->getComponent<Transform>(background).visible);

    EXPECT_TRUE(down(1, 70, 390)) << "the area is UI";
    EXPECT_EQ(stickOf(root).pointerId, 0) << "first pointer wins (D14)";
    up(1, 70, 390);

    up(0, 60, 380);
    EXPECT_NEAR(scene->getComponent<Transform>(background).position.x, 100.0f, 1e-3f) << "back to rest";
    frame();
    EXPECT_FALSE(scene->getComponent<Transform>(background).visible);

    stickOf(root).mode = OnScreenStickMode::DYNAMIC;
    stickOf(root).moveThreshold = 1.0f;
    down(0, 150, 450);
    move(0, 150 + 150, 450);   // 3 radii right: the base moves 2 radii (100 px)
    EXPECT_NEAR(stickOf(root).center.x, 250.0f, 1e-3f);
    EXPECT_NEAR(stickOf(root).value.x, 1.0f, 1e-4f);
    up(0, 300, 450);
}

// the Touch Area next to a stick on another finger: both actions move in the same frame
TEST_F(UIMultiPointerTest, TouchAreaAndStickOnTwoFingers) {
    Entity background, handle;
    stick(OnScreenStickMode::FIXED, background, handle);
    const Entity area = element("Area", 400, 0, 400, 600);
    scene->addComponent<TouchAreaComponent>(area, {});
    std::vector<Vector2> drags;
    scene->getComponent<TouchAreaComponent>(area).onDrag.add("t", std::function<void(float, float)>([&](float dx, float dy) {
        drags.push_back(Vector2(dx, dy));
    }));
    start();

    down(0, 175, 450);
    down(1, 500, 300);
    move(1, 510, 280);
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.5f, 1e-4f);
    EXPECT_NEAR(InputActions::getAxis("Mouse X"), 10.0f * 0.1f, 1e-4f);
    EXPECT_NEAR(InputActions::getAxis("Mouse Y"), 20.0f * 0.1f, 1e-4f) << "a drag up is Mouse Y up";
    frame();
    ASSERT_EQ(drags.size(), 1u);
    EXPECT_NEAR(drags[0].x, 10.0f, 1e-4f);
    EXPECT_NEAR(drags[0].y, -20.0f, 1e-4f);
    EXPECT_NEAR(InputActions::getAxis("Mouse X"), 0.0f, 1e-4f) << "a delta lasts its frame";
    up(1, 510, 280);
    up(0, 175, 450);
}

// an On-Screen Button presses its action, also with two fingers, and slides on with pressWhenPointerEnters
TEST_F(UIMultiPointerTest, OnScreenButtonPressesItsAction) {
    const Entity fire = element("Fire", 0, 0, 100, 100);
    scene->addComponent<OnScreenButtonComponent>(fire, {});
    start();

    // a source's press is seen by the next frame's update (InputActions)
    down(0, 50, 50);
    frame();
    EXPECT_TRUE(InputActions::getButton("Fire1"));
    down(1, 60, 60);
    up(0, 50, 50);
    frame();
    EXPECT_TRUE(InputActions::getButton("Fire1")) << "the other finger still holds it";
    up(1, 60, 60);
    frame();
    EXPECT_FALSE(InputActions::getButton("Fire1"));

    scene->getComponent<OnScreenButtonComponent>(fire).pressWhenPointerEnters = true;
    EXPECT_FALSE(down(2, 300, 300));
    move(2, 50, 50);
    EXPECT_TRUE(scene->getComponent<OnScreenButtonComponent>(fire).pressed) << "a finger sliding on presses it";
    up(2, 50, 50);
    EXPECT_FALSE(scene->getComponent<OnScreenButtonComponent>(fire).pressed);
}

// D15 per pointer: the mouse hovering, a finger pressing, a finger elsewhere
TEST_F(UIMultiPointerTest, IsPointerOverUIPerPointer) {
    element("E", 0, 0, 100, 100);
    start();

    move(-1, 50, 50);
    EXPECT_TRUE(ui().isPointerOverUI(-1));
    EXPECT_FALSE(ui().isPointerOverUI(0));
    down(0, 500, 500);
    EXPECT_FALSE(ui().isPointerOverUI(0)) << "a finger over nothing";
    down(1, 10, 10);
    EXPECT_TRUE(ui().isPointerOverUI(1));
    EXPECT_TRUE(ui().isPointerOverUI(INT_MIN));
    move(-1, 500, 500);
    up(1, 10, 10);
    up(0, 500, 500);
    EXPECT_FALSE(ui().isPointerOverUI(INT_MIN));
}
