// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Animation clips spec 9.R3/R15 in the editor: a Model in the legacy layout switches to the node
// hierarchy in the same undo step as what puts an Animation Player on or above it (Add Component,
// a move under a player: editor/animation/PlayerModelLayout), keeping every bone where it was even
// when the glTF scene root is scaled and rotated (ModelLoadCmd takes the root's matrix off the
// model entity, now carried by the root node's entity); undo and redo restore both layouts. And
// a pasted Model value applies animationEntities with its load (ComponentValuesCmd).
// A headless project; the models load from absolute paths (the pose-animation fixture's heroes,
// the T-pose one rewritten with a 0.01 scale and a 90 degree turn on its scene root node).

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "PhysicsTestScene.h"
#include "Project.h"
#include "Stream.h"
#include "animation/PlayerModelLayout.h"
#include "command/CommandHandle.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/ComponentValuesCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/MoveEntityOrderCmd.h"
#include "component/AnimationPlayerComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"

#include "json.hpp"

#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    fs::path fixtureModel(const std::string& name) {
        return (fs::path(JASPER_EDITOR_TEST_FIXTURES) / ".." / ".." / ".." / "tools" / "fixtures" / "pose-animation" /
                "assets" / "models" / name).lexically_normal();
    }

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            physicstest::installTestSystem();
            Engine::setAsyncLoading(false);
            const fs::path root = fs::temp_directory_path() / ("jasper-player-layout-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets" / "models");
            std::ofstream(out.dir / "project.yaml") << "name: playerlayout\nassetsDir: assets\n";
            // the T-pose hero with a scaled and turned scene root node, as Mixamo/Blender exports have
            nlohmann::json gltf = nlohmann::json::parse(readAll(fixtureModel("hero_tpose.gltf")));
            gltf["nodes"][0]["scale"] = {0.01, 0.01, 0.01};
            gltf["nodes"][0]["rotation"] = {0.7071068, 0.0, 0.0, 0.7071068};
            std::ofstream(out.dir / "assets" / "models" / "hero_scaled.gltf", std::ios::binary) << gltf.dump(1);
            AppSettings::initialize();
            out.project.reset(new Project());
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        return h;
    }

    Project* project() { return headless().project.get(); }

    std::string scaledHero() { return (headless().dir / "assets" / "models" / "hero_scaled.gltf").generic_string(); }

    uint32_t newScene() {
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("Layout", SceneType::SCENE_3D);
        return p->getSelectedSceneId();
    }

    Scene* sceneOf(uint32_t sceneId) { return project()->getScene(sceneId)->scene; }
    CommandHistory* history(uint32_t sceneId) { return CommandHandle::get(sceneId); }

    Entity createGameObject(uint32_t sceneId, const std::string& name) {
        CreateEntityCmd* cmd = new CreateEntityCmd(project(), sceneId, name, EntityCreationType::GAMEOBJECT);
        cmd->setQuiet(true);
        history(sceneId)->addCommandNoMerge(cmd);
        return cmd->getEntity();
    }

    // a Model GameObject with `path` loaded by the editor's own command (a new model: the loader
    // puts the scene root's transform on the entity in the legacy layout)
    Entity createModel(uint32_t sceneId, const std::string& path) {
        CreateEntityCmd* create = new CreateEntityCmd(project(), sceneId, "Hero", EntityCreationType::MODEL);
        create->setQuiet(true);
        history(sceneId)->addCommandNoMerge(create);
        const Entity entity = create->getEntity();
        history(sceneId)->addCommandNoMerge(new ModelLoadCmd(project(), sceneId, entity, path));
        const ModelComponent& model = sceneOf(sceneId)->getComponent<ModelComponent>(entity);
        EXPECT_FALSE(model.loadedFilename.empty()) << "loaded " << path << " (file '" << model.filename << "')";
        return entity;
    }

    Matrix4 localMatrix(const Transform& t) {
        return Matrix4::translateMatrix(t.position) * t.rotation.getRotationMatrix() * Matrix4::scaleMatrix(t.scale);
    }

    Matrix4 worldMatrix(Scene* scene, Entity entity) {
        Matrix4 world;
        for (int depth = 0; entity != NULL_ENTITY && depth < 1000; depth++) {
            const Transform* t = scene->findComponent<Transform>(entity);
            if (!t) break;
            world = localMatrix(*t) * world;
            entity = t->parent;
        }
        return world;
    }

    bool isUnder(Scene* scene, Entity entity, Entity root) {
        for (int depth = 0; entity != NULL_ENTITY && depth < 1000; depth++) {
            if (entity == root) return true;
            const Transform* t = scene->findComponent<Transform>(entity);
            entity = t ? t->parent : NULL_ENTITY;
        }
        return false;
    }

    // the one entity named `name` under `root`
    Entity findUnder(Scene* scene, Entity root, const std::string& name) {
        Entity found = NULL_ENTITY;
        int count = 0;
        auto transforms = scene->getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            Entity e = transforms->getEntity(i);
            if (e != root && scene->getEntityName(e) == name && isUnder(scene, e, root)) {
                found = e;
                count++;
            }
        }
        return count == 1 ? found : NULL_ENTITY;
    }

    void expectSameMatrix(const Matrix4& a, const Matrix4& b, const std::string& what) {
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                EXPECT_NEAR(a[c][r], b[c][r], 2e-5f) << what << " [" << c << "][" << r << "]";
            }
        }
    }

    const ModelComponent& modelOf(uint32_t sceneId, Entity entity) {
        return sceneOf(sceneId)->getComponent<ModelComponent>(entity);
    }

    // the legacy layout of the scaled hero, with its bone matrices
    struct Legacy {
        Matrix4 hand;
        Matrix4 head;
        Matrix4 rootLocal;
    };

    Legacy expectLegacy(uint32_t sceneId, Entity model) {
        Scene* scene = sceneOf(sceneId);
        Legacy out;
        EXPECT_TRUE(modelOf(sceneId, model).nodesIdMapping.empty()) << "the legacy layout";
        EXPECT_NE(modelOf(sceneId, model).skeleton, NULL_ENTITY);
        EXPECT_NEAR(scene->getComponent<Transform>(model).scale.x, 0.01f, 1e-6f) << "the scene root's scale on the entity";
        Entity hand = findUnder(scene, model, "mixamorig:LeftHand");
        Entity head = findUnder(scene, model, "mixamorig:Head");
        EXPECT_NE(hand, NULL_ENTITY);
        EXPECT_NE(head, NULL_ENTITY);
        out.hand = worldMatrix(scene, hand);
        out.head = worldMatrix(scene, head);
        out.rootLocal = localMatrix(scene->getComponent<Transform>(model));
        return out;
    }

    void expectNodeHierarchy(uint32_t sceneId, Entity model, const Legacy& legacy) {
        Scene* scene = sceneOf(sceneId);
        ASSERT_FALSE(modelOf(sceneId, model).nodesIdMapping.empty()) << "the node hierarchy";
        Entity heroNode = findUnder(scene, model, "Hero");
        ASSERT_NE(heroNode, NULL_ENTITY) << "the scene root node's entity";
        EXPECT_NEAR(scene->getComponent<Transform>(heroNode).scale.x, 0.01f, 1e-6f);
        // the root's matrix moved from the model entity to its node: the entity is left identity here
        expectSameMatrix(localMatrix(scene->getComponent<Transform>(model)), Matrix4(), "model entity local");
        Entity hand = findUnder(scene, model, "mixamorig:LeftHand");
        Entity head = findUnder(scene, model, "mixamorig:Head");
        ASSERT_NE(hand, NULL_ENTITY);
        ASSERT_NE(head, NULL_ENTITY);
        expectSameMatrix(worldMatrix(scene, hand), legacy.hand, "LeftHand world");
        expectSameMatrix(worldMatrix(scene, head), legacy.head, "Head world");
    }

    class PlayerModelLayoutTest : public ::testing::Test {
    protected:
        void SetUp() override {
            ASSERT_TRUE(headless().ready);
            // loads finish inside their command (loading a project turns async loading on)
            Engine::setAsyncLoading(false);
        }
    };

}

