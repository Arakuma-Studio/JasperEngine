// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Copies and moves of the engine's object wrappers (EntityHandle and every subclass in
// object/ and action/). Not HTML UI: engine-level coverage hosted by this harness, like
// TestEntityHandleLifetime.cpp. A wrapper made with (Scene*) owns its entity: a move hands
// that ownership over (the moved-from wrapper leaves the entity alone), a copy is a
// non-owning observer. Sprite also holds the SpriteAnimation it starts: its copies share it
// (one animation per sprite, deleted once, with the last wrapper holding it).

#include "harness/GpuEnvironment.h"
#include "harness/TestSystem.h"

#include "Scene.h"
#include "object/GameObject.h"
#include "object/FluidSimulation.h"
#include "object/Bone.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Light2D.h"
#include "object/LineRenderer.h"
#include "object/Mesh.h"
#include "object/MeshPolygon.h"
#include "object/Mirror.h"
#include "object/Model.h"
#include "object/Points.h"
#include "object/Primitive.h"
#include "object/ShadowCaster2D.h"
#include "object/Sprite.h"
#include "object/Terrain.h"
#include "object/Tilemap.h"
#include "object/environment/Fog.h"
#include "object/environment/ReflectionProbe.h"
#include "object/environment/Skybox.h"
#include "object/sound/AudioSource.h"
#include "object/ui/Button.h"
#include "object/ui/Image.h"
#include "object/ui/InputField.h"
#include "object/ui/LayoutGroup.h"
#include "object/ui/Panel.h"
#include "object/ui/Polygon.h"
#include "object/ui/Progressbar.h"
#include "object/ui/RectTransform.h"
#include "object/ui/Scrollbar.h"
#include "object/ui/Text.h"
#include "object/ui/UIDocument.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "object/physics/Joint2D.h"
#include "object/physics/Rigidbody2D.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "object/physics/Joint3D.h"
#include "object/physics/Rigidbody.h"
#endif
#include "action/Action.h"
#include "action/AlphaTween.h"
#include "action/Animation.h"
#include "action/ColorTween.h"
#include "action/ParticleSystem.h"
#include "action/PositionTween.h"
#include "action/RotationTween.h"
#include "action/ScaleTween.h"
#include "action/SpriteAnimation.h"
#include "action/Tween.h"
#include "action/keyframe/MorphTracks.h"
#include "action/keyframe/RotateTracks.h"
#include "action/keyframe/ScaleTracks.h"
#include "action/keyframe/TranslateTracks.h"
#include "component/ActionComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "script/LuaBinding.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // An owning wrapper of a new entity: (Scene*), or (Scene*, Entity) + setEntityOwned
    // for the wrappers that only view an entity (Bone, the rigid bodies)
    template <typename T>
    struct Make {
        static T* owning(Scene* scene){ return new T(scene); }
    };

    template <typename T>
    T* takeOwnership(Scene* scene){
        T* handle = new T(scene, scene->createEntity());
        handle->setEntityOwned(true);
        return handle;
    }

    template <> struct Make<Bone> { static Bone* owning(Scene* scene){ return takeOwnership<Bone>(scene); } };
#ifdef JASPERENGINE_PHYSICS_2D
    template <> struct Make<Rigidbody2D> { static Rigidbody2D* owning(Scene* scene){ return takeOwnership<Rigidbody2D>(scene); } };
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    template <> struct Make<Rigidbody> { static Rigidbody* owning(Scene* scene){ return takeOwnership<Rigidbody>(scene); } };
#endif

    template <typename T>
    class WrapperOwnershipTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            TestSystem::get().clearLogs();
            scene.reset(new Scene());
        }

        void TearDown() override{
            scene.reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << "errors logged";
        }

        std::unique_ptr<T> owning(){ return std::unique_ptr<T>(Make<T>::owning(scene.get())); }

        std::unique_ptr<Scene> scene;
    };

    using Wrappers = ::testing::Types<
        EntityHandle, GameObject, Mesh, Model, Primitive, Mirror, MeshPolygon, Sprite, Terrain, Tilemap,
        Light, Light2D, Camera, Points, LineRenderer, ShadowCaster2D, Bone,
        Fog, Skybox, ReflectionProbe, AudioSource, FluidSimulation,
        RectTransform, Image, Button, Panel, Scrollbar, Progressbar, InputField, LayoutGroup, Polygon, Text, UIDocument,
