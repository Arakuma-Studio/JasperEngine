// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Canvas Group (feature ui-canvas-group, spec section 10): propagation of alpha /
// interactable / blocksRaycasts over the chain, ignoreParentGroups, the widgets' interactability
// (Button, InputField, Scrollbar, Panel), raycasts and drag continuity, the dirty detection
// (getCanvasGroupStats), the draw colour per blend mode and the draw skip, UI particles,
// serialization (Stream, Factory), the AlphaTween target and unscaled tweens, the CanvasGroup
// fades and the Lua API. The scenes are built in code; positions are set as world positions
// (no RenderSystem runs), the pointer goes through UISystem's entry points.

#include <gtest/gtest.h>

#include "ScriptDataTestSupport.h"

#include "Scene.h"
#include "Engine.h"
#include "subsystem/UISystem.h"
#include "subsystem/ActionSystem.h"
#include "render/UIDrawColor.h"
#include "object/ui/CanvasGroup.h"
#include "action/AlphaTween.h"
#include "component/CanvasGroupComponent.h"
#include "component/ButtonComponent.h"
#include "component/ImageComponent.h"
#include "component/InputFieldComponent.h"
#include "component/PanelComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/PointsComponent.h"
#include "Stream.h"
#include "Factory.h"
#include "Catalog.h"
#include "animation/CanvasGroupTracks.h"

#include "LuaBridge.h"

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // A UI hierarchy built by hand: every element a RectTransform at a world rect
    class CanvasGroupTest : public ::testing::Test {
    protected:
        Scene scene;
        lua_State* L = nullptr;

        void SetUp() override {
            // the engine (System, Lua) made once for the binary: InputField focus shows the
            // virtual keyboard through System::instance(), the Lua tests need the state
            L = scriptdata::engineLua();
        }

        UISystem* ui() { return scene.getSystem<UISystem>().get(); }

        Entity node(Entity parent, float x, float y, float w, float h, bool withUI = false, bool withImage = false){
            Entity entity = scene.createEntity();
            Transform transform;
            transform.worldPosition = Vector3(x, y, 0);
            scene.addComponent<Transform>(entity, transform);
            RectTransformComponent layout;
            layout.width = (unsigned int)w;
            layout.height = (unsigned int)h;
            scene.addComponent<RectTransformComponent>(entity, layout);
            if (withUI || withImage){
                scene.addComponent<UIComponent>(entity, UIComponent());
            }
            if (withImage){
                scene.addComponent<ImageComponent>(entity, ImageComponent());
            }
            if (parent != NULL_ENTITY){
                scene.addEntityChild(parent, entity, false);
            }
            return entity;
        }

        Entity image(Entity parent, float x, float y, float w, float h){
            return node(parent, x, y, w, h, true, true);
        }

        Entity container(Entity parent){
            return node(parent, 0, 0, 400, 400);
        }

        CanvasGroupComponent& addGroup(Entity entity, float alpha = 1.0f){
            CanvasGroupComponent group;
            group.alpha = alpha;
            scene.addComponent<CanvasGroupComponent>(entity, group);
            return scene.getComponent<CanvasGroupComponent>(entity);
        }

        CanvasGroupComponent& group(Entity entity){
            return scene.getComponent<CanvasGroupComponent>(entity);
        }

        UIComponent& uic(Entity entity){
            return scene.getComponent<UIComponent>(entity);
        }

        Entity button(Entity parent, float x, float y, float w, float h){
            Entity entity = image(parent, x, y, w, h);
            scene.addComponent<ButtonComponent>(entity, ButtonComponent());
            return entity;
        }

        void click(float x, float y){
            ui()->eventOnPointerDown(x, y);
            ui()->eventOnPointerUp(x, y);
        }

        // runs the Lua chunk with `scene` as a global; "" or the error
        std::string lua(const std::string& code){
            luabridge::setGlobal(L, &scene, "scene");
            const int top = lua_gettop(L);
            std::string error;
            if (luaL_loadbuffer(L, code.data(), code.size(), "=canvasgroup") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
                const char* message = lua_tostring(L, -1);
                error = message ? message : "(error)";
            }
            lua_settop(L, top);
            lua_pushnil(L);
            lua_setglobal(L, "scene");
            return error;
        }

        double luaNumber(const std::string& expression){
            luabridge::setGlobal(L, &scene, "scene");
            const std::string code = "return " + expression;
            double value = NAN;
            if (luaL_loadbuffer(L, code.data(), code.size(), "=canvasgroup") == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK){
                if (lua_isnumber(L, -1)) value = lua_tonumber(L, -1);
                else if (lua_isboolean(L, -1)) value = lua_toboolean(L, -1) ? 1.0 : 0.0;
                else if (lua_isnil(L, -1)) value = -1.0;
            }
            lua_settop(L, 0);
            lua_pushnil(L);
            lua_setglobal(L, "scene");
            return value;
        }
    };

}

// ---- 1. propagation

