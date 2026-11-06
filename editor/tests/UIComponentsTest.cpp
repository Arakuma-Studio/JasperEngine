// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The component UI's Buttons and layouts (ui-components spec 10): state textures, hidden layout
// children, text buffers, canvas refresh, Button edits from every path, and Unity's Selectable:
// selection, navigation, Submit and Cancel, the typing guard, the stick hand-back, transitions.

#include "ScriptTestHarness.h"

#include "subsystem/UISystem.h"
#include "input/InputActions.h"
#include "component/RectTransformComponent.h"
#include "component/UIComponent.h"
#include "component/ImageComponent.h"
#include "component/ButtonComponent.h"
#include "component/CameraComponent.h"
#include "component/InputFieldComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/TextComponent.h"
#include "object/Camera.h"
#include "htmlui/HtmlUIClock.h"
#include "texture/TextureData.h"
#include "Screen.h"
#include "Input.h"
#include "Catalog.h"

#include <cmath>
#include <cstdio>
#include <map>

namespace jasperengine {
    // TEST-ONLY seam (Engine.h befriends it): a view loaded without a GPU, the per-frame gamepad step
    struct EngineTestAccess {
        static bool setViewLoaded(bool loaded) {
            const bool previous = Engine::viewLoaded;
            Engine::viewLoaded = loaded;
            return previous;
        }
        static void updateGamepadNavigation() { Engine::updateGamepadNavigation(); }
    };
}

using namespace jasperengine;
using namespace jasperengine::scripttest;

namespace {

    class UIComponentsTest : public ScriptTest {
    protected:
        Scene* scene = nullptr;
        std::vector<std::unique_ptr<Camera>> cameras;
        bool previousViewLoaded = false;
        int previousCanvasWidth = 0;
        int previousCanvasHeight = 0;
        std::map<std::string, int> counts;

        void SetUp() override {
            ScriptTest::SetUp();
            InputActions::loadDefaults();
            InputActions::resetRuntimeState();
            // no view: the canvas is what the test sets (a loaded view would compute it)
            previousViewLoaded = EngineTestAccess::setViewLoaded(false);
            previousCanvasWidth = Engine::getPreferredCanvasWidth();
            previousCanvasHeight = Engine::getPreferredCanvasHeight();
            Engine::setCanvasSize(1000, 1000);
            HtmlUIClock::setFixedTime(0.0);
            scene = uiScene();
            Engine::setScene(scene);
            Engine::onKeyDown.add("uicomponents", std::function<void(int, bool, int)>([this](int key, bool, int) {
                counts["key" + std::to_string(key)]++;
            }));
            Engine::onCharInput.add("uicomponents", std::function<void(char32_t)>([this](char32_t) { counts["char"]++; }));
            Engine::onGamepadButtonDown.add("uicomponents", std::function<void(int, int)>([this](int, int button) {
                counts["pad" + std::to_string(button)]++;
            }));
            Engine::onGamepadAxisMove.add("uicomponents", std::function<void(int, int, float)>([this](int, int axis, float value) {
                if (axis == D_GAMEPAD_AXIS_LEFT_X) lastAxisX = value;
            }));
        }

        void TearDown() override {
            if (HasFailure()) {
                for (const std::string& line : system.logLines) std::printf("log: %s\n", line.c_str());
            }
            if (padConnected) Engine::systemGamepadDisconnect(0);
            Engine::onKeyDown.remove("uicomponents");
            Engine::onCharInput.remove("uicomponents");
            Engine::onGamepadButtonDown.remove("uicomponents");
            Engine::onGamepadAxisMove.remove("uicomponents");
            HtmlUIClock::reset();
            Engine::setCanvasSize(previousCanvasWidth, previousCanvasHeight);
            EngineTestAccess::setViewLoaded(previousViewLoaded);
            cameras.clear();
            InputActions::resetRuntimeState();
            ScriptTest::TearDown();
        }

        float lastAxisX = 0.0f;
        bool padConnected = false;

        // a scene with a UI camera, as a loaded UI scene has (keys and pads reach its UI)
        Scene* uiScene() {
            Scene* made = newScene();
            cameras.push_back(std::make_unique<Camera>(made));
            cameras.back()->setType(CameraType::CAMERA_UI);
            made->setCamera(cameras.back()->getEntity());
            return made;
        }