// Add Component > Animation Player on a legacy model: one undo step switches the layout, every
// bone stays (scaled, turned scene root); undo and redo
TEST_F(PlayerModelLayoutTest, AddingAPlayerSwitchesTheLayoutInOneStep) {
    const uint32_t sceneId = newScene();
    const Entity model = createModel(sceneId, scaledHero());
    const Legacy legacy = expectLegacy(sceneId, model);

    history(sceneId)->addCommandNoMerge(new AddComponentCmd(project(), sceneId, model, ComponentType::AnimationPlayerComponent));
    ASSERT_TRUE(sceneOf(sceneId)->findComponent<AnimationPlayerComponent>(model));
    expectNodeHierarchy(sceneId, model, legacy);

    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_FALSE(sceneOf(sceneId)->findComponent<AnimationPlayerComponent>(model)) << "one step: the player goes too";
    const Legacy back = expectLegacy(sceneId, model);
    expectSameMatrix(back.hand, legacy.hand, "LeftHand world after undo");
    expectSameMatrix(back.rootLocal, legacy.rootLocal, "model local after undo");

    ASSERT_TRUE(history(sceneId)->redo());
    ASSERT_TRUE(sceneOf(sceneId)->findComponent<AnimationPlayerComponent>(model));
    expectNodeHierarchy(sceneId, model, legacy);
}