TEST_F(CanvasGroupTest, AlphaMultipliesDownTheChain){
    Entity root = container(NULL_ENTITY);
    addGroup(root, 0.5f);
    Entity child = image(root, 0, 0, 10, 10);
    uic(child).color = Vector4(1, 1, 1, 0.8f);
    ui()->syncCanvasGroups();

    EXPECT_FLOAT_EQ(uic(child).groupAlpha, 0.5f);
    EXPECT_NEAR(uiDrawColor(uic(child).color, uic(child).groupAlpha, MaterialBlendMode::ALPHA).w, 0.4f, 1e-6f);
    EXPECT_EQ(uic(child).groupEntity, root);
    // the element's own colour is not changed by the group
    EXPECT_FLOAT_EQ(uic(child).color.w, 0.8f);

    // a nested group multiplies
    Entity inner = container(root);
    addGroup(inner, 0.5f);
    Entity grandchild = image(inner, 0, 0, 10, 10);
    ui()->syncCanvasGroups();
    EXPECT_FLOAT_EQ(uic(grandchild).groupAlpha, 0.25f);
    EXPECT_EQ(uic(grandchild).groupEntity, inner);

    // a disabled group counts as 1
    group(inner).enabled = false;
    ui()->syncCanvasGroups();
    EXPECT_FLOAT_EQ(uic(grandchild).groupAlpha, 0.5f);
    EXPECT_EQ(uic(grandchild).groupEntity, root);

    // the on-demand walk agrees
    EXPECT_FLOAT_EQ(ui()->getCanvasGroupState(grandchild).alpha, 0.5f);
}

// ---- 2. ignoreParentGroups

TEST_F(CanvasGroupTest, IgnoreParentGroupsStopsTheChain){
    Entity root = container(NULL_ENTITY);
    addGroup(root, 0.5f).interactable = false;
    Entity child = container(root);
    addGroup(child, 0.8f).ignoreParentGroups = true;
    Entity element = image(child, 0, 0, 10, 10);
    ui()->syncCanvasGroups();
    EXPECT_FLOAT_EQ(uic(element).groupAlpha, 0.8f);
    EXPECT_TRUE(uic(element).groupInteractable);

    // under a parent at 0 the child is still drawn at its own alpha (no Unity cull)
    group(root).alpha = 0.0f;
    ui()->syncCanvasGroups();
    EXPECT_FLOAT_EQ(uic(element).groupAlpha, 0.8f);
    EXPECT_FALSE(uiDrawSkipped(uic(element).color, uic(element).groupAlpha, MaterialBlendMode::ALPHA, false));
}

// ---- 3. interactable

TEST_F(CanvasGroupTest, NonInteractableGroupDisablesButtons){
    Entity root = container(NULL_ENTITY);
    addGroup(root).interactable = false;
    Entity b = button(root, 10, 10, 50, 20);
    int presses = 0, clicks = 0;
    scene.getComponent<ButtonComponent>(b).onPress.add("t", std::function<void()>([&](){ presses++; }));
    uic(b).onClick.add("t", std::function<void(float, float)>([&](float, float){ clicks++; }));

    click(20, 15);
    EXPECT_FALSE(scene.getComponent<ButtonComponent>(b).pressed);
    EXPECT_EQ(presses, 0);
    EXPECT_EQ(clicks, 0);
    // the disabled visual (Unity's default disabledColor)
    EXPECT_EQ(uic(b).color, scene.getComponent<ButtonComponent>(b).colorDisabled);
    EXPECT_FALSE(ui()->isInteractable(b));

    // R3: interactable again restores the normal visual; an own disabled flag stays disabled
    group(root).interactable = true;
    ui()->syncCanvasGroups();
    EXPECT_EQ(uic(b).color, scene.getComponent<ButtonComponent>(b).colorNormal);
    click(20, 15);
    EXPECT_EQ(presses, 1);
    EXPECT_EQ(clicks, 1);
    scene.getComponent<ButtonComponent>(b).disabled = true;
    EXPECT_FALSE(ui()->isInteractable(b));
}

TEST_F(CanvasGroupTest, GroupOnTheButtonItselfCounts){
    Entity b = button(NULL_ENTITY, 10, 10, 50, 20);
    addGroup(b).interactable = false;
    int clicks = 0;
    uic(b).onClick.add("t", std::function<void(float, float)>([&](float, float){ clicks++; }));
    click(20, 15);
    EXPECT_EQ(clicks, 0) << "R1: the group on the element itself counts";
}

TEST_F(CanvasGroupTest, ImageCallbacksStillFireInANonInteractableGroup){
    Entity root = container(NULL_ENTITY);
    addGroup(root).interactable = false;
    Entity img = image(root, 0, 0, 50, 50);
    int clicks = 0;
    uic(img).onClick.add("t", std::function<void(float, float)>([&](float, float){ clicks++; }));
    click(10, 10);
    EXPECT_EQ(clicks, 1) << "legacy pointer callbacks fire, as EventTrigger in Unity";
}

TEST_F(CanvasGroupTest, InputFieldCannotFocusAndLosesFocusOnce){
    Entity root = container(NULL_ENTITY);
    addGroup(root).interactable = false;
    Entity field = image(root, 0, 0, 100, 30);
    scene.addComponent<InputFieldComponent>(field, InputFieldComponent());
    click(10, 10);
    EXPECT_FALSE(uic(field).focused);

    // focused, then the group turns off: onLostFocus exactly once, in the next update
    group(root).interactable = true;
    ui()->syncCanvasGroups();
    click(10, 10);
    ASSERT_TRUE(uic(field).focused);
    int lost = 0;
    uic(field).onLostFocus.add("t", std::function<void()>([&](){ lost++; }));
    group(root).interactable = false;
    ui()->syncCanvasGroups();
    ui()->update(0);
    ui()->update(0);
    EXPECT_FALSE(uic(field).focused);
    EXPECT_EQ(lost, 1);
}