#ifdef JASPERENGINE_PHYSICS_2D
        Rigidbody2D, Joint2D,
#endif
#ifdef JASPERENGINE_PHYSICS_3D
        Rigidbody, Joint3D,
#endif
        Action, Tween, AlphaTween, ColorTween, PositionTween, RotationTween, ScaleTween, Animation, ParticleSystem,
        SpriteAnimation, MorphTracks, RotateTracks, ScaleTracks, TranslateTracks>;

    class WrapperNames {
    public:
        template <typename T>
        static std::string GetName(int index){
            const char* names[] = {
                "EntityHandle", "GameObject", "Mesh", "Model", "Primitive", "Mirror", "MeshPolygon", "Sprite", "Terrain", "Tilemap",
                "Light", "Light2D", "Camera", "Points", "LineRenderer", "ShadowCaster2D", "Bone",
                "Fog", "Skybox", "ReflectionProbe", "AudioSource", "FluidSimulation",
                "RectTransform", "Image", "Button", "Panel", "Scrollbar", "Progressbar", "InputField", "LayoutGroup", "Polygon", "Text", "UIDocument",
#ifdef JASPERENGINE_PHYSICS_2D
                "Rigidbody2D", "Joint2D",
#endif
#ifdef JASPERENGINE_PHYSICS_3D
                "Rigidbody", "Joint3D",
#endif
                "Action", "Tween", "AlphaTween", "ColorTween", "PositionTween", "RotationTween", "ScaleTween", "Animation", "ParticleSystem",
                "SpriteAnimation", "MorphTracks", "RotateTracks", "ScaleTracks", "TranslateTracks"};
            return names[index];
        }
    };

}

TYPED_TEST_SUITE(WrapperOwnershipTest, Wrappers, WrapperNames);

TYPED_TEST(WrapperOwnershipTest, MovesAreRealMoves){
    // a move constructor of its own: without one (a declared destructor suppresses it) a
    // "move" is the copy constructor, which is not noexcept
    EXPECT_TRUE(std::is_nothrow_move_constructible<TypeParam>::value) << "moving falls back to the copy constructor";
    EXPECT_TRUE(std::is_nothrow_move_assignable<TypeParam>::value) << "move assignment falls back to copy assignment";
    EXPECT_TRUE(std::is_copy_constructible<TypeParam>::value);
    EXPECT_TRUE(std::is_copy_assignable<TypeParam>::value);
}

TYPED_TEST(WrapperOwnershipTest, MoveConstructionTransfersOwnership){
    Scene* scene = this->scene.get();
    std::unique_ptr<TypeParam> source = this->owning();
    const Entity entity = source->getEntity();
    ASSERT_TRUE(source->isEntityOwned());
    {
        TypeParam moved(std::move(*source));
        EXPECT_TRUE(moved.isEntityOwned()) << "the ownership did not move";
        EXPECT_FALSE(source->isEntityOwned()) << "the moved-from wrapper still owns the entity";
        EXPECT_EQ(moved.getEntity(), entity);
        EXPECT_EQ(moved.getScene(), scene);
        source.reset();
        EXPECT_TRUE(scene->isEntityCreated(entity)) << "the moved-from wrapper destroyed the entity";
    }
    EXPECT_FALSE(scene->isEntityCreated(entity)) << "the wrapper the ownership moved to did not destroy the entity";
}

