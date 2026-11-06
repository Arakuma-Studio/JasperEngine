// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// An omitted boolean from Lua keeps the meaning the C++ API gives it (scripting catch-up, Doriax
// 04501b15b): LuaBridge reads a missing bool as false, so MeshSystem:loadGLTF(e, path) did not
// place the entity at the file's root transform (changeRootTransform defaults to true), and
// Rigidbody2D:applyForceToCenter(f) left a sleeping body asleep (Unity's AddForce wakes it).

#include <gtest/gtest.h>

#include "Physics2DTestWorld.h"
#include "support/TwoPartGltf.h"

#include "Scene.h"
#include "component/Transform.h"
#include "object/Model.h"
#include "script/LuaBinding.h"
#include "subsystem/MeshSystem.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <filesystem>
#include <string>

using namespace jasperengine;
using namespace jasperengine::physics2dtest;
namespace fs = std::filesystem;

namespace {

    std::string runLua(lua_State* L, const std::string& code){
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

}

TEST(LuaDefaults, LoadGltfKeepsChangeRootTransform) {
    World2D world;  // the test System and the Lua state
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    const fs::path path = editor::test::writeTriangleGltf(fs::temp_directory_path() / "jasperengine-editor-tests-lua-defaults", "rooted",
                                                          {{{"name", "Mesh"}, {"translation", {0, 0, 3}}, {"mesh", 0}}}, {0});

    Scene scene;
    Model fromCpp(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(fromCpp.getEntity(), path.string()));
    const Vector3 expected = scene.getComponent<Transform>(fromCpp.getEntity()).position;
    ASSERT_EQ(expected, Vector3(0, 0, 3));

    Model fromLua(&scene);
    luabridge::setGlobal(L, &scene, "defaultsScene");
    lua_pushinteger(L, fromLua.getEntity()); lua_setglobal(L, "MODEL");
    luabridge::setGlobal(L, path.string(), "MODEL_PATH");
    EXPECT_EQ(runLua(L, "assert(defaultsScene:getMeshSystem():loadGLTF(MODEL, MODEL_PATH))"), "");
    EXPECT_EQ(scene.getComponent<Transform>(fromLua.getEntity()).position, expected);
}

TEST(LuaDefaults, Rigidbody2DForceWakesByDefault) {
    World2D world(Vector2(0.0f, 0.0f));
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    const Entity pushed = world.body(Vector3(0, 0, 0), BodyType::DYNAMIC);
    const Entity kept = world.body(Vector3(500, 0, 0), BodyType::DYNAMIC);
    world.rb(pushed).createCircleCollider(Vector2(0, 0), 10);
    world.rb(kept).createCircleCollider(Vector2(0, 0), 10);
    world.load();
    world.step();
    world.rb(pushed).setAwake(false);
    world.rb(kept).setAwake(false);
    ASSERT_FALSE(world.rb(pushed).isAwake());

    luabridge::setGlobal(L, world.rb(pushed), "pushedBody");
    luabridge::setGlobal(L, world.rb(kept), "keptBody");
    EXPECT_EQ(runLua(L, R"(
        pushedBody:applyForceToCenter(Vector2(0, 1000))
        keptBody:applyForceToCenter(Vector2(0, 1000), false)
    )"), "");
    EXPECT_TRUE(world.rb(pushed).isAwake());
    EXPECT_FALSE(world.rb(kept).isAwake());
    const Vector2 before = world.rb(pushed).getPosition();
    world.step(5);
    EXPECT_GT(world.rb(pushed).getPosition().y, before.y);
}