TEST_F(CanvasGroupTest, ScrollbarAndPanelGesturesAreIgnored){
    Entity root = container(NULL_ENTITY);
    addGroup(root).interactable = false;
    Entity bar = image(root, 0, 0, 20, 200);
    ScrollbarComponent scrollbar;
    scrollbar.barSize = 0.5f;
    scene.addComponent<ScrollbarComponent>(bar, scrollbar);
    ui()->eventOnPointerDown(5, 150);
    EXPECT_FALSE(scene.getComponent<ScrollbarComponent>(bar).barPointerDown);
    ui()->eventOnPointerUp(5, 150);

    Entity panel = image(root, 100, 100, 100, 100);
    PanelComponent panelcomp;
    panelcomp.canResize = true;
    scene.addComponent<PanelComponent>(panel, panelcomp);
    ui()->eventOnPointerDown(199, 150);
    EXPECT_EQ(scene.getComponent<PanelComponent>(panel).edgePointerDown, PanelEdge::NONE);
    EXPECT_FALSE(scene.getComponent<PanelComponent>(panel).headerPointerDown);
    ui()->eventOnPointerUp(199, 150);
}

// ---- 4. raycasts

TEST_F(CanvasGroupTest, RaycastsGoThroughNonBlockingGroups){
    Entity back = image(NULL_ENTITY, 0, 0, 100, 100);
    Entity frontRoot = container(NULL_ENTITY);
    addGroup(frontRoot).blocksRaycasts = false;
    Entity front = image(frontRoot, 0, 0, 100, 100);

    EXPECT_EQ(ui()->raycast(Vector2(50, 50)), back);
    int backClicks = 0;
    uic(back).onClick.add("t", std::function<void(float, float)>([&](float, float){ backClicks++; }));
    click(50, 50);
    EXPECT_EQ(backClicks, 1);

    // blocking again: the front is hit, also at alpha 0 (decision 5)
    group(frontRoot).blocksRaycasts = true;
    EXPECT_EQ(ui()->raycast(Vector2(50, 50)), front);
    group(frontRoot).alpha = 0.0f;
    EXPECT_EQ(ui()->raycast(Vector2(50, 50)), front);

    // an ignoreParentGroups child that blocks, under a parent that does not, is hit
    group(frontRoot).blocksRaycasts = false;
    Entity inner = container(frontRoot);
    addGroup(inner).ignoreParentGroups = true;
    Entity innerImage = image(inner, 0, 0, 100, 100);
    EXPECT_EQ(ui()->raycast(Vector2(50, 50)), innerImage);
}

TEST_F(CanvasGroupTest, PointerOverUIIsFalseOverANonBlockingElement){
    Entity root = container(NULL_ENTITY);
    addGroup(root).blocksRaycasts = false;
    image(root, 0, 0, 100, 100);
    ui()->eventOnPointerMove(50, 50);
    EXPECT_FALSE(ui()->isPointerOverUI());
    group(root).blocksRaycasts = true;
    ui()->eventOnPointerMove(51, 50);
    EXPECT_TRUE(ui()->isPointerOverUI());
}

// ---- 5. drag continuity (Garden GInventorySlot)

TEST_F(CanvasGroupTest, DraggedItemKeepsItsDragAndTheDropFindsTheSlotBehind){
    Entity slot = image(NULL_ENTITY, 0, 0, 100, 100);
    Entity item = image(NULL_ENTITY, 0, 0, 100, 100);
    CanvasGroupComponent& itemGroup = addGroup(item);
    (void)itemGroup;
    int drags = 0, dragEnds = 0;
    uic(item).onDragStart.add("t", std::function<void(float, float)>([&](float, float){
        // the game's onBeginDrag
        group(item).alpha = 0.6f;
        group(item).blocksRaycasts = false;
    }));
    uic(item).onDrag.add("t", std::function<void(float, float)>([&](float, float){ drags++; }));
    uic(item).onDragEnd.add("t", std::function<void(float, float)>([&](float, float){ dragEnds++; }));

    ui()->eventOnPointerDown(50, 50);
    ui()->eventOnPointerMove(60, 50);   // past the drag distance: onDragStart, onDrag
    ui()->eventOnPointerMove(70, 50);
    ui()->eventOnPointerMove(80, 50);
    EXPECT_EQ(drags, 3) << "the drag keeps arriving after the group stopped blocking";
    EXPECT_EQ(ui()->raycast(Vector2(80, 50)), slot) << "the drop target is the slot behind";
    ui()->eventOnPointerUp(80, 50);
    EXPECT_EQ(dragEnds, 1);
}

// ---- 6. press then disable

TEST_F(CanvasGroupTest, PressThenDisableReleasesOnceWithoutClick){
    Entity root = container(NULL_ENTITY);
    addGroup(root);
    Entity b = button(root, 0, 0, 50, 50);
    int releases = 0, clicks = 0;
    scene.getComponent<ButtonComponent>(b).onRelease.add("t", std::function<void()>([&](){ releases++; }));
    uic(b).onClick.add("t", std::function<void(float, float)>([&](float, float){ clicks++; }));

    ui()->eventOnPointerDown(10, 10);
    ASSERT_TRUE(scene.getComponent<ButtonComponent>(b).pressed);
    group(root).interactable = false;
    ui()->syncCanvasGroups();
    ui()->update(0);
    EXPECT_EQ(releases, 1);
    EXPECT_FALSE(scene.getComponent<ButtonComponent>(b).pressed);
    ui()->eventOnPointerUp(10, 10);
    EXPECT_EQ(releases, 1);
    EXPECT_EQ(clicks, 0);
}

