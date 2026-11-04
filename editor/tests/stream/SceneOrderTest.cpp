// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Transform array holds each branch in one run, which the moves and the encoders read: a
// GameObject decoded below a parent already placed joins the parent's branch, a save that would
// leave a GameObject out throws (and a split order is found), and the scene's list keeps the
// GameObjects without a Transform where they were (docs/data-safety.md, test_save_completeness.py
// and test_scene_order.py in a running editor).

#include "Project.h"
#include "component/Transform.h"
#include "Stream.h"
#include "util/ProjectUtils.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    Entity make(Scene& scene, const std::string& name, Entity parent = NULL_ENTITY) {
        const Entity entity = scene.createUserEntity();
        scene.setEntityName(entity, name);
        Transform transform;
        transform.parent = parent;
        scene.addComponent<Transform>(entity, transform);
        if (parent != NULL_ENTITY) {
            // its place: the end of the parent's branch, as the editor's creators put it
            scene.moveChildToIndex(entity, scene.findBranchLastIndex(parent) + 1);
        }
        return entity;
    }

    std::vector<std::string> order(Scene& scene) {
        std::vector<std::string> names;
        auto transforms = scene.getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            names.push_back(scene.getEntityName(transforms->getEntity(i)));
        }
        return names;
    }

    std::vector<std::string> names(Scene& scene, const std::vector<Entity>& entities) {
        std::vector<std::string> out;
        for (Entity entity : entities) out.push_back(scene.getEntityName(entity));
        return out;
    }

}

TEST(SceneOrder, ADecodedChildJoinsItsParentsBranch) {
    Scene scene;
    const Entity holder = make(scene, "Holder");
    make(scene, "Later");
    YAML::Node kid;
    kid["type"] = "Entity";
    kid["name"] = "Kid";
    kid["components"]["transform"] = YAML::Node(YAML::NodeType::Map);
    YAML::Node grandchild;
    grandchild["type"] = "Entity";
    grandchild["name"] = "Grandchild";
    grandchild["components"]["transform"] = YAML::Node(YAML::NodeType::Map);
    kid["children"].push_back(grandchild);

    const std::vector<Entity> decoded = Stream::decodeEntity(kid, &scene, nullptr, nullptr, nullptr, holder);
    ASSERT_EQ(decoded.size(), 2u);
    EXPECT_EQ(order(scene), (std::vector<std::string>{"Holder", "Kid", "Grandchild", "Later"}));
    EXPECT_EQ(ProjectUtils::findSplitBranch(&scene), NULL_ENTITY);
}

TEST(SceneOrder, ASplitOrderIsFoundAndTheSaveRefused) {
    auto scene = std::make_unique<Scene>();
    SceneProject project;
    project.scene = scene.get();
    project.name = "Split";
    const Entity holder = make(*scene, "Holder");
    const Entity kid = make(*scene, "Kid", holder);
    const Entity later = make(*scene, "Later");
    project.entities = {holder, kid, later};
    EXPECT_NO_THROW(Stream::encodeSceneProject(nullptr, &project));

    // a script's moveChildToIndex of the child past another root
    scene->moveChildToIndex(kid, scene->findBranchLastIndex(later) + 1);
    EXPECT_EQ(ProjectUtils::findSplitBranch(scene.get()), holder);
    EXPECT_FALSE(ProjectUtils::isBranchPlace(scene.get(), kid));
    try {
        Stream::encodeSceneProject(nullptr, &project);
        ADD_FAILURE() << "the encode of a scene that leaves Kid out did not throw";
    } catch (const Stream::IncompleteSave& e) {
        EXPECT_NE(std::string(e.what()).find("'Kid'"), std::string::npos) << e.what();
        EXPECT_NE(std::string(e.what()).find("left out"), std::string::npos) << e.what();
    }

    scene->moveChildToIndex(kid, scene->findBranchLastIndex(holder) + 1);
    EXPECT_TRUE(ProjectUtils::isBranchPlace(scene.get(), kid));
    EXPECT_NO_THROW(Stream::encodeSceneProject(nullptr, &project));
    project.scene = nullptr;
}

TEST(SceneOrder, TheSortKeepsGameObjectsWithoutATransformInTheirPlace) {
    Scene scene;
    const Entity light = make(scene, "Light");
    const Entity a = make(scene, "A");
    const Entity sky = scene.createUserEntity();
    scene.setEntityName(sky, "Skybox");
    const Entity b = make(scene, "B");
    const Entity c = make(scene, "C");
    std::vector<Entity> entities = {light, a, sky, b, c};

    // C moved before A in the Transform order: the Skybox stays after A
    scene.moveChildToIndex(c, scene.getComponentArray<Transform>()->getIndex(a));
    ProjectUtils::sortEntitiesByTransformOrder(&scene, entities, c);
    EXPECT_EQ(names(scene, entities), (std::vector<std::string>{"Light", "C", "A", "Skybox", "B"}));

    // the undo cannot tell from the list: the list from before puts the Skybox back
    const std::vector<Entity> before = {light, a, sky, b, c};
    scene.moveChildToIndex(c, scene.getComponentArray<Transform>()->size());
    ProjectUtils::sortEntitiesByTransformOrder(&scene, entities);
    ProjectUtils::keepUntransformedPlaces(&scene, before, entities);
    EXPECT_EQ(names(scene, entities), (std::vector<std::string>{"Light", "A", "Skybox", "B", "C"}));
}