        UISystem& ui(Scene* in = nullptr) { return *(in ? in : scene)->getSystem<UISystem>(); }

        Entity element(const std::string& name, float x, float y, unsigned int w, unsigned int h, Entity parent = NULL_ENTITY, Scene* in = nullptr) {
            Scene* s = in ? in : scene;
            const Entity e = entity(s, name, parent);
            s->addComponent<RectTransformComponent>(e, {});
            s->addComponent<UIComponent>(e, {});
            s->addComponent<ImageComponent>(e, {});
            RectTransformComponent& layout = s->getComponent<RectTransformComponent>(e);
            layout.width = w;
            layout.height = h;
            Transform& transform = s->getComponent<Transform>(e);
            transform.position = Vector3(x, y, 0);
            transform.needUpdate = true;
            return e;
        }

        Entity button(const std::string& name, float x, float y, Scene* in = nullptr) {
            Scene* s = in ? in : scene;
            const Entity e = element(name, x, y, 100, 50, NULL_ENTITY, s);
            s->addComponent<ButtonComponent>(e, {});
            ui(s).createButtonObjects(e, s->getComponent<ButtonComponent>(e));
            return e;
        }

        ButtonComponent& btn(Entity e, Scene* in = nullptr) { return (in ? in : scene)->getComponent<ButtonComponent>(e); }
        UIComponent& uic(Entity e, Scene* in = nullptr) { return (in ? in : scene)->getComponent<UIComponent>(e); }

        void start() {
            LuaBinding::initializeLuaScripts(scene);
            frame();
        }

        void key(int k, int mods = 0) {
            Engine::systemKeyDown(k, false, mods);
            Engine::systemKeyUp(k, false, mods);
        }

        void connectPad() {
            Engine::systemGamepadConnect(0, "Test pad");
            padConnected = true;
        }

        void pad(int button) {
            Engine::systemGamepadButtonDown(0, button);
            Engine::systemGamepadButtonUp(0, button);
        }

        void count(FunctionSubscribe<void()>& event, const std::string& name) {
            event.add(name, std::function<void()>([this, name]() { counts[name]++; }));
        }
        void count(FunctionSubscribe<void(float, float)>& event, const std::string& name) {
            event.add(name, std::function<void(float, float)>([this, name](float, float) { counts[name]++; }));
        }

        // a 2 x 2 menu: A B over C D (100 x 50 each, 50 px apart)
        Entity a = NULL_ENTITY, b = NULL_ENTITY, c = NULL_ENTITY, d = NULL_ENTITY;
        void grid() {
            a = button("A", 100, 100);
            b = button("B", 250, 100);
            c = button("C", 100, 200);
            d = button("D", 250, 200);
        }
    };

    std::vector<unsigned char> whitePixels(4, 255);

    Texture memoryTexture(const std::string& id) {
        return Texture(id, TextureData(1, 1, 4, ColorFormat::RGBA, 4, whitePixels.data()));
    }

}

// ---- fixes

// a textured Button without a Pressed or Disabled texture keeps its Normal one (98aa06265)
TEST_F(UIComponentsTest, PressedWithoutTextureKeepsNormal) {
    const Entity e = button("B", 0, 0);
    btn(e).textureNormal = memoryTexture("uicomponents|normal");
    start();
    ASSERT_EQ(uic(e).texture.getId(), "uicomponents|normal");

    ui().eventOnPointerDown(-1, D_MOUSE_BUTTON_LEFT, 50, 25, false);
    EXPECT_TRUE(btn(e).pressed);
    EXPECT_EQ(uic(e).texture.getId(), "uicomponents|normal") << "pressed, no Pressed texture: the Normal one";
    ui().eventOnPointerUp(-1, D_MOUSE_BUTTON_LEFT, 50, 25, false);

    btn(e).disabled = true;
    btn(e).needUpdateButton = true;
    frame();
    EXPECT_EQ(uic(e).texture.getId(), "uicomponents|normal") << "disabled, no Disabled texture: the Normal one";
}