// ---- 7. dirty propagation

TEST_F(CanvasGroupTest, NoWalkWhenNothingChanged){
    Entity root = container(NULL_ENTITY);
    addGroup(root, 0.5f);
    Entity a = image(root, 0, 0, 10, 10);
    image(a, 0, 0, 10, 10);
    image(root, 0, 0, 10, 10);
    Entity other = image(NULL_ENTITY, 0, 0, 10, 10);
    (void)other;
    ui()->syncCanvasGroups();

    ui()->resetCanvasGroupStats();
    for (int i = 0; i < 10; i++) ui()->syncCanvasGroups();
    EXPECT_EQ(ui()->getCanvasGroupStats().visited, 0u);
    EXPECT_EQ(ui()->getCanvasGroupStats().fullPasses, 0u);

    // an alpha change of a group with 3 descendants: one subtree pass over 4 entities
    group(root).alpha = 0.25f;
    ui()->syncCanvasGroups();
    EXPECT_EQ(ui()->getCanvasGroupStats().subtreePasses, 1u);
    EXPECT_EQ(ui()->getCanvasGroupStats().fullPasses, 0u);
    EXPECT_EQ(ui()->getCanvasGroupStats().visited, 4u);
    EXPECT_FLOAT_EQ(uic(a).groupAlpha, 0.25f);

    // a reparent: one full pass
    ui()->resetCanvasGroupStats();
    scene.addEntityChild(root, other, false);
    ui()->syncCanvasGroups();
    EXPECT_EQ(ui()->getCanvasGroupStats().fullPasses, 1u);
    EXPECT_FLOAT_EQ(uic(other).groupAlpha, 0.25f);
}

TEST_F(CanvasGroupTest, NestedDirtyGroupsAreWalkedOnce){
    Entity root = container(NULL_ENTITY);
    addGroup(root, 0.5f);
    Entity inner = container(root);
    addGroup(inner, 0.5f);
    Entity element = image(inner, 0, 0, 10, 10);
    ui()->syncCanvasGroups();
    ui()->resetCanvasGroupStats();
    group(root).alpha = 0.8f;
    group(inner).alpha = 0.25f;
    ui()->syncCanvasGroups();
    EXPECT_EQ(ui()->getCanvasGroupStats().subtreePasses, 1u);
    EXPECT_FLOAT_EQ(uic(element).groupAlpha, 0.2f);
}

TEST_F(CanvasGroupTest, LastGroupRemovedResetsTheElements){
    Entity root = container(NULL_ENTITY);
    addGroup(root, 0.5f).interactable = false;
    Entity element = image(root, 0, 0, 10, 10);
    ui()->syncCanvasGroups();
    ASSERT_FLOAT_EQ(uic(element).groupAlpha, 0.5f);
    scene.removeComponent<CanvasGroupComponent>(root);
    ui()->syncCanvasGroups();
    EXPECT_FLOAT_EQ(uic(element).groupAlpha, 1.0f);
    EXPECT_TRUE(uic(element).groupInteractable);
    EXPECT_EQ(uic(element).groupEntity, NULL_ENTITY);
    // with no group at all nothing runs
    ui()->resetCanvasGroupStats();
    ui()->syncCanvasGroups();
    EXPECT_EQ(ui()->getCanvasGroupStats().visited, 0u);
}

TEST_F(CanvasGroupTest, RuntimeElementGetsItsStateBeforeItsFirstRaycast){
    Entity root = container(NULL_ENTITY);
    addGroup(root).blocksRaycasts = false;
    ui()->syncCanvasGroups();
    Entity late = image(root, 0, 0, 50, 50);
    EXPECT_EQ(ui()->raycast(Vector2(10, 10)), NULL_ENTITY) << "R5: the lazy sync covers a new element";
    (void)late;
}

TEST_F(CanvasGroupTest, AlphaIsClampedAndNaNRejected){
    Entity root = container(NULL_ENTITY);
    addGroup(root, 0.5f);
    Entity element = image(root, 0, 0, 10, 10);
    ui()->syncCanvasGroups();
    group(root).alpha = 1.5f;
    ui()->syncCanvasGroups();
    EXPECT_FLOAT_EQ(group(root).alpha, 1.0f);
    group(root).alpha = NAN;
    ui()->syncCanvasGroups();
    EXPECT_FLOAT_EQ(group(root).alpha, 1.0f) << "the value seen last stays";
    EXPECT_FLOAT_EQ(uic(element).groupAlpha, 1.0f);
}

// ---- 8. blend modes and the draw skip