TYPED_TEST(WrapperOwnershipTest, MoveAssignmentTransfersOwnership){
    Scene* scene = this->scene.get();
    std::unique_ptr<TypeParam> source = this->owning();
    const Entity entity = source->getEntity();
    const Entity other = scene->createEntity();
    {
        TypeParam target(scene, other);   // a non-owning view of another entity
        target = std::move(*source);
        EXPECT_TRUE(target.isEntityOwned()) << "the ownership did not move";
        EXPECT_FALSE(source->isEntityOwned()) << "the moved-from wrapper still owns the entity";
        EXPECT_EQ(target.getEntity(), entity);
        source.reset();
        EXPECT_TRUE(scene->isEntityCreated(entity)) << "the moved-from wrapper destroyed the entity";
    }
    EXPECT_FALSE(scene->isEntityCreated(entity)) << "the wrapper the ownership moved to did not destroy the entity";
    EXPECT_TRUE(scene->isEntityCreated(other)) << "the entity the target viewed before was destroyed";
}

TYPED_TEST(WrapperOwnershipTest, CopiesNeverOwn){
    Scene* scene = this->scene.get();
    std::unique_ptr<TypeParam> original = this->owning();
    const Entity entity = original->getEntity();
    {
        TypeParam copy(*original);
        EXPECT_FALSE(copy.isEntityOwned());
        TypeParam assigned(scene, scene->createEntity());
        assigned = *original;
        EXPECT_FALSE(assigned.isEntityOwned());
        EXPECT_EQ(assigned.getEntity(), entity);
    }
    EXPECT_TRUE(scene->isEntityCreated(entity)) << "a copy destroyed the entity";
    EXPECT_TRUE(original->isEntityOwned());
    original.reset();
    EXPECT_FALSE(scene->isEntityCreated(entity));
}

TYPED_TEST(WrapperOwnershipTest, ContainersMoveTheirElements){
    // a growing vector moves its elements when the move cannot throw, else it copies them
    // and destroys the originals: with copies, the (owning) originals took the entities along
    Scene* scene = this->scene.get();
    std::vector<Entity> entities;
    {
        std::vector<TypeParam> held;
        for (int i = 0; i < 9; i++){
            std::unique_ptr<TypeParam> made = this->owning();
            entities.push_back(made->getEntity());
            held.push_back(std::move(*made));
        }
        for (Entity entity : entities){
            EXPECT_TRUE(scene->isEntityCreated(entity)) << "entity " << entity << " went with a wrapper the vector left";
        }
        for (const TypeParam& handle : held){
            EXPECT_TRUE(handle.isEntityOwned());
        }
    }
    for (Entity entity : entities){
        EXPECT_FALSE(scene->isEntityCreated(entity)) << "entity " << entity << " outlived its wrappers";
    }
}

// ---------------------------------------------------------------- LuaBridge

namespace {

    class WrapperLuaTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            L = LuaBinding::getLuaState();
            ASSERT_NE(L, nullptr);
            TestSystem::get().clearLogs();
            scene.reset(new Scene());
        }

        void TearDown() override{
            lua("pushed = nil; collectgarbage(); collectgarbage()");
            scene.reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << "errors logged";
        }

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

        // pushes an owning temporary into pushed[index] as LuaBridge pushes what a bound
        // function returns by value: Stack<T>::push(L, T&&) moves it into the userdata
        template <typename T>
        Entity pushTemporary(int index){
            lua_getglobal(L, "pushed");
            T made(scene.get());
            const Entity entity = made.getEntity();
            EXPECT_TRUE(luabridge::Stack<T>::push(L, std::move(made)));
            lua_rawseti(L, -2, index);
            lua_pop(L, 1);
            return entity;
        }

        lua_State* L = nullptr;
        std::unique_ptr<Scene> scene;
    };

}