// a Layout Group child that gets no box drops its old index (b84e20aa6)
TEST_F(UIComponentsTest, HiddenLayoutChildTakesNoBox) {
    const Entity group = element("Group", 0, 0, 300, 100);
    scene->addComponent<LayoutGroupComponent>(group, {});
    scene->getComponent<LayoutGroupComponent>(group).type = LayoutGroupType::HORIZONTAL;
    element("First", 0, 0, 50, 50, group);
    const Entity middle = element("Middle", 0, 0, 80, 50, group);
    const Entity last = element("Last", 0, 0, 30, 50, group);
    start();
    ASSERT_EQ(scene->getComponent<RectTransformComponent>(middle).containerBoxIndex, 1);

    scene->setEntityActive(middle, false);
    frame();
    frame();
    EXPECT_EQ(scene->getComponent<RectTransformComponent>(middle).containerBoxIndex, -1) << "an inactive child keeps no box";
    EXPECT_EQ(scene->getComponent<RectTransformComponent>(last).containerBoxIndex, 1);
    const LayoutGroupComponent& container = scene->getComponent<LayoutGroupComponent>(group);
    EXPECT_EQ(container.numBoxes, 2u);
    EXPECT_EQ(container.boxes[1].layout, last);
}

// a larger maxTextSize from the Inspector or MCP reloads the text's GPU buffers (a175f0c90)
TEST_F(UIComponentsTest, MaxTextSizeGrowthReloads) {
    const Entity t = entity(scene, "Text");
    scene->addComponent<RectTransformComponent>(t, {});
    scene->addComponent<UIComponent>(t, {});
    TextComponent text;
    text.text = "abc";
    text.maxTextSize = 10;
    scene->addComponent<TextComponent>(t, text);
    start();
    UIComponent& component = uic(t);
    ASSERT_GT(component.buffer.getStride(), 0u);
    // loaded as RenderSystem::loadUI would make it: room for 10 characters
    component.loaded = true;
    component.gpuBufferBytes = (size_t)10 * 4 * component.buffer.getStride();
    component.needReload = false;

    scene->getComponent<TextComponent>(t).maxTextSize = 100;
    editor::Catalog::updateEntity(scene, t, editor::UpdateFlags_Text);
    frame();
    EXPECT_TRUE(uic(t).needReload) << "the buffers of 10 characters cannot take 100";
}

// MCP, undo and drag previews refresh a Button through its fields' update flags (6d0b683f6, 9628666fd)
TEST_F(UIComponentsTest, ButtonFieldEditsRefreshFromAnyPath) {
    const Entity e = button("B", 0, 0);
    start();
    for (const char* property : {"colorNormal", "textureHovered", "transition", "navigationMode", "selectOnEnable"}) {
        const editor::PropertyData prop = editor::Catalog::findProperty(scene, e, editor::ComponentType::ButtonComponent, property);
        EXPECT_TRUE(prop.updateFlags & editor::UpdateFlags_Button) << property;
    }

    const Vector4 red(1, 0, 0, 1);
    btn(e).colorNormal = red;   // as a MultiPropertyCmd writes it, with no Inspector callback
    const editor::PropertyData prop = editor::Catalog::findProperty(scene, e, editor::ComponentType::ButtonComponent, "colorNormal");
    editor::Catalog::updateEntity(scene, e, prop.updateFlags);
    frame();
    EXPECT_EQ(uic(e).color, red);
}

// Engine.setCanvasSize at runtime updates the cameras and the view rect at once (5462d849a)
TEST_F(UIComponentsTest, RuntimeCanvasSizeUpdatesCameras) {
    class ScreenSystem : public System {
    public:
        int getScreenWidth() override { return 1000; }
        int getScreenHeight() override { return 500; }
    } screen;
    System* previous = SystemTestAccess::swap(&screen);
    EngineTestAccess::setViewLoaded(true);
    Engine::setScalingMode(Scaling::STRETCH);
    Engine::setCanvasSize(400, 300);
    Engine::systemViewChanged();
    CameraComponent& camera = scene->getComponent<CameraComponent>(scene->getCamera());
    ASSERT_FLOAT_EQ(camera.rightClip, (float)Engine::getCanvasWidth());

    int changed = 0;
    Engine::onViewChanged.add("uicomponents", std::function<void()>([&]() {
        changed++;
        Engine::setCanvasSize(800, 600);   // a handler that sets it again does not recurse
    }));
    Engine::setCanvasSize(800, 600);
    Engine::onViewChanged.remove("uicomponents");
    EXPECT_EQ(Engine::getCanvasWidth(), 800);
    EXPECT_FLOAT_EQ(camera.rightClip, 800.0f) << "the scene camera follows the runtime canvas at once";
    EXPECT_FLOAT_EQ(camera.topClip, 600.0f);
    EXPECT_EQ(changed, 1);

    EngineTestAccess::setViewLoaded(false);
    SystemTestAccess::swap(previous);
}