TEST(CanvasGroupDrawColor, EveryBlendMode){
    const Vector4 color(0.5f, 0.25f, 1.0f, 0.8f);
    EXPECT_EQ(uiDrawColor(color, 1.0f, MaterialBlendMode::ALPHA), color);
    for (MaterialBlendMode mode : {MaterialBlendMode::ALPHA, MaterialBlendMode::ADDITIVE, MaterialBlendMode::AUTO}){
        const Vector4 c = uiDrawColor(color, 0.5f, mode);
        EXPECT_FLOAT_EQ(c.x, 0.5f);
        EXPECT_FLOAT_EQ(c.w, 0.4f);
    }
    const Vector4 pre = uiDrawColor(color, 0.5f, MaterialBlendMode::PREMULTIPLIED);
    EXPECT_FLOAT_EQ(pre.x, 0.25f);
    EXPECT_FLOAT_EQ(pre.y, 0.125f);
    EXPECT_FLOAT_EQ(pre.z, 0.5f);
    EXPECT_FLOAT_EQ(pre.w, 0.4f);
    const Vector4 mul = uiDrawColor(color, 0.5f, MaterialBlendMode::MULTIPLY);
    EXPECT_FLOAT_EQ(mul.x, 0.75f);
    EXPECT_FLOAT_EQ(mul.y, 0.625f);
    EXPECT_FLOAT_EQ(mul.z, 1.0f);
    EXPECT_FLOAT_EQ(mul.w, 0.8f);
    EXPECT_EQ(uiDrawColor(color, 0.5f, MaterialBlendMode::NONE), color);
}

TEST(CanvasGroupDrawColor, SkipAtZeroExceptStencilWriters){
    const Vector4 color(1, 1, 1, 1);
    EXPECT_TRUE(uiDrawSkipped(color, 0.0f, MaterialBlendMode::ALPHA, false));
    EXPECT_TRUE(uiDrawSkipped(color, 0.0f, MaterialBlendMode::NONE, false)) << "a group at 0 hides every mode";
    EXPECT_FALSE(uiDrawSkipped(color, 0.0f, MaterialBlendMode::ALPHA, true)) << "a mask keeps drawing";
    EXPECT_FALSE(uiDrawSkipped(color, 0.5f, MaterialBlendMode::ALPHA, false));
    EXPECT_TRUE(uiDrawSkipped(Vector4(1, 1, 1, 0), 0.5f, MaterialBlendMode::ALPHA, false));
    // outside any group nothing changes, even for an element at alpha 0
    EXPECT_FALSE(uiDrawSkipped(Vector4(1, 1, 1, 0), 1.0f, MaterialBlendMode::ALPHA, false));
}

// ---- 9. UI particles

TEST_F(CanvasGroupTest, PointsUnderAGroupAreFaded){
    Entity root = container(NULL_ENTITY);
    addGroup(root, 0.5f);
    Entity points = scene.createEntity();
    scene.addComponent<Transform>(points, Transform());
    scene.addComponent<PointsComponent>(points, PointsComponent());
    scene.addEntityChild(root, points, false);
    ui()->syncCanvasGroups();
    EXPECT_FLOAT_EQ(scene.getComponent<PointsComponent>(points).uiGroupAlpha, 0.5f);
    EXPECT_TRUE(scene.getComponent<PointsComponent>(points).needUpdate);

    // a Points outside the UI hierarchy is no UI particle
    Entity world = scene.createEntity();
    scene.addComponent<Transform>(world, Transform());
    CanvasGroupComponent worldGroup;
    worldGroup.alpha = 0.5f;
    scene.addComponent<CanvasGroupComponent>(world, worldGroup);
    Entity worldPoints = scene.createEntity();
    scene.addComponent<Transform>(worldPoints, Transform());
    scene.addComponent<PointsComponent>(worldPoints, PointsComponent());
    scene.addEntityChild(world, worldPoints, false);
    ui()->syncCanvasGroups();
    EXPECT_FLOAT_EQ(scene.getComponent<PointsComponent>(worldPoints).uiGroupAlpha, 1.0f);
}

// ---- 10. serialization

TEST_F(CanvasGroupTest, StreamRoundTripAndDefaults){
    CanvasGroupComponent group;
    group.enabled = false;
    group.alpha = 0.25f;
    group.interactable = false;
    group.blocksRaycasts = false;
    group.ignoreParentGroups = true;
    group.renderMode = CanvasGroupRenderMode::FLATTEN;
    YAML::Node node = Stream::encodeCanvasGroupComponent(group);
    EXPECT_EQ(node["renderMode"].as<std::string>(), "FLATTEN");
    CanvasGroupComponent decoded = Stream::decodeCanvasGroupComponent(node);
    EXPECT_FALSE(decoded.enabled);
    EXPECT_FLOAT_EQ(decoded.alpha, 0.25f);
    EXPECT_FALSE(decoded.interactable);
    EXPECT_FALSE(decoded.blocksRaycasts);
    EXPECT_TRUE(decoded.ignoreParentGroups);
    EXPECT_EQ(decoded.renderMode, CanvasGroupRenderMode::FLATTEN);
    // byte stable
    YAML::Emitter first, second;
    first << node;
    second << Stream::encodeCanvasGroupComponent(decoded);
    EXPECT_EQ(std::string(first.c_str()), std::string(second.c_str()));

    // missing keys: the defaults; an alpha out of range is clamped
    CanvasGroupComponent partial = Stream::decodeCanvasGroupComponent(YAML::Load("{alpha: 3}"));
    EXPECT_TRUE(partial.enabled);
    EXPECT_FLOAT_EQ(partial.alpha, 1.0f);
    EXPECT_TRUE(partial.interactable);
    EXPECT_TRUE(partial.blocksRaycasts);
    EXPECT_FALSE(partial.ignoreParentGroups);
    EXPECT_EQ(partial.renderMode, CanvasGroupRenderMode::PER_ELEMENT);
}