// Moving a legacy model under a GameObject with a player switches it in the move's step
TEST_F(PlayerModelLayoutTest, MovingAModelUnderAPlayerSwitchesTheLayoutInOneStep) {
    const uint32_t sceneId = newScene();
    const Entity rig = createGameObject(sceneId, "Rig");
    history(sceneId)->addCommandNoMerge(new AddComponentCmd(project(), sceneId, rig, ComponentType::AnimationPlayerComponent));
    const Entity model = createModel(sceneId, scaledHero());
    const Legacy legacy = expectLegacy(sceneId, model);

    // a reparent keeps the world matrix the last frame computed: no frame runs here
    {
        Scene* scene = sceneOf(sceneId);
        auto transforms = scene->getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            transforms->getComponentFromIndex(i).modelMatrix = worldMatrix(scene, transforms->getEntity(i));
        }
    }
    history(sceneId)->addCommandNoMerge(new MoveEntityOrderCmd(project(), sceneId, model, rig, InsertionType::INTO));
    EXPECT_EQ(sceneOf(sceneId)->getComponent<Transform>(model).parent, rig);
    expectNodeHierarchy(sceneId, model, legacy);

    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_EQ(sceneOf(sceneId)->getComponent<Transform>(model).parent, NULL_ENTITY);
    const Legacy back = expectLegacy(sceneId, model);
    expectSameMatrix(back.hand, legacy.hand, "LeftHand world after undo");

    ASSERT_TRUE(history(sceneId)->redo());
    EXPECT_EQ(sceneOf(sceneId)->getComponent<Transform>(model).parent, rig);
    expectNodeHierarchy(sceneId, model, legacy);

    // a model already in the node hierarchy, or one with no player above, is left alone
    EXPECT_TRUE(PlayerModelLayout::modelsNeedingReload(project(), sceneId, rig).empty());
    const Entity loose = createModel(sceneId, scaledHero());
    EXPECT_TRUE(PlayerModelLayout::modelsNeedingReload(project(), sceneId, loose).empty());
}

// A pasted Model value carries animationEntities into its load, in the same step
TEST_F(PlayerModelLayoutTest, PastedModelValueAppliesAnimationEntitiesWithItsLoad) {
    const uint32_t sceneId = newScene();
    const std::string animated = fixtureModel("hero_animated.gltf").generic_string();
    const Entity model = createModel(sceneId, scaledHero());

    ModelComponent value;
    value.filename = animated;
    value.animationEntities = false;
    auto clip = std::make_shared<ComponentClip>();
    clip->type = ComponentType::ModelComponent;
    clip->value = Stream::encodeModelComponent(value);
    ASSERT_FALSE(clip->value["animationEntities"].as<bool>());

    std::string reason;
    Command* paste = ComponentValuesCmd::buildPasteValues(project(), sceneId, {model}, ComponentType::ModelComponent, clip, &reason);
    ASSERT_TRUE(paste) << reason;
    history(sceneId)->addCommandNoMerge(paste);
    EXPECT_EQ(modelOf(sceneId, model).filename, animated);
    EXPECT_FALSE(modelOf(sceneId, model).animationEntities);
    EXPECT_TRUE(modelOf(sceneId, model).animations.empty()) << "the load honoured the pasted flag";

    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_TRUE(modelOf(sceneId, model).animationEntities);
    EXPECT_NE(modelOf(sceneId, model).filename, animated);

    // the same file with only the flag changed reloads it: the entity animations come and go
    history(sceneId)->addCommandNoMerge(new ModelLoadCmd(project(), sceneId, model, animated));
    ASSERT_EQ(modelOf(sceneId, model).animations.size(), 5u);
    paste = ComponentValuesCmd::buildPasteValues(project(), sceneId, {model}, ComponentType::ModelComponent, clip, &reason);
    ASSERT_TRUE(paste) << reason;
    history(sceneId)->addCommandNoMerge(paste);
    EXPECT_FALSE(modelOf(sceneId, model).animationEntities);
    EXPECT_TRUE(modelOf(sceneId, model).animations.empty());
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_TRUE(modelOf(sceneId, model).animationEntities);
    EXPECT_EQ(modelOf(sceneId, model).animations.size(), 5u);
}