// Lua's button.color and button:setTexture write the Normal state, which the state changes keep
TEST_F(UIComponentsTest, LuaButtonColorWritesNormal) {
    const Entity e = button("B", 0, 0);
    start();
    setScene("scn", scene);
    lua("local b = Button(scn, " + std::to_string(e) + "); b.color = Vector4(1, 0, 0, 1)");
    EXPECT_EQ(btn(e).colorNormal, Vector4(1, 0, 0, 1));
    ui().eventOnPointerMove(-1, 50, 25, false);
    ui().eventOnPointerMove(-1, 900, 900, false);
    EXPECT_EQ(uic(e).color, Vector4(1, 0, 0, 1)) << "the hover went and Normal came back red";
}

// ---- no swallowing (the Doriax flaw's control)

// visible Buttons and nothing selected: every key and pad button is the game's
TEST_F(UIComponentsTest, NoSelectionNoSwallow) {
    grid();
    int clicks = 0;
    btn(a).onClick.add("t", std::function<void()>([&]() { clicks++; }));
    start();
    connectPad();
    for (int k : {D_KEY_DOWN, D_KEY_UP, D_KEY_SPACE, D_KEY_ENTER, D_KEY_TAB}) key(k);
    pad(D_GAMEPAD_BUTTON_DPAD_DOWN);
    pad(D_GAMEPAD_BUTTON_A);
    for (int k : {D_KEY_DOWN, D_KEY_UP, D_KEY_SPACE, D_KEY_ENTER, D_KEY_TAB}) {
        EXPECT_EQ(counts["key" + std::to_string(k)], 1) << "key " << k;
    }
    EXPECT_EQ(counts["pad" + std::to_string(D_GAMEPAD_BUTTON_DPAD_DOWN)], 1);
    EXPECT_EQ(counts["pad" + std::to_string(D_GAMEPAD_BUTTON_A)], 1);
    EXPECT_EQ(ui().getSelected(), NULL_ENTITY);
    EXPECT_EQ(clicks, 0);
    EXPECT_FALSE(Engine::wantsGamepadNavigation());
}

// a HUD button clicked with the mouse does not take Space from the game
TEST_F(UIComponentsTest, ClickedHudButtonDoesNotTakeKeys) {
    const Entity hud = button("Pause", 0, 0);
    int clicks = 0;
    btn(hud).onClick.add("t", std::function<void()>([&]() { clicks++; }));
    start();
    ui().eventOnPointerDown(-1, D_MOUSE_BUTTON_LEFT, 50, 25, false);
    ui().eventOnPointerUp(-1, D_MOUSE_BUTTON_LEFT, 50, 25, false);
    EXPECT_EQ(clicks, 1);
    key(D_KEY_SPACE);
    EXPECT_EQ(counts["key" + std::to_string(D_KEY_SPACE)], 1);
    EXPECT_EQ(clicks, 1) << "Space is the game's: the click selected nothing";
    EXPECT_EQ(ui().getSelected(), NULL_ENTITY);
}

// ---- selection and navigation

TEST_F(UIComponentsTest, SelectOnEnableAtStartAndOnShowWithFocusReturn) {
    grid();
    btn(b).selectOnEnable = true;
    btn(d).selectOnEnable = true;
    const Entity panel = element("Submenu", 600, 600, 200, 200);
    const Entity inner = button("Inner", 0, 0);
    scene->addEntityChild(panel, inner, false);
    btn(inner).selectOnEnable = true;
    scene->setEntityActive(panel, false);
    start();
    EXPECT_EQ(ui().getSelected(), b) << "the first Select On Enable in Hierarchy order";
    EXPECT_TRUE(btn(b).selected);

    scene->setEntityActive(panel, true);
    frame();
    EXPECT_EQ(ui().getSelected(), inner) << "a submenu shown later takes the selection";
    scene->setEntityActive(panel, false);
    frame();
    EXPECT_EQ(ui().getSelected(), b) << "closing it gives the selection back";
}