TEST_F(CanvasGroupTest, OldTweensEncodeUnchanged){
    // a tween of an old scene (no unscaled time, AUTO target) writes no new keys
    TweenComponent tween;
    EXPECT_FALSE(Stream::encodeTweenComponent(tween)["useUnscaledTime"]);
    AlphaTweenComponent alpha{1.0f, 0.0f};
    EXPECT_FALSE(Stream::encodeAlphaTweenComponent(alpha)["alphaTarget"]);
    alpha.alphaTarget = AlphaTweenTarget::CANVAS_GROUP;
    tween.useUnscaledTime = true;
    EXPECT_EQ(Stream::decodeAlphaTweenComponent(Stream::encodeAlphaTweenComponent(alpha)).alphaTarget, AlphaTweenTarget::CANVAS_GROUP);
    EXPECT_TRUE(Stream::decodeTweenComponent(Stream::encodeTweenComponent(tween)).useUnscaledTime);
}

TEST_F(CanvasGroupTest, FactoryEmitsTheGroup){
    Entity root = container(NULL_ENTITY);
    CanvasGroupComponent& g = addGroup(root, 0.5f);
    g.interactable = false;
    g.renderMode = CanvasGroupRenderMode::FLATTEN;
    const std::string code = Factory::createCanvasGroupComponent(4, &scene, root, "scene", "entity");
    EXPECT_NE(code.find("CanvasGroupComponent canvasgroup;"), std::string::npos) << code;
    EXPECT_NE(code.find("canvasgroup.alpha = 0.5"), std::string::npos) << code;
    EXPECT_NE(code.find("canvasgroup.interactable = false;"), std::string::npos) << code;
    EXPECT_NE(code.find("canvasgroup.renderMode = CanvasGroupRenderMode::FLATTEN;"), std::string::npos) << code;
    EXPECT_NE(code.find("CanvasGroupComponent"), std::string::npos);
    // the generic dispatch reaches it
    EXPECT_EQ(Factory::createComponent(4, &scene, root, ComponentType::CanvasGroupComponent, std::filesystem::path(), "scene", "entity"), code);
}

// ---- 11. tweens

TEST_F(CanvasGroupTest, AlphaTweenAutoWritesTheGroupAndColorWritesTheColour){
    Entity root = image(NULL_ENTITY, 0, 0, 10, 10);
    addGroup(root, 1.0f);
    AlphaTween tween(&scene);
    tween.setAction(1.0f, 0.0f, 1.0f);
    tween.setTarget(root);
    tween.start();
    scene.getSystem<ActionSystem>()->update(0.5);
    scene.getSystem<ActionSystem>()->update(0.5);
    EXPECT_LT(group(root).alpha, 1.0f) << "AUTO on a group entity writes the group";
    EXPECT_FLOAT_EQ(uic(root).color.w, 1.0f);

    AlphaTween colorTween(&scene);
    colorTween.setAction(1.0f, 0.5f, 0.0f);
    colorTween.setTarget(root);
    colorTween.setAlphaTarget(AlphaTweenTarget::COLOR);
    colorTween.start();
    scene.getSystem<ActionSystem>()->update(0.1);
    scene.getSystem<ActionSystem>()->update(0.1);
    EXPECT_FLOAT_EQ(uic(root).color.w, 0.5f);
}

TEST_F(CanvasGroupTest, FadeOrderAndReplacement){
    Entity root = container(NULL_ENTITY);
    addGroup(root, 1.0f);
    CanvasGroup cg(&scene, root);
    std::vector<std::string> order;
    CanvasGroupFadeOptions options;
    options.deactivate = true;
    options.onComplete = [&](){
        order.push_back("complete alpha=" + std::to_string(group(root).alpha) + " active=" + std::to_string(scene.isEntityActive(root)));
    };
    // a first fade replaced by a second: no fighting, the first never completes
    bool firstCompleted = false;
    CanvasGroupFadeOptions first;
    first.onComplete = [&](){ firstCompleted = true; };
    cg.fadeTo(0.5f, 1.0f, first);
    cg.fadeOut(0.2f, options);
    EXPECT_TRUE(cg.isFading());
    for (int i = 0; i < 4; i++){
        scene.getSystem<ActionSystem>()->update(0.1);
        ui()->update(0);
    }
    EXPECT_FALSE(firstCompleted);
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], "complete alpha=0.000000 active=1") << "final value, then onComplete, then deactivate";
    EXPECT_FALSE(scene.isEntityActive(root));
    EXPECT_FALSE(cg.isFading());
}

TEST_F(CanvasGroupTest, UnscaledFadeCompletesAtTimeScaleZero){
    Entity root = container(NULL_ENTITY);
    addGroup(root, 1.0f);
    CanvasGroup cg(&scene, root);
    const float previousScale = Engine::getTimeScale();
    const double previousFixed = Engine::getFixedDeltatime();
    Engine::setTimeScale(0.0f);
    Engine::setFixedDeltatime(0.1);
    CanvasGroupFadeOptions options;
    options.unscaled = true;
    bool completed = false;
    options.onComplete = [&](){ completed = true; };
    cg.fadeOut(0.25f, options);
    CanvasGroup scaled(&scene, container(NULL_ENTITY));
    scaled.fadeOut(0.25f);
    for (int i = 0; i < 5; i++){
        scene.getSystem<ActionSystem>()->update(0.0);
        ui()->update(0);
    }
    Engine::setTimeScale(previousScale);
    Engine::setFixedDeltatime(previousFixed);
    EXPECT_TRUE(completed);
    EXPECT_FLOAT_EQ(group(root).alpha, 0.0f);
    EXPECT_FLOAT_EQ(scaled.getAlpha(), 1.0f) << "a scaled fade waits at time scale 0";
}

