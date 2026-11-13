// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's Transform hierarchy on GameObject, C++ and Lua (scripting catch-up, Doriax 04501b15b
// field gaps): parent, setParent(p, worldPositionStays), childCount, getChild, getChildren,
// findChild("a/b"), and the parents a GameObject refuses.

#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"
#include "object/GameObject.h"
#include "subsystem/RenderSystem.h"
#include "script/LuaBinding.h"
#include "../ecs/EcsTestUtil.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;
using jasperengine::test::makeTransformEntity;

namespace {

    std::string runLua(lua_State* L, const std::string& code){
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
    EXPECT_EQ(runLua(L, R"(
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
    EXPECT_EQ(runLua(L, R"(
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

// S95 REQ-11: Camera(scene) aims with its Transform rotation, as the editor's GameObject > Camera;
// setTarget and assigning target turn target mode on, useTarget switches it either way
TEST(LuaHierarchy, CameraConstructorUsesRotation){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);

    Scene scene;
    luabridge::setGlobal(L, &scene, "cameraTestScene");
    ASSERT_EQ(runLua(L, R"(
        cameraTest = Camera(cameraTestScene)
        assert(cameraTest.useTarget == false, "a new camera aims with its rotation")
        assert(cameraTest:isUsingTarget() == false)
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

    EXPECT_EQ(runLua(L, R"(
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