TEST_F(UIComponentsTest, AutomaticGridNavigation) {
    grid();
    start();
    ASSERT_TRUE(ui().setSelected(a));
    key(D_KEY_RIGHT);
    EXPECT_EQ(ui().getSelected(), b);
    key(D_KEY_DOWN);
    EXPECT_EQ(ui().getSelected(), d);
    key(D_KEY_LEFT);
    EXPECT_EQ(ui().getSelected(), c);
    key(D_KEY_UP);
    EXPECT_EQ(ui().getSelected(), a);
    key(D_KEY_UP);
    EXPECT_EQ(ui().getSelected(), a) << "nothing above: it stays";
    EXPECT_EQ(counts["key" + std::to_string(D_KEY_UP)], 0) << "consumed at the menu's edge too";
    EXPECT_EQ(counts["key" + std::to_string(D_KEY_RIGHT)], 0);
}

TEST_F(UIComponentsTest, HorizontalVerticalExplicitWrap) {
    grid();
    start();
    btn(a).navigationMode = NavigationMode::HORIZONTAL;
    EXPECT_EQ(ui().findSelectableOnDirection(a, MoveDirection::DOWN), NULL_ENTITY);
    EXPECT_EQ(ui().findSelectableOnDirection(a, MoveDirection::RIGHT), b);

    btn(a).navigationMode = NavigationMode::EXPLICIT;
    btn(a).selectOnDown = d;
    EXPECT_EQ(ui().findSelectableOnDirection(a, MoveDirection::DOWN), d);
    btn(d).disabled = true;
    EXPECT_EQ(ui().findSelectableOnDirection(a, MoveDirection::DOWN), NULL_ENTITY) << "a target that is not interactable: none";
    btn(d).disabled = false;

    btn(c).navigationMode = NavigationMode::VERTICAL;
    btn(c).wrapAround = true;
    EXPECT_EQ(ui().findSelectableOnDirection(c, MoveDirection::DOWN), a) << "wraps to the top of its column";
    EXPECT_EQ(ui().findSelectableOnDirection(c, MoveDirection::LEFT), NULL_ENTITY);

    btn(b).navigationMode = NavigationMode::NONE;
    EXPECT_EQ(ui().findSelectableOnDirection(d, MoveDirection::UP), a) << "None: B is never a target";
}

TEST_F(UIComponentsTest, TabFollowsHierarchyOrder) {
    grid();
    start();
    ui().setSelected(a);
    key(D_KEY_TAB);
    EXPECT_EQ(ui().getSelected(), b);
    key(D_KEY_TAB, D_MODIFIER_SHIFT);
    EXPECT_EQ(ui().getSelected(), a);
    key(D_KEY_TAB, D_MODIFIER_SHIFT);
    EXPECT_EQ(ui().getSelected(), d) << "wraps";
    key(D_KEY_TAB, D_MODIFIER_CONTROL);
    EXPECT_EQ(counts["key" + std::to_string(D_KEY_TAB)], 1) << "Ctrl+Tab is the game's";
}

TEST_F(UIComponentsTest, SubmitClicksOnDown) {
    grid();
    count(btn(a).onPress, "press");
    count(btn(a).onRelease, "release");
    count(btn(a).onClick, "click");
    count(uic(a).onClick, "legacyClick");
    start();
    ui().setSelected(a);

    Engine::systemKeyDown(D_KEY_ENTER, false, 0);
    EXPECT_TRUE(btn(a).pressed);
    EXPECT_EQ(ui().getButtonState(a), SelectionState::PRESSED);
    EXPECT_EQ(counts["press"], 1);
    EXPECT_EQ(counts["click"], 1) << "Unity's Submit clicks on the press";
    EXPECT_EQ(counts["legacyClick"], 1);
    Engine::systemKeyDown(D_KEY_ENTER, true, 0);
    EXPECT_EQ(counts["click"], 1) << "a repeat does nothing";
    Engine::systemKeyUp(D_KEY_ENTER, false, 0);
    EXPECT_FALSE(btn(a).pressed);
    EXPECT_EQ(counts["release"], 1);
    EXPECT_EQ(counts["key" + std::to_string(D_KEY_ENTER)], 0);

    key(D_KEY_SPACE);
    EXPECT_EQ(counts["click"], 2);
    connectPad();
    pad(D_GAMEPAD_BUTTON_A);
    EXPECT_EQ(counts["click"], 3);
    EXPECT_EQ(counts["pad" + std::to_string(D_GAMEPAD_BUTTON_A)], 0);
}

