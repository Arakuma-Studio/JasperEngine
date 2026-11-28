// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's Transform hierarchy on GameObject, C++ and Lua (scripting catch-up, Doriax 04501b15b
// field gaps): parent, setParent(p, worldPositionStays), childCount, getChild, getChildren,
// findChild("a/b"), and the parents a GameObject refuses. S95: the transform members of an
// entity without a Transform (REQ-3), lookups among children of the same name (S16), and world
// getters that are current after a move (REQ-6).

#include "Scene.h"
#include "action/Action.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"
#include "object/GameObject.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/TransformPropagation.h"
#include "Engine.h"
#include "script/LuaBinding.h"
#include "../ecs/EcsTestUtil.h"
#include "LogCapture.h"
#include "TestSystem.h"
#include "support/TestTempDir.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace LuaHierarchyTest_cpp {

using namespace jasperengine;
using jasperengine::test::makeTransformEntity;

namespace {

    std::string runHierarchyLua(lua_State* L, const std::string& code){
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

    Entity named(Scene& scene, const std::string& name, Entity parent = NULL_ENTITY){
        const Entity entity = makeTransformEntity(scene, parent);
        scene.setEntityName(entity, name);
        return entity;
    }

    // the world matrices as a frame computes them
    void updateWorld(Scene& scene){
        auto transforms = scene.getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++){
            const Entity entity = transforms->getEntity(i);
            scene.getSystem<RenderSystem>()->updateTransform(scene.getComponent<Transform>(entity), entity);
        }
    }

}