TEST_F(WrapperLuaTest, PushedTemporariesKeepTheirEntity){
    ASSERT_EQ(lua("pushed = {}"), "");
    std::vector<Entity> entities;
    entities.push_back(pushTemporary<GameObject>(1));
    entities.push_back(pushTemporary<Button>(2));
    entities.push_back(pushTemporary<Image>(3));
    entities.push_back(pushTemporary<Text>(4));
    entities.push_back(pushTemporary<Sprite>(5));
    entities.push_back(pushTemporary<Model>(6));
    entities.push_back(pushTemporary<Primitive>(7));
    entities.push_back(pushTemporary<Camera>(8));
    entities.push_back(pushTemporary<Light>(9));
    entities.push_back(pushTemporary<Mesh>(10));
    entities.push_back(pushTemporary<Animation>(11));

    for (size_t i = 0; i < entities.size(); i++){
        EXPECT_TRUE(scene->isEntityCreated(entities[i])) << "pushed[" << i + 1 << "]: the temporary took its entity along";
        EXPECT_TRUE(luaBool("pushed[" + std::to_string(i + 1) + "].entityOwned"))
            << "pushed[" << i + 1 << "]: the Lua copy does not own the entity";
    }
    // usable from Lua, and its entity goes with it
    EXPECT_EQ(lua("pushed[2].name = 'moved in'; pushed[5]:setSize(16, 16); pushed[6]:setPosition(1, 2, 3)"), "");
    EXPECT_EQ(scene->getEntityName(entities[1]), "moved in");
    ASSERT_EQ(lua("pushed = nil; collectgarbage(); collectgarbage()"), "");
    for (size_t i = 0; i < entities.size(); i++){
        EXPECT_FALSE(scene->isEntityCreated(entities[i])) << "pushed[" << i + 1 << "]: the collected handle leaked its entity";
    }
}

TEST_F(WrapperLuaTest, BoundFunctionsReturnOwningWrappers){
    // C++ functions returning a new wrapper by value, called from Lua
    luabridge::getGlobalNamespace(L)
        .beginNamespace("ownershipTest")
            .addFunction("button", [](Scene* scene){ return Button(scene); })
            .addFunction("sprite", [](Scene* scene){ return Sprite(scene); })
            .addFunction("model", [](Scene* scene){ return Model(scene); })
            .addFunction("light", [](Scene* scene){ return Light(scene); })
        .endNamespace();
    ASSERT_TRUE(luabridge::push(L, scene.get()));
    lua_setglobal(L, "ownershipScene");
    ASSERT_EQ(lua(R"(
        pushed = { ownershipTest.button(ownershipScene), ownershipTest.sprite(ownershipScene),
                   ownershipTest.model(ownershipScene), ownershipTest.light(ownershipScene) }
        ownershipScene = nil
    )"), "");
    std::vector<Entity> entities;
    for (int i = 1; i <= 4; i++){
        EXPECT_TRUE(luaBool("pushed[" + std::to_string(i) + "].entityOwned")) << "pushed[" << i << "] does not own its entity";
        EXPECT_EQ(lua("result = pushed[" + std::to_string(i) + "].entity"), "");
        lua_getglobal(L, "result");
        entities.push_back(static_cast<Entity>(lua_tointeger(L, -1)));
        lua_pop(L, 1);
        EXPECT_TRUE(scene->isEntityCreated(entities.back())) << "pushed[" << i << "]: the returned temporary took its entity along";
    }
    ASSERT_EQ(lua("result = nil; pushed = nil; ownershipTest = nil; collectgarbage(); collectgarbage()"), "");
    for (Entity entity : entities){
        EXPECT_FALSE(scene->isEntityCreated(entity)) << "a collected handle leaked its entity";
    }
}

TEST_F(WrapperLuaTest, PushedCopiesDoNotOwn){
    Button original(scene.get());
    ASSERT_TRUE(luabridge::push(L, static_cast<const Button&>(original)));
    lua_setglobal(L, "pushed");
    EXPECT_FALSE(luaBool("pushed.entityOwned"));
    ASSERT_EQ(lua("pushed = nil; collectgarbage(); collectgarbage()"), "");
    EXPECT_TRUE(scene->isEntityCreated(original.getEntity())) << "the collected Lua copy destroyed the entity";
}

// ---------------------------------------------------------------- Sprite's animation

namespace {

    class SpriteAnimationOwnershipTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            TestSystem::get().clearLogs();
            scene.reset(new Scene());
        }

        void TearDown() override{
            scene.reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << "errors logged";
        }

        // the SpriteAnimation actions targeting the sprite
        std::vector<Entity> animationsOf(const Sprite& sprite){
            std::vector<Entity> found;
            for (Entity entity : scene->getEntityList()){
                ActionComponent* action = scene->findComponent<ActionComponent>(entity);
                if (action && action->target == sprite.getEntity() && scene->findComponent<SpriteAnimationComponent>(entity)){
                    found.push_back(entity);
                }
            }
            return found;
        }

        ActionComponent& action(Entity entity){
            return scene->getComponent<ActionComponent>(entity);
        }

        static void animate(Sprite& sprite){
            sprite.addFrame(0, 0, 8, 8);
            sprite.addFrame(8, 0, 8, 8);
            sprite.startAnimation(0, 1, 100, true);
        }

        std::unique_ptr<Scene> scene;
    };

}