TEST_F(UIComponentsTest, SubmitHoldEndsWhenTheSceneLeaves) {
    grid();
    count(btn(a).onRelease, "release");
    start();
    ui().setSelected(a);
    Engine::systemKeyDown(D_KEY_ENTER, false, 0);
    ASSERT_TRUE(btn(a).pressed);
    Engine::removeScene(scene);
    EXPECT_FALSE(btn(a).pressed);
    EXPECT_EQ(ui().getSelected(), a) << "the selection stays for when the scene comes back";
    Engine::systemKeyUp(D_KEY_ENTER, false, 0);
    EXPECT_EQ(counts["release"], 0) << "no callbacks into a scene being removed";
}

TEST_F(UIComponentsTest, CancelGoesToHandlerElseGame) {
    writeScript("Menu",
        "local M = {}\n"
        "function M:onCancel(e) log[#log + 1] = 'cancel:' .. e.selectedObject.name end\n"
        "return M\n");
    const Entity menu = element("Menu", 0, 0, 500, 500);
    const Entity inside = button("Inside", 10, 10);
    scene->addEntityChild(menu, inside, false);
    const Entity outside = button("Outside", 600, 600);
    addScript(scene, menu, "Menu");
    start();

    ui().setSelected(inside);
    key(D_KEY_ESCAPE);
    EXPECT_EQ(takeLog(), "cancel:Inside") << "up the hierarchy to the menu's script";
    EXPECT_EQ(counts["key" + std::to_string(D_KEY_ESCAPE)], 0);

    ui().setSelected(outside);
    key(D_KEY_ESCAPE);
    EXPECT_EQ(takeLog(), "");
    EXPECT_EQ(counts["key" + std::to_string(D_KEY_ESCAPE)], 1) << "nobody handles it: the game closes its menu";
}

TEST_F(UIComponentsTest, PointerPressSelectionRules) {
    grid();
    const Entity field = element("Field", 600, 100, 100, 30);
    scene->addComponent<InputFieldComponent>(field, {});
    start();
    ui().setSelected(a);

    ui().eventOnPointerDown(-1, D_MOUSE_BUTTON_LEFT, 260, 110, false);
    ui().eventOnPointerUp(-1, D_MOUSE_BUTTON_LEFT, 260, 110, false);
    EXPECT_EQ(ui().getSelected(), b) << "with a selection, a click selects";

    // a press on the label (or another child) selects its Button
    ui().pointerSelects(btn(c).label);
    EXPECT_EQ(ui().getSelected(), c);

    ui().eventOnPointerDown(-1, D_MOUSE_BUTTON_LEFT, 610, 110, false);
    ui().eventOnPointerUp(-1, D_MOUSE_BUTTON_LEFT, 610, 110, false);
    EXPECT_EQ(ui().getSelected(), c) << "an InputField keeps the selection";

    ui().eventOnPointerDown(-1, D_MOUSE_BUTTON_LEFT, 900, 900, false);
    ui().eventOnPointerUp(-1, D_MOUSE_BUTTON_LEFT, 900, 900, false);
    EXPECT_EQ(ui().getSelected(), NULL_ENTITY) << "a press elsewhere lets go";
}

TEST_F(UIComponentsTest, NonInteractableAndCanvasGroup) {
    grid();
    btn(b).disabled = true;
    start();
    ui().setSelected(a);
    key(D_KEY_RIGHT);
    EXPECT_EQ(ui().getSelected(), a) << "B is not interactable: nothing that way";
    btn(a).disabled = true;
    btn(a).needUpdateButton = true;
    frame();
    EXPECT_EQ(ui().getSelected(), a) << "a selection that became non-interactable stays";
    EXPECT_EQ(ui().getButtonState(a), SelectionState::DISABLED);
    int clicks = 0;
    btn(a).onClick.add("t", std::function<void()>([&]() { clicks++; }));
    key(D_KEY_ENTER);
    EXPECT_EQ(clicks, 0);
    EXPECT_EQ(counts["key" + std::to_string(D_KEY_ENTER)], 0) << "still the menu's key";
}