TEST_F(CanvasGroupTest, StopFadeJumpsToTheEnd){
    Entity root = container(NULL_ENTITY);
    addGroup(root, 1.0f);
    CanvasGroup cg(&scene, root);
    bool completed = false;
    CanvasGroupFadeOptions options;
    options.onComplete = [&](){ completed = true; };
    cg.fadeTo(0.2f, 10.0f, options);
    scene.getSystem<ActionSystem>()->update(0.1);
    cg.stopFade(true);
    EXPECT_FLOAT_EQ(cg.getAlpha(), 0.2f);
    EXPECT_TRUE(completed);
    EXPECT_FALSE(cg.isFading());

    // stopped without jumping: no completion, the alpha stays where it was
    completed = false;
    cg.fadeTo(1.0f, 10.0f, options);
    scene.getSystem<ActionSystem>()->update(0.1);
    scene.getSystem<ActionSystem>()->update(1.0);
    const float midway = cg.getAlpha();
    cg.stopFade();
    EXPECT_FALSE(completed);
    EXPECT_FLOAT_EQ(cg.getAlpha(), midway);
}

TEST_F(CanvasGroupTest, FadeGoesWithTheGroup){
    Entity root = container(NULL_ENTITY);
    addGroup(root, 1.0f);
    CanvasGroup cg(&scene, root);
    bool completed = false;
    CanvasGroupFadeOptions options;
    options.onComplete = [&](){ completed = true; };
    AlphaTween tween = cg.fadeOut(1.0f, options);
    const Entity tweenEntity = tween.getEntity();
    scene.destroyEntity(root);
    EXPECT_FALSE(scene.isEntityCreated(tweenEntity)) << "the tween is destroyed with its group";
    scene.getSystem<ActionSystem>()->update(2.0);
    ui()->update(0);
    EXPECT_FALSE(completed);
}

// ---- 12. Lua

TEST_F(CanvasGroupTest, LuaApi){
    Entity panel = container(NULL_ENTITY);
    Entity element = image(panel, 0, 0, 10, 10);
    lua_pushinteger(L, (lua_Integer)panel);
    lua_setglobal(L, "panelEntity");
    lua_pushinteger(L, (lua_Integer)element);
    lua_setglobal(L, "elementEntity");

    EXPECT_EQ(luaNumber("GameObject(scene, panelEntity):getCanvasGroup()"), -1.0) << "nil without one";
    EXPECT_EQ(luaNumber("GameObject(scene, panelEntity):hasCanvasGroup()"), 0.0);
    ASSERT_EQ(lua("local g = CanvasGroup(scene, panelEntity); g.alpha = 1.5"), "");
    EXPECT_NE(scene.findComponent<CanvasGroupComponent>(panel), nullptr) << "the constructor adds the component";
    EXPECT_DOUBLE_EQ(luaNumber("CanvasGroup(scene, panelEntity).alpha"), 1.0) << "clamped";
    ASSERT_EQ(lua("CanvasGroup(scene, panelEntity).alpha = 0/0"), "");
    EXPECT_DOUBLE_EQ(luaNumber("CanvasGroup(scene, panelEntity).alpha"), 1.0) << "NaN rejected";
    ASSERT_EQ(lua("local g = GameObject(scene, panelEntity):getCanvasGroup(); g.alpha = 0.5; g.interactable = false; "
                  "g.renderMode = CanvasGroupRenderMode.FLATTEN"), "");
    EXPECT_EQ(group(panel).renderMode, CanvasGroupRenderMode::FLATTEN);
    ui()->syncCanvasGroups();
    EXPECT_DOUBLE_EQ(luaNumber("RectTransform(scene, elementEntity):getInheritedAlpha()"), 0.5);
    EXPECT_DOUBLE_EQ(luaNumber("RectTransform(scene, elementEntity):getGroupsAllowInteraction()"), 0.0);
    EXPECT_DOUBLE_EQ(luaNumber("RectTransform(scene, elementEntity):getGroupsBlockRaycasts()"), 1.0);
    EXPECT_DOUBLE_EQ(luaNumber("RectTransform(scene, elementEntity):isInteractable()"), 0.0);
    // the getters match the synced state
    EXPECT_FLOAT_EQ(uic(element).groupAlpha, 0.5f);

    // a fade with options, from Lua
    ASSERT_EQ(lua("done = 0; local t = CanvasGroup(scene, panelEntity):fadeTo(0, 0, {ease = EaseType.QUAD_OUT, onComplete = function() done = done + 1 end}); "
                  "assert(t.alphaTarget == AlphaTweenTarget.CANVAS_GROUP)"), "");
    scene.getSystem<ActionSystem>()->update(0.1);
    ui()->update(0);
    EXPECT_DOUBLE_EQ(luaNumber("done"), 1.0);
    EXPECT_FLOAT_EQ(group(panel).alpha, 0.0f);
    EXPECT_NE(lua("CanvasGroup(scene, panelEntity):fadeTo(1, 1, 5)"), "") << "options must be a table";

    // CanvasGroup(scene): a new stretched container with a group
    ASSERT_EQ(lua("made = CanvasGroup(scene); made:setEntityOwned(false); madeEntity = made:getEntity()"), "");
    const Entity made = (Entity)luaNumber("madeEntity");
    ASSERT_TRUE(scene.isEntityCreated(made));
    EXPECT_NE(scene.findComponent<CanvasGroupComponent>(made), nullptr);
    EXPECT_EQ(scene.getComponent<RectTransformComponent>(made).anchorPreset, AnchorPreset::FULL_LAYOUT);
    lua("made = nil; collectgarbage()");
}