TEST(LuaHierarchy, ParentChildrenFind){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);

    Scene scene;
    const Entity root = named(scene, "Root");
    const Entity arm = named(scene, "Arm", root);
    const Entity leg = named(scene, "Leg", root);
    const Entity hand = named(scene, "Hand", arm);

    // C++
    GameObject rootObject(&scene, root);
    EXPECT_FALSE(rootObject.getParent().has_value());
    EXPECT_EQ(GameObject(&scene, hand).getParent()->getEntity(), arm);
    EXPECT_EQ(rootObject.getChildCount(), 2u);
    EXPECT_EQ(rootObject.getChild(0)->getEntity(), arm);
    EXPECT_EQ(rootObject.getChild(1)->getEntity(), leg);
    EXPECT_FALSE(rootObject.getChild(2).has_value());
    ASSERT_EQ(rootObject.getChildren().size(), 2u);
    EXPECT_EQ(rootObject.findChild("Arm/Hand")->getEntity(), hand);
    EXPECT_FALSE(rootObject.findChild("Hand").has_value());  // children only, not any descendant
    EXPECT_FALSE(rootObject.findChild("Arm//Hand").has_value());

    // Lua
    luabridge::setGlobal(L, rootObject, "hierRoot");
    lua_pushinteger(L, arm); lua_setglobal(L, "ARM");
    lua_pushinteger(L, leg); lua_setglobal(L, "LEG");
    lua_pushinteger(L, hand); lua_setglobal(L, "HAND");
    EXPECT_EQ(runHierarchyLua(L, R"(
        local root = hierRoot
        assert(root.parent == nil, "a root has no parent")
        assert(root.childCount == 2, "two children, got " .. tostring(root.childCount))
        assert(root:getChild(0).entity == ARM and root:getChild(1).entity == LEG, "Hierarchy order")
        assert(root:getChild(5) == nil, "out of range")
        local children = root:getChildren()
        assert(#children == 2 and children[2].entity == LEG, "getChildren")
        local found = root:findChild("Arm/Hand")
        assert(found and found.entity == HAND, "findChild path")
        assert(found.parent.entity == ARM, "parent")
        assert(root:findChild("Nope") == nil, "missing child")
    )"), "");
}

TEST(LuaHierarchy, SetParentKeepsWorldOrLocal){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);

    Scene scene;
    const Entity holder = named(scene, "Holder");
    const Entity item = named(scene, "Item");
    scene.getComponent<Transform>(holder).position = Vector3(10, 0, 0);
    scene.getComponent<Transform>(item).position = Vector3(1, 2, 3);
    updateWorld(scene);

    luabridge::setGlobal(L, GameObject(&scene, holder), "holder");
    luabridge::setGlobal(L, GameObject(&scene, item), "item");
    EXPECT_EQ(runHierarchyLua(L, R"(
        -- moved this frame: the world matrix catches up only at the transform update
        holder.position = Vector3(20, 0, 0)
        item:setParent(holder)
        assert(item.parent.entity == holder.entity, "reparented")
        assert(item.position == Vector3(-19, 2, 3), "world kept against where the holder is now: local " .. tostring(item.position))
        item.parent = nil
        assert(item.parent == nil, "back to the root")
        assert(item.position == Vector3(1, 2, 3), "world kept at the root: " .. tostring(item.position))
        item:setParent(holder, false)
        assert(item.position == Vector3(1, 2, 3), "local kept")
    )"), "");
}

TEST(LuaHierarchy, SetParentRefusesCycleAndOtherScene){
    Scene scene;
    Scene other;
    const Entity parent = named(scene, "Parent");
    const Entity child = named(scene, "Child", parent);
    const Entity stranger = named(other, "Stranger");

    GameObject parentObject(&scene, parent);
    const GameObject childObject(&scene, child);
    parentObject.setParent(&childObject);
    EXPECT_FALSE(parentObject.getParent().has_value());  // its own child: refused
    parentObject.setParent(&parentObject);
    EXPECT_FALSE(parentObject.getParent().has_value());  // itself: refused

    const GameObject strangerObject(&other, stranger);
    GameObject(&scene, child).setParent(&strangerObject);
    EXPECT_EQ(scene.getComponent<Transform>(child).parent, parent);  // another scene: refused
}

// S95 REQ-3 (S17): an action has no Transform. The getters give nil and log nothing; the setters
// raise an error naming the GameObject
TEST(LuaHierarchy, NoTransformGivesNil){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);

    Scene scene;
    Action action(&scene);
    scene.setEntityName(action.getEntity(), "Fade");
    ASSERT_EQ(scene.findComponent<Transform>(action.getEntity()), nullptr);

    luabridge::setGlobal(L, GameObject(&scene, action.getEntity()), "noTransform");
    EXPECT_EQ(runHierarchyLua(L, R"lua(
        local go = noTransform
        for _, name in ipairs({"getWorldPosition", "getWorldRotation", "getWorldScale"}) do
            assert(go[name](go) == nil, name .. " is nil")
        end
        for _, name in ipairs({"position", "rotation", "scale", "parent", "localMatrix", "modelMatrix", "normalMatrix"}) do
            assert(go[name] == nil, name .. " is nil")
        end
        local setters = {
            function() go.position = Vector3(1, 2, 3) end,
            function() go:setPosition(1, 2) end,
            function() go:setPosition(1, 2, 3) end,
            function() go.rotation = Quaternion() end,
            function() go:setRotation(0, 90, 0) end,
            function() go.scale = Vector3(2, 2, 2) end,
            function() go:setScale(2) end,
            function() go.localMatrix = Matrix4() end,
            function() go.parent = nil end,
            function() go:setParent(nil) end,
        }
        for i, setter in ipairs(setters) do
            local ok, message = pcall(setter)
            assert(not ok, "setter " .. i .. " raises")
            assert(tostring(message):find("GameObject 'Fade' has no Transform", 1, true),
                "setter " .. i .. " names it: " .. tostring(message))
        end
    )lua"), "");
    EXPECT_NO_ENGINE_ERRORS();
}

// with a Transform the same members read and write it (the optional getters keep the values)
TEST(LuaHierarchy, TransformMembersReadAndWrite){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);

    Scene scene;
    const Entity mover = named(scene, "Mover");
    luabridge::setGlobal(L, GameObject(&scene, mover), "mover");
    EXPECT_EQ(runHierarchyLua(L, R"lua(
        mover:setPosition(1, 2)
        assert(mover.position == Vector3(1, 2, 0), "(x, y): z is 0")
        mover:setPosition(1, 2, 3)
        assert(mover.position == Vector3(1, 2, 3), "(x, y, z)")
        mover.scale = Vector3(2, 3, 4)
        assert(mover.scale == Vector3(2, 3, 4), "scale")
        mover:setScale(5)
        assert(mover.scale == Vector3(5, 5, 5), "uniform scale")
        mover:setRotation(0, 0, 90)
        assert(mover.rotation == Quaternion(0, 0, 90), "rotation")
        assert(mover:getWorldPosition() ~= nil and mover.modelMatrix ~= nil, "world values")
    )lua"), "");
    EXPECT_NO_ENGINE_ERRORS();
}

// S95 AC-5 (S16): two children named alike under different parents are told apart by the parent
TEST(LuaHierarchy, FindEntityUnderParent){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);

    Scene scene;
    const Entity a = named(scene, "A");
    const Entity b = named(scene, "B");
    const Entity blueA = named(scene, "Blue", a);
    const Entity blueB = named(scene, "Blue", b);
    EXPECT_EQ(scene.findEntity("Blue", b), blueB);
    EXPECT_EQ(scene.findEntity("Blue", a), blueA);

    luabridge::setGlobal(L, &scene, "hierScene");
    luabridge::setGlobal(L, GameObject(&scene, b), "hierB");
    lua_pushinteger(L, a); lua_setglobal(L, "A_ID");
    lua_pushinteger(L, b); lua_setglobal(L, "B_ID");
    lua_pushinteger(L, blueA); lua_setglobal(L, "BLUE_A");
    lua_pushinteger(L, blueB); lua_setglobal(L, "BLUE_B");
    EXPECT_EQ(runHierarchyLua(L, R"lua(
        assert(hierScene:findEntity("Blue", B_ID) == BLUE_B, "B's Blue")
        assert(hierScene:findEntity("Blue", A_ID) == BLUE_A, "A's Blue")
        local found = hierB:findChild("Blue")
        assert(found and found.entity == BLUE_B, "findChild agrees")
        assert(found.parent.entity == B_ID, "its parent is B")
    )lua"), "");
}

namespace {

    // the world values a frame's transform pass computes, its flags cleared after (as
    // RenderSystem::update leaves them)
    void propagateAndClear(Scene& scene){
        auto transforms = scene.getComponentArray<Transform>();
        propagateTransforms(scene, [&scene](Transform& transform, Entity entity){
            computeWorldTransform(scene, transform, entity);
        });
        for (size_t i = 0; i < transforms->size(); i++){
            transforms->getComponentFromIndex(i).needUpdate = false;
        }
    }

    bool sameWorldBits(const void* a, const void* b, size_t size){
        return std::memcmp(a, b, size) == 0;
    }

}

// S95 AC-10 (REQ-6, S26): a parent moved in onUpdate; its child's getWorldPosition, Rotation,
// Scale and modelMatrix read in the same call are the values the transform pass gives after it,
// bit for bit, and reading them changes nothing (no flag, no stored value). Red: last frame's
TEST(LuaHierarchy, WorldPoseFreshAfterParentMove){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);

    Scene scene;
    const Entity root = named(scene, "Root");
    const Entity parent = named(scene, "Parent", root);
    const Entity child = named(scene, "Child", parent);
    Transform& rootTransform = scene.getComponent<Transform>(root);
    rootTransform.position = Vector3(-4.0f, 1.5f, 2.0f);
    rootTransform.rotation = Quaternion(20.0f, Vector3(0.0f, 1.0f, 0.0f));
    Transform& childTransform = scene.getComponent<Transform>(child);
    childTransform.position = Vector3(1.0f, 2.0f, -3.0f);
    childTransform.rotation = Quaternion(35.0f, Vector3(0.3f, 1.0f, 0.2f).normalize());
    childTransform.scale = Vector3(1.5f, 0.5f, 2.0f);
    propagateAndClear(scene);

    luabridge::setGlobal(L, GameObject(&scene, parent), "poseParent");
    luabridge::setGlobal(L, GameObject(&scene, child), "poseChild");
    ASSERT_EQ(runHierarchyLua(L, R"lua(
        Engine.onUpdate:add("worldPoseFresh", function()
            poseParent.position = Vector3(7, -2, 5)
            poseParent:setRotation(10, 70, -30)
            poseParent.scale = Vector3(2, 3, 0.5)
            poseSeen = {
                position = poseChild:getWorldPosition(),
                rotation = poseChild:getWorldRotation(),
                scale = poseChild:getWorldScale(),
                model = poseChild.modelMatrix,
            }
        end)
    )lua"), "");
    const Transform before = scene.getComponent<Transform>(child);
    Engine::onUpdate.call();
    Engine::onUpdate.remove("worldPoseFresh");

    // nothing written: the child is as it was, its parent still waits for the pass
    const Transform& after = scene.getComponent<Transform>(child);
    EXPECT_FALSE(after.needUpdate);
    EXPECT_TRUE(scene.getComponent<Transform>(parent).needUpdate);
    EXPECT_TRUE(sameWorldBits(&after.worldPosition, &before.worldPosition, sizeof(Vector3)));
    EXPECT_TRUE(sameWorldBits(&after.modelMatrix, &before.modelMatrix, sizeof(Matrix4)));

    ASSERT_EQ(runHierarchyLua(L, "poseSeenPosition = poseSeen.position; poseSeenRotation = poseSeen.rotation; "
                        "poseSeenScale = poseSeen.scale; poseSeenModel = poseSeen.model; poseSeen = nil"), "");
    const Vector3 seenPosition = luabridge::getGlobal(L, "poseSeenPosition").cast<Vector3>().value();
    const Quaternion seenRotation = luabridge::getGlobal(L, "poseSeenRotation").cast<Quaternion>().value();
    const Vector3 seenScale = luabridge::getGlobal(L, "poseSeenScale").cast<Vector3>().value();
    const Matrix4 seenModel = luabridge::getGlobal(L, "poseSeenModel").cast<Matrix4>().value();

    propagateAndClear(scene);
    const Transform& world = scene.getComponent<Transform>(child);
    EXPECT_TRUE(sameWorldBits(&seenPosition, &world.worldPosition, sizeof(Vector3)))
        << "position " << seenPosition.x << " " << seenPosition.y << " " << seenPosition.z << " after the pass "
        << world.worldPosition.x << " " << world.worldPosition.y << " " << world.worldPosition.z;
    EXPECT_TRUE(sameWorldBits(&seenRotation, &world.worldRotation, sizeof(Quaternion))) << "rotation";
    EXPECT_TRUE(sameWorldBits(&seenScale, &world.worldScale, sizeof(Vector3))) << "scale";
    EXPECT_TRUE(sameWorldBits(&seenModel, &world.modelMatrix, sizeof(Matrix4))) << "modelMatrix";
    // the values did move: the check compares this frame's pose, not two stale ones
    EXPECT_FALSE(sameWorldBits(&before.worldPosition, &world.worldPosition, sizeof(Vector3)));

    ASSERT_EQ(runHierarchyLua(L, "poseParent = nil; poseChild = nil; poseSeenPosition = nil; poseSeenRotation = nil; "
                        "poseSeenScale = nil; poseSeenModel = nil"), "");
    EXPECT_NO_ENGINE_ERRORS();
}

// S95 REQ-11: Camera(scene) keeps target mode, as before (old scripts that look at the origin keep
// their view); useTarget = false aims it with its Transform rotation, as the editor's
// GameObject > Camera; setTarget and assigning target turn target mode on again
TEST(LuaHierarchy, CameraUseTargetSwitchesToRotation){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);

    Scene scene;
    luabridge::setGlobal(L, &scene, "cameraTestScene");
    ASSERT_EQ(runHierarchyLua(L, R"(
        cameraTest = Camera(cameraTestScene)
        assert(cameraTest.useTarget == true, "a new camera keeps target mode, as before")
        assert(cameraTest:isUsingTarget() == true)
        cameraTest.useTarget = false
        assert(cameraTest:isUsingTarget() == false, "useTarget = false aims it with its rotation")
        cameraTest.position = Vector3(0, 0, 5)
        cameraTest.rotation = Quaternion(90, Vector3(0, 1, 0))
        cameraTestEntity = cameraTest.entity
    )"), "");

    lua_getglobal(L, "cameraTestEntity");
    const Entity entity = static_cast<Entity>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    ASSERT_TRUE(scene.findComponent<CameraComponent>(entity));
    Camera camera(&scene, entity);
    camera.updateTransform();
    camera.updateCamera();
    // the point one unit ahead along rotation * -Z is straight ahead in view space (a look-at of
    // the origin from (0, 0, 5) would put it to the side)
    const Transform& transform = scene.getComponent<Transform>(camera.getEntity());
    const Vector3 ahead = camera.getViewMatrix() * (transform.worldPosition + transform.worldRotation * Vector3(0, 0, -1));
    EXPECT_NEAR(ahead.x, 0.0f, 1e-4f);
    EXPECT_NEAR(ahead.y, 0.0f, 1e-4f);
    EXPECT_NEAR(ahead.z, -1.0f, 1e-4f);

    EXPECT_EQ(runHierarchyLua(L, R"(
        cameraTest:setTarget(0, 0, 0)
        assert(cameraTest.useTarget == true, "setTarget turns target mode on")
        cameraTest.useTarget = false
        assert(cameraTest.useTarget == false)
        cameraTest.target = Vector3(1, 2, 3)
        assert(cameraTest.useTarget == true, "assigning target turns it on")
        cameraTest = nil
        cameraTestEntity = nil
        cameraTestScene = nil
    )"), "");
    camera.updateCamera();
    EXPECT_EQ(camera.getViewMatrix(), Matrix4::lookAtMatrix(transform.worldPosition, Vector3(1, 2, 3), Vector3(0, 1, 0)));
}

// main.lua runs before the game's init sets a scene, so Engine.getScene() is nil there: an object
// made with it is a Lua error naming the fix, not an entity written through a null Scene (a crash
// here; on the web a page that hung before its first frame)
TEST(LuaHierarchy, ObjectsMadeWithoutASceneFail){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    for (const char* code : {"GameObject(nil)", "GameObject(nil, 1)", "Sprite(nil)", "Camera(nil)", "AudioSource(nil, true)",
                             "PositionTween(nil)", "CullingGroup(nil)"}) {
        const std::string error = runHierarchyLua(L, std::string("local object = ") + code);
        EXPECT_NE(error.find("the scene is nil"), std::string::npos) << code << ": " << error;
    }

    // a scene still makes them, over a new entity or one it has
    Scene scene;
    const Entity existing = named(scene, "Existing");
    luabridge::setGlobal(L, &scene, "testScene");
    EXPECT_EQ(runHierarchyLua(L, "local made = GameObject(testScene); assert(made.entity ~= nil)\n"
                        "local over = GameObject(testScene, " + std::to_string(existing) + "); assert(over.entity == " +
                        std::to_string(existing) + ")\n"
                        "local sound = AudioSource(testScene, false); assert(sound.entity ~= nil)"), "");
    lua_pushnil(L);
    lua_setglobal(L, "testScene");
}

// main.lua's error (here the object of the test above, made at init) is logged and the game runs
// on in the same state: closing it left the binding's state pointer dangling, which every later
// script and frame then used (a crash on desktop, a hung web page)
TEST(LuaHierarchy, AFailingMainLuaKeepsTheState){
    test::TestTempDir dir("lua-main-error");
    test::TestTempDir::write(dir / "main.lua", "Before = 1\nlocal menu = UIDocument(Engine.getScene())\nAfter = 2\n");
    const std::string luaDir = dir.path().string();
    test::TestSystem::get().setPathProvider({nullptr, [luaDir]() { return luaDir; }, nullptr});
    LuaBinding::recreateLuaState();
    LuaBinding::init();
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    EXPECT_EQ(runHierarchyLua(L, "assert(Before == 1, 'what main.lua ran before its error stays')\n"
                        "assert(After == nil, 'nothing after it ran')\n"
                        "local scene = GameObject ~= nil\nassert(scene, 'the bindings are still there')"), "");
    test::TestSystem::get().clearPathProvider();
    LuaBinding::recreateLuaState();
}

}  // namespace LuaHierarchyTest_cpp