TEST_F(UIComponentsTest, TopSceneWithSelectionTakesKeys) {
    grid();
    Scene* top = uiScene();
    const Entity x = button("X", 100, 100, top);
    const Entity y = button("Y", 100, 200, top);
    Engine::addSceneLayer(top);
    start();
    LuaBinding::initializeLuaScripts(top);
    frame();
    ui().setSelected(a);
    ui(top).setSelected(x);
    key(D_KEY_DOWN);
    EXPECT_EQ(ui(top).getSelected(), y);
    EXPECT_EQ(ui().getSelected(), a) << "only the top scene with a selection navigates";
    ui(top).setSelected(NULL_ENTITY);
    key(D_KEY_DOWN);
    EXPECT_EQ(ui().getSelected(), c);
}

// a field typed in, in any scene, keeps the keys it types with; the game does not get them
TEST_F(UIComponentsTest, TypingGuardAcrossScenes) {
    const Entity field = element("Field", 0, 0, 200, 30);
    const Entity text = element("Text", 0, 0, 200, 30, field);
    scene->removeComponent<ImageComponent>(text);
    scene->addComponent<TextComponent>(text, {});
    scene->getComponent<RectTransformComponent>(text).ignoreEvents = true;
    InputFieldComponent inputfield;
    inputfield.text = text;
    scene->addComponent<InputFieldComponent>(field, inputfield);
    Scene* top = uiScene();
    const Entity x = button("X", 300, 300, top);
    button("Y", 300, 400, top);
    Engine::addSceneLayer(top);
    start();
    LuaBinding::initializeLuaScripts(top);
    frame();
    ui(top).setSelected(x);

    ui().eventOnPointerDown(-1, D_MOUSE_BUTTON_LEFT, 10, 10, false);
    ui().eventOnPointerUp(-1, D_MOUSE_BUTTON_LEFT, 10, 10, false);
    ASSERT_TRUE(uic(field).focused);

    Engine::systemKeyDown(D_KEY_W, false, 0);
    Engine::systemCharInput(U'w');
    Engine::systemKeyUp(D_KEY_W, false, 0);
    EXPECT_EQ(counts["key" + std::to_string(D_KEY_W)], 0) << "the player does not walk while typing";
    EXPECT_EQ(counts["char"], 0);
    EXPECT_EQ(scene->getComponent<TextComponent>(text).text, "w");

    key(D_KEY_DOWN);
    Engine::systemCharInput(U' ');
    EXPECT_EQ(ui(top).getSelected(), x) << "the upper scene's menu does not move";
    EXPECT_EQ(scene->getComponent<TextComponent>(text).text, "w ") << "Space typed, not a click";
}

TEST_F(UIComponentsTest, InputActionsSuppressedWhileSelected) {
    grid();
    start();
    EXPECT_FALSE(Engine::wantsKeyboardNavigation());
    ui().setSelected(a);
    EXPECT_TRUE(Engine::wantsKeyboardNavigation());
    EXPECT_TRUE(Engine::wantsGamepadNavigation());
    ui().setSelected(NULL_ENTITY);
    EXPECT_FALSE(Engine::wantsGamepadNavigation());
}

// the d-pad and the stick move a selection; a stick held while the menu goes reaches the game
TEST_F(UIComponentsTest, GamepadNavigationAndStickHandBack) {
    grid();
    start();
    connectPad();
    ui().setSelected(a);
    pad(D_GAMEPAD_BUTTON_DPAD_RIGHT);
    EXPECT_EQ(ui().getSelected(), b);
    EXPECT_EQ(counts["pad" + std::to_string(D_GAMEPAD_BUTTON_DPAD_RIGHT)], 0);

    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_LEFT_Y, 0.9f);
    EXPECT_EQ(ui().getSelected(), d) << "the stick moves it as the d-pad does";
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_LEFT_X, 0.2f);
    EXPECT_FLOAT_EQ(lastAxisX, 0.0f) << "the game does not get the stick while the menu has it";

    ui().setSelected(NULL_ENTITY);
    EngineTestAccess::updateGamepadNavigation();
    EXPECT_FLOAT_EQ(lastAxisX, 0.2f) << "the menu went: the held stick goes back to the game";
}

// ---- transitions