// ---- the clip property rows (anim-properties registry contract)

TEST_F(CanvasGroupTest, TrackRowsMatchTheCatalogAndClamp){
    Entity root = container(NULL_ENTITY);
    addGroup(root, 0.5f);
    size_t count = 0;
    const anim::CanvasGroupTrack* tracks = anim::canvasGroupTracks(count);
    ASSERT_EQ(count, 5u);
    CanvasGroupComponent& component = group(root);
    for (size_t i = 0; i < count; i++){
        const std::string name = tracks[i].property;
        PropertyData data = Catalog::getProperties(ComponentType::CanvasGroupComponent, &component)[name];
        ASSERT_NE(data.ref, nullptr) << "every row exists in the Catalog: " << name;
        EXPECT_EQ(data.type, tracks[i].isBool ? PropertyType::Bool : PropertyType::Float) << name;
        float value = -1.0f;
        EXPECT_TRUE(tracks[i].read(&scene, root, &value)) << name;
    }
    const float high = 1.7f;
    EXPECT_TRUE(anim::writeCanvasGroupAlpha(&scene, root, &high));
    EXPECT_FLOAT_EQ(component.alpha, 1.0f) << "clamped on write";
    const float nan = NAN;
    EXPECT_FALSE(anim::writeCanvasGroupAlpha(&scene, root, &nan));
    const float off = 0.0f;
    EXPECT_TRUE(anim::writeCanvasGroupBlocksRaycasts(&scene, root, &off));
    EXPECT_FALSE(component.blocksRaycasts);
    // a plain field write the next sync sees (decision 10)
    Entity element = image(root, 0, 0, 10, 10);
    ui()->syncCanvasGroups();
    EXPECT_FALSE(uic(element).groupBlocksRaycasts);
    float read = 0.0f;
    EXPECT_FALSE(anim::readCanvasGroupAlpha(&scene, element, &read)) << "no group on the element";
}

// ---- 13. performance (Release only)

#ifdef NDEBUG
TEST_F(CanvasGroupTest, PerformanceBudgets){
    // structural: 5000 transforms, one pass <= 0.4 ms (host, Release)
    Entity root = container(NULL_ENTITY);
    addGroup(root, 0.5f);
    Entity parent = root;
    for (int i = 0; i < 4999; i++){
        Entity e = image(i % 10 == 0 ? root : parent, 0, 0, 10, 10);
        if (i % 10 == 0) parent = e;
    }
    ui()->syncCanvasGroups();

    auto time = [](auto&& fn){
        const auto start = std::chrono::steady_clock::now();
        fn();
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    };
    double structural = 1e9;
    for (int i = 0; i < 5; i++){
        scene.hierarchyChanged();
        structural = std::min(structural, time([&](){ ui()->syncCanvasGroups(); }));
    }
    EXPECT_LE(structural, 400.0) << "full pass over 5000 transforms (us)";

    // clean: 50 groups, one comparison each <= 2 us
    Scene small;
    UISystem* smallUi = small.getSystem<UISystem>().get();
    for (int i = 0; i < 50; i++){
        Entity e = small.createEntity();
        small.addComponent<Transform>(e, Transform());
        small.addComponent<CanvasGroupComponent>(e, CanvasGroupComponent());
    }
    smallUi->syncCanvasGroups();
    double clean = 1e9;
    for (int i = 0; i < 20; i++){
        clean = std::min(clean, time([&](){ smallUi->syncCanvasGroups(); }));
    }
    EXPECT_LE(clean, 2.0) << "clean check of 50 groups (us)";

    // an alpha fade of a group with 100 descendants <= 15 us a frame
    Scene fade;
    UISystem* fadeUi = fade.getSystem<UISystem>().get();
    Entity fadeRoot = fade.createEntity();
    fade.addComponent<Transform>(fadeRoot, Transform());
    fade.addComponent<CanvasGroupComponent>(fadeRoot, CanvasGroupComponent());
    for (int i = 0; i < 100; i++){
        Entity e = fade.createEntity();
        fade.addComponent<Transform>(e, Transform());
        fade.addComponent<RectTransformComponent>(e, RectTransformComponent());
        fade.addComponent<UIComponent>(e, UIComponent());
        fade.addEntityChild(fadeRoot, e, false);
    }
    fadeUi->syncCanvasGroups();
    double fading = 1e9;
    for (int i = 0; i < 20; i++){
        fade.getComponent<CanvasGroupComponent>(fadeRoot).alpha = 0.01f * (float)(i + 1);
        fading = std::min(fading, time([&](){ fadeUi->syncCanvasGroups(); }));
    }
    EXPECT_LE(fading, 15.0) << "subtree pass over 101 entities (us)";
}
#endif
