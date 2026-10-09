// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Handles that own their entity (EntityHandle(Scene*), every GameObject made from Lua) and
// the lifetime of their scene. Not HTML UI: the engine-level coverage of a crash found while
// testing it. A Lua handle goes when the garbage collector takes it, which can be after its
// scene was deleted (the editor's Play copies at play stop, any scene before lua_close); it
// must then leave the scene alone. While the scene lives, it still destroys its entity.

#include "harness/GpuEnvironment.h"
#include "harness/TestSystem.h"

#include "Scene.h"
#include "object/GameObject.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "object/Sprite.h"
#include "object/ui/Button.h"
#include "object/ui/Image.h"
#include "object/ui/Text.h"
#include "script/LuaBinding.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class EntityHandleLifetimeTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            L = LuaBinding::getLuaState();
            ASSERT_NE(L, nullptr);
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            lua("gcScene = nil; held = nil; collectgarbage(); collectgarbage()");
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << "errors logged";
        }

        // runs a chunk; "" on success, else the error message
        std::string lua(const std::string& code){
            if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
                std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
                lua_pop(L, 1);
                return error;
            }
            return std::string();
        }

        bool luaBool(const std::string& expression){
            EXPECT_EQ(lua("result = (" + expression + ")"), "");
            lua_getglobal(L, "result");
            bool value = lua_toboolean(L, -1);
            lua_pop(L, 1);
            lua("result = nil");
            return value;
        }

        void setSceneGlobal(Scene* scene){
            ASSERT_TRUE(luabridge::push(L, scene));
            lua_setglobal(L, "gcScene");
        }

        // a scene made right after another was deleted (usually at its address), with
        // entities of the same ids the deleted one had
        std::unique_ptr<Scene> sceneWithEntities(size_t count, std::vector<Entity>& entities){
            std::unique_ptr<Scene> scene(new Scene());
            for (size_t i = 0; i < count; i++){
                entities.push_back(scene->createEntity());
            }
            return scene;
        }

        lua_State* L = nullptr;
    };

}

TEST_F(EntityHandleLifetimeTest, CppHandlesOutliveTheirScene){
    Scene* scene = new Scene();
    std::vector<std::unique_ptr<EntityHandle>> held;
    held.emplace_back(new GameObject(scene));
    held.emplace_back(new Model(scene));
    held.emplace_back(new Primitive(scene));
    held.emplace_back(new Button(scene));
    std::vector<Entity> deleted;
    for (const auto& handle : held){
        deleted.push_back(handle->getEntity());
    }
    delete scene;

    std::vector<Entity> entities;
    std::unique_ptr<Scene> next = sceneWithEntities(held.size() + 2, entities);
    held.clear();   // nothing of the deleted scene is touched, nor the next one at its address

    for (Entity entity : entities){
        EXPECT_TRUE(next->isEntityCreated(entity)) << "entity " << entity << " of the next scene was destroyed";
    }
}

TEST_F(EntityHandleLifetimeTest, LuaHandlesCollectedAfterTheirSceneWasDeleted){
    Scene* scene = new Scene();
    setSceneGlobal(scene);
    ASSERT_EQ(lua(R"(
        held = { Button(gcScene), Image(gcScene), Text(gcScene), GameObject(gcScene),
                 Primitive(gcScene), Model(gcScene), Sprite(gcScene) }
        for i = 1, 4 do local dropped = Model(gcScene) end
        gcScene = nil
    )"), "");
    delete scene;

    std::vector<Entity> entities;
    std::unique_ptr<Scene> next = sceneWithEntities(16, entities);
    ASSERT_EQ(lua("held = nil; collectgarbage(); collectgarbage()"), "");

    for (Entity entity : entities){
        EXPECT_TRUE(next->isEntityCreated(entity)) << "entity " << entity << " of the next scene was destroyed";
    }
}

TEST_F(EntityHandleLifetimeTest, LuaHandlesStillDestroyTheirEntityWhileTheSceneLives){
    std::unique_ptr<Scene> scene(new Scene());
    setSceneGlobal(scene.get());
    ASSERT_EQ(lua(R"(
        local function make(kind, owned)
            local object = kind(gcScene)
            object.entityOwned = owned
            return object.entity
        end
        held = { Button(gcScene), Model(gcScene) }
        ownedButton, ownedModel = make(Button, true), make(Model, true)
        unownedButton = make(Button, false)
        -- destroyed by the scene before its handle is collected
        local destroyed = Model(gcScene)
        destroyedModel = destroyed.entity
        GameObject.destroyImmediate(destroyed)
        destroyed = nil
        collectgarbage(); collectgarbage()
    )"), "");

    EXPECT_FALSE(luaBool("gcScene:isEntityCreated(ownedButton)")) << "a collected owning handle leaks its entity";
    EXPECT_FALSE(luaBool("gcScene:isEntityCreated(ownedModel)")) << "a collected owning handle leaks its entity";
    EXPECT_FALSE(luaBool("gcScene:isEntityCreated(destroyedModel)"));
    EXPECT_TRUE(luaBool("gcScene:isEntityCreated(unownedButton)")) << "entityOwned = false is left to the scene";
    EXPECT_TRUE(luaBool("gcScene:isEntityCreated(held[1].entity) and gcScene:isEntityCreated(held[2].entity)"));
    lua("ownedButton, ownedModel, unownedButton, destroyedModel = nil, nil, nil, nil");
}

TEST_F(EntityHandleLifetimeTest, OwnershipMovesAndCanBeTaken){
    std::unique_ptr<Scene> scene(new Scene());

    // a move carries the ownership: the moved-from handle leaves the entity alone (the
    // subclasses: TestWrapperOwnership.cpp)
    EntityHandle* first = new EntityHandle(scene.get());
    Entity entity = first->getEntity();
    {
        EntityHandle moved(std::move(*first));
        delete first;
        EXPECT_TRUE(scene->isEntityCreated(entity));
        EXPECT_TRUE(moved.isEntityOwned());
    }
    EXPECT_FALSE(scene->isEntityCreated(entity)) << "the handle the ownership moved to did not destroy the entity";

    // a copy never owns
    {
        GameObject original(scene.get());
        entity = original.getEntity();
        { GameObject copy(original); EXPECT_FALSE(copy.isEntityOwned()); }
        EXPECT_TRUE(scene->isEntityCreated(entity));
    }
    EXPECT_FALSE(scene->isEntityCreated(entity));

    // taking ownership of an existing entity
    entity = scene->createEntity();
    { GameObject taken(scene.get(), entity); taken.setEntityOwned(true); }
    EXPECT_FALSE(scene->isEntityCreated(entity)) << "setEntityOwned(true) did not make the handle destroy the entity";

    // ... and giving it up
    {
        GameObject given(scene.get());
        entity = given.getEntity();
        given.setEntityOwned(false);
    }
    EXPECT_TRUE(scene->isEntityCreated(entity));

    // a handle that took ownership outlives its scene too
    Scene* doomed = new Scene();
    GameObject* late = new GameObject(doomed, doomed->createEntity());
    late->setEntityOwned(true);
    delete doomed;
    std::vector<Entity> entities;
    std::unique_ptr<Scene> next = sceneWithEntities(4, entities);
    delete late;
    for (Entity created : entities){
        EXPECT_TRUE(next->isEntityCreated(created));
    }
}