TEST_F(UIComponentsTest, ColorTintFadesOnTheUIClock) {
    const Entity e = button("B", 0, 0);
    btn(e).colorHovered = Vector4(0, 0, 0, 1);
    btn(e).fadeDuration = 0.2f;
    start();
    ASSERT_EQ(uic(e).color, Vector4(1, 1, 1, 1));
    Engine::setTimeScale(0.0f);
    ui().eventOnPointerMove(-1, 50, 25, false);
    HtmlUIClock::setFixedTime(0.1);
    frame();
    EXPECT_NEAR(uic(e).color.x, 0.5f, 0.01f) << "half way after half the fade, at time scale 0 too";
    HtmlUIClock::setFixedTime(0.3);
    frame();
    EXPECT_EQ(uic(e).color, Vector4(0, 0, 0, 1));
    Engine::setTimeScale(1.0f);
}

TEST_F(UIComponentsTest, TransitionOwnership) {
    const Entity e = button("B", 0, 0);
    btn(e).colorHovered = Vector4(0, 1, 0, 1);
    btn(e).textureNormal = memoryTexture("uicomponents|n");
    btn(e).textureHovered = memoryTexture("uicomponents|h");
    btn(e).transition = ButtonTransition::SPRITE_SWAP;
    start();
    uic(e).color = Vector4(0, 0, 1, 1);
    ui().eventOnPointerMove(-1, 50, 25, false);
    EXPECT_EQ(uic(e).texture.getId(), "uicomponents|h");
    EXPECT_EQ(uic(e).color, Vector4(0, 0, 1, 1)) << "Sprite Swap leaves the colour alone";

    btn(e).transition = ButtonTransition::COLOR_TINT;
    btn(e).needUpdateButton = true;
    frame();
    EXPECT_EQ(uic(e).texture.getId(), "uicomponents|n") << "no longer swapped: the Normal texture once";
    EXPECT_EQ(uic(e).color, Vector4(0, 1, 0, 1));

    btn(e).transition = ButtonTransition::NONE;
    btn(e).needUpdateButton = true;
    frame();
    uic(e).color = Vector4(1, 0, 1, 1);
    ui().eventOnPointerMove(-1, 900, 900, false);
    EXPECT_EQ(uic(e).color, Vector4(1, 0, 1, 1)) << "None writes nothing";
}

TEST_F(UIComponentsTest, ScaleTransitionKeepsScriptScale) {
    const Entity e = button("B", 0, 0);
    btn(e).scaleHovered = 2.0f;
    start();
    Transform& transform = scene->getComponent<Transform>(e);
    ui().eventOnPointerMove(-1, 50, 25, false);
    frame();
    EXPECT_FLOAT_EQ(transform.scale.x, 2.0f);
    transform.scale = transform.scale * 3.0f;   // the game scales it meanwhile
    ui().eventOnPointerMove(-1, 900, 900, false);
    frame();
    EXPECT_FLOAT_EQ(transform.scale.x, 3.0f) << "the hover's factor goes, the game's stays";
}

TEST_F(UIComponentsTest, ScriptMessagesFromLua) {
    writeScript("Item",
        "local M = {}\n"
        "function M:onSelect(e) log[#log + 1] = 'select' .. self.gameObject.name end\n"
        "function M:onDeselect(e) log[#log + 1] = 'deselect' .. self.gameObject.name end\n"
        "function M:onMove(e) log[#log + 1] = 'move' .. tostring(e.moveDir == MoveDirection.RIGHT); if self.gameObject.name == 'C' then e:use() end end\n"
        "function M:onSubmit(e) log[#log + 1] = 'submit'; e:use() end\n"
        "return M\n");
    grid();
    for (Entity e : {a, b, c}) addScript(scene, e, "Item");
    int clicks = 0;
    btn(a).onClick.add("t", std::function<void()>([&]() { clicks++; }));
    start();
    clearLog();
    ui().setSelected(a);
    EXPECT_EQ(takeLog(), "selectA");
    key(D_KEY_RIGHT);
    EXPECT_EQ(takeLog(), "movetrue deselectA selectB");
    ui().setSelected(c);
    takeLog();
    key(D_KEY_RIGHT);
    EXPECT_EQ(ui().getSelected(), c) << "onMove used: no navigation";
    ui().setSelected(a);
    takeLog();
    key(D_KEY_ENTER);
    EXPECT_EQ(takeLog(), "submit");
    EXPECT_EQ(clicks, 0) << "onSubmit used: no click";

    setScene("scn", scene);
    EXPECT_EQ(lua("return UI.getSelected(scn).name"), "A");
    EXPECT_EQ(lua("return tostring(UI.setSelected(scn, nil))"), "true");
    EXPECT_EQ(ui().getSelected(), NULL_ENTITY);
}