TEST_F(SpriteAnimationOwnershipTest, CopiesShareTheAnimation){
    Sprite original(scene.get());
    animate(original);
    std::vector<Entity> animations = animationsOf(original);
    ASSERT_EQ(animations.size(), 1u);
    const Entity animation = animations[0];

    {
        Sprite copy(original);
        // the copy drives the same animation
        copy.pauseAnimation();
        EXPECT_TRUE(action(animation).pauseTrigger);
    }
    EXPECT_TRUE(scene->isEntityCreated(animation)) << "a copy going deleted the sprite's animation";

    {
        Sprite assigned(scene.get());
        animate(assigned);
        const Entity own = animationsOf(assigned).at(0);
        assigned = original;
        EXPECT_FALSE(scene->isEntityCreated(own)) << "copy assignment leaked the animation the sprite had";
        assigned.stopAnimation();
        EXPECT_TRUE(action(animation).stopTrigger);
    }
    EXPECT_TRUE(scene->isEntityCreated(animation)) << "a copy going deleted the sprite's animation";

    // still the original's to use
    original.resumeAnimation();
    EXPECT_TRUE(action(animation).startTrigger);
    EXPECT_EQ(animationsOf(original).size(), 1u) << "the animation was replaced";
}

TEST_F(SpriteAnimationOwnershipTest, TheLastWrapperDeletesTheAnimationOnce){
    Entity animation = NULL_ENTITY;
    Entity sprite = NULL_ENTITY;
    {
        std::unique_ptr<Sprite> original(new Sprite(scene.get()));
        animate(*original);
        animation = animationsOf(*original).at(0);
        sprite = original->getEntity();
        Sprite copy(*original);
        original.reset();   // the sprite's entity goes with its owner, the animation stays with the copy
        EXPECT_FALSE(scene->isEntityCreated(sprite));
        EXPECT_TRUE(scene->isEntityCreated(animation));
    }
    EXPECT_FALSE(scene->isEntityCreated(animation)) << "the last wrapper leaked the animation";
}

TEST_F(SpriteAnimationOwnershipTest, MovesCarryTheAnimation){
    std::unique_ptr<Sprite> source(new Sprite(scene.get()));
    animate(*source);
    const Entity animation = animationsOf(*source).at(0);
    {
        Sprite moved(std::move(*source));
        source.reset();
        EXPECT_TRUE(scene->isEntityCreated(animation)) << "the moved-from sprite deleted the animation";
        moved.pauseAnimation();
        EXPECT_TRUE(action(animation).pauseTrigger);

        Sprite target(scene.get());
        target = std::move(moved);
        EXPECT_TRUE(scene->isEntityCreated(animation));
        target.stopAnimation();
        EXPECT_TRUE(action(animation).stopTrigger);
    }
    EXPECT_FALSE(scene->isEntityCreated(animation));
}

TEST_F(SpriteAnimationOwnershipTest, ManyCopiesInAContainer){
    std::vector<Sprite> sprites;
    for (int i = 0; i < 6; i++){
        sprites.emplace_back(scene.get());
        animate(sprites.back());
    }
    std::vector<Sprite> copies(sprites.begin(), sprites.end());
    copies.insert(copies.end(), sprites.begin(), sprites.end());
    copies.clear();
    for (Sprite& sprite : sprites){
        EXPECT_EQ(animationsOf(sprite).size(), 1u);
        sprite.pauseAnimation();
    }
    sprites.clear();
    for (Entity entity : scene->getEntityList()){
        EXPECT_EQ(scene->findComponent<SpriteAnimationComponent>(entity), nullptr) << "an animation outlived every sprite";
    }
}
