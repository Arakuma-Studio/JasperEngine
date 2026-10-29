// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The three-way scene merge (docs/specs/S29-version-control.md R17, AC13)

#include "vcs/scene/SceneMerge.h"
#include "vcs/scene/SceneDiff.h"

#include "support/EditorTestSupport.h"
#include "StreamMigrations.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::vcs::scene;

namespace {

    std::string entity(uint64_t id, const std::string& name, const std::string& position = "[0, 0, 0]", const std::string& extra = "") {
        return "  - type: Entity\n    entity: " + std::to_string(id) + "\n    name: " + name +
               "\n    components:\n      transform:\n        position: " + position +
               "\n        rotation: [1, 0, 0, 0]\n        scale: [1, 1, 1]\n" + extra;
    }

    std::string scene(const std::string& entities, uint64_t mainCamera = 0) {
        return "formatVersion: " + std::to_string(StreamMigrations::currentVersion(DocumentKind::Scene)) +
               "\nid: 1\nname: Test\nsceneType: scene_3d\nmainCamera: " + std::to_string(mainCamera) + "\nentities:\n" + entities;
    }

    std::string reference(uint64_t target) {
        return "      script:\n        scripts:\n          - type: lua\n            path: Follow.lua\n            className: Follow\n"
               "            enabled: true\n            properties:\n              - name: target\n                type: EntityReference\n"
               "                value: " + std::to_string(target) + "\n";
    }

    SceneYamlModel model(const std::string& text) {
        SceneYamlModel out;
        std::string error;
        EXPECT_TRUE(SceneYamlModel::parse(text, out, &error)) << error;
        return out;
    }

    MergeResult merge(const std::string& base, const std::string& ours, const std::string& theirs,
                      const std::map<std::string, MergeSide>& resolutions = {}) {
        MergeResult result = mergeSceneTexts(base, ours, theirs, resolutions);
        EXPECT_TRUE(result.ok) << result.error;
        return result;
    }

    std::string positionOf(const SceneYamlModel& model, uint64_t id) {
        const SceneObject* object = model.find(id);
        if (!object) return "<none>";
        const YAML::Node* transform = object->component("transform");
        return transform ? flowString((*transform)["position"]) : "<no transform>";
    }

}

TEST(SceneMerge, NonOverlappingEditsMergeCleanly) {
    const std::string base = scene(entity(10, "A") + entity(11, "B"));
    const std::string ours = scene(entity(10, "A", "[1, 0, 0]") + entity(11, "B"));
    const std::string theirs = scene(entity(10, "A") + entity(11, "B", "[0, 2, 0]"));
    const MergeResult result = merge(base, ours, theirs);
    EXPECT_TRUE(result.conflicts.empty());
    EXPECT_EQ(positionOf(result.merged, 10), "[1, 0, 0]");
    EXPECT_EQ(positionOf(result.merged, 11), "[0, 2, 0]");
    // merging is symmetric for disjoint edits
    const MergeResult swapped = merge(base, theirs, ours);
    EXPECT_TRUE(diffScenes(result.merged, swapped.merged).empty());
}

TEST(SceneMerge, SamePropertyDifferentValuesIsOneConflict) {
    const std::string base = scene(entity(10, "A"));
    const std::string ours = scene(entity(10, "A", "[1, 0, 0]"));
    const std::string theirs = scene(entity(10, "A", "[2, 0, 0]"));
    const MergeResult result = merge(base, ours, theirs);
    ASSERT_EQ(result.conflicts.size(), 1u);
    EXPECT_EQ(result.conflicts[0].object, 10u);
    EXPECT_EQ(result.conflicts[0].path, "transform.position");
    EXPECT_EQ(result.conflicts[0].base, "[0, 0, 0]");
    EXPECT_EQ(result.conflicts[0].ours, "[1, 0, 0]");
    EXPECT_EQ(result.conflicts[0].theirs, "[2, 0, 0]");
    // ours stays until resolved
    EXPECT_EQ(positionOf(result.merged, 10), "[1, 0, 0]");
    // the same change on both sides is no conflict
    EXPECT_TRUE(merge(base, ours, ours).conflicts.empty());
    // a resolution takes theirs
    const MergeResult resolved = merge(base, ours, theirs, {{result.conflicts[0].key(), MergeSide::Theirs}});
    EXPECT_TRUE(resolved.conflicts.empty());
    EXPECT_EQ(positionOf(resolved.merged, 10), "[2, 0, 0]");
}

TEST(SceneMerge, BothSidesAddTheSameIdTheirsIsRenumbered) {
    const std::string base = scene(entity(1000, "Root"));
    const std::string ours = scene(entity(1000, "Root") + entity(1050, "Ours"));
    // theirs adds 1050 too, and a GameObject that refers to it
    const std::string theirs = scene(entity(1000, "Root") + entity(1050, "Theirs", "[5, 0, 0]") + entity(1051, "Follower", "[0, 0, 0]", reference(1050)),
                                     1050);
    const MergeResult result = merge(base, ours, theirs);
    ASSERT_EQ(result.renumbered.size(), 1u);
    const uint64_t renumbered = result.renumbered.at(1050);
    // above every id of the three sides
    EXPECT_EQ(renumbered, 1052u);
    ASSERT_NE(result.merged.find(1050), nullptr);
    EXPECT_EQ(result.merged.find(1050)->name, "Ours");
    ASSERT_NE(result.merged.find(renumbered), nullptr);
    EXPECT_EQ(result.merged.find(renumbered)->name, "Theirs");
    EXPECT_EQ(positionOf(result.merged, renumbered), "[5, 0, 0]");
    // the reference follows it
    const SceneObject* follower = result.merged.find(1051);
    ASSERT_NE(follower, nullptr);
    const YAML::Node* script = follower->component("script");
    ASSERT_NE(script, nullptr);
    EXPECT_EQ((*script)["scripts"][0]["properties"][0]["value"].Scalar(), std::to_string(renumbered));
    // and mainCamera, which only theirs changed
    EXPECT_TRUE(result.conflicts.empty());
    const YAML::Node* camera = result.merged.find(kSceneSettingsId)->component("mainCamera");
    ASSERT_NE(camera, nullptr);
    EXPECT_EQ(camera->Scalar(), std::to_string(renumbered));
}

TEST(SceneMerge, NextEntityTakesTheMax) {
    auto prefab = [](uint64_t next, const std::string& members) {
        return "type: Prefab\nversion: " + std::to_string(StreamMigrations::currentVersion(DocumentKind::Prefab)) +
               "\nguid: abcd\nnextEntity: " + std::to_string(next) + "\nroot: 1001\nmembers:\n" + members;
    };
    const std::string base = prefab(1002, entity(1001, "Root"));
    const std::string ours = prefab(1003, entity(1001, "Root") + entity(1002, "Ours"));
    const std::string theirs = prefab(1010, entity(1001, "Root") + entity(1005, "Theirs"));
    const MergeResult result = merge(base, ours, theirs);
    EXPECT_TRUE(result.conflicts.empty());
    EXPECT_EQ(result.merged.nextEntity(), 1010u);
    EXPECT_NE(result.merged.find(1002), nullptr);
    EXPECT_NE(result.merged.find(1005), nullptr);
}

TEST(SceneMerge, DeleteVersusModifyIsAConflict) {
    const std::string base = scene(entity(10, "A") + entity(11, "B"));
    const std::string ours = scene(entity(10, "A"));
    const std::string theirs = scene(entity(10, "A") + entity(11, "B", "[9, 9, 9]"));
    const MergeResult result = merge(base, ours, theirs);
    ASSERT_EQ(result.conflicts.size(), 1u);
    EXPECT_EQ(result.conflicts[0].object, 11u);
    EXPECT_EQ(result.conflicts[0].path, "");
    EXPECT_EQ(result.merged.find(11), nullptr);
    // theirs as the resolution keeps it, modified
    const MergeResult kept = merge(base, ours, theirs, {{result.conflicts[0].key(), MergeSide::Theirs}});
    EXPECT_EQ(positionOf(kept.merged, 11), "[9, 9, 9]");
    // deleted on one side and untouched on the other: deleted, no conflict
    EXPECT_TRUE(merge(base, ours, base).conflicts.empty());
    EXPECT_EQ(merge(base, base, ours).merged.find(11), nullptr);
}

TEST(SceneMerge, UnparseableInputFails) {
    const MergeResult result = mergeSceneTexts(scene(entity(10, "A")), "entities: [unclosed", scene(entity(10, "A")));
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
}

TEST(SceneMerge, MergedOutputRoundTripsThroughLoadAndSave) {
    // a scene as this editor saves it, edited on two sides
    const auto decoded = editor::test::decodeSceneFile(editor::test::fixture("scenes/shader-library-export-2d.scene"));
    const std::string base = editor::test::encodeSceneText(decoded);
    SceneYamlModel baseModel = model(base);
    const std::vector<uint64_t> order = baseModel.treeOrder();
    ASSERT_GE(order.size(), 4u);
    auto edited = [&](uint64_t id, const std::string& name) {
        SceneYamlModel copy = model(base);
        YAML::Node* own = copy.objects[id].component(kGameObjectKey);
        EXPECT_NE(own, nullptr);
        if (own) (*own)["name"] = name;
        return copy.text();
    };
    const std::string ours = edited(order[1], "Renamed by us");
    const std::string theirs = edited(order[3], "Renamed by them");
    const MergeResult result = merge(base, ours, theirs);
    ASSERT_TRUE(result.conflicts.empty());
    EXPECT_EQ(result.merged.find(order[1])->name, "Renamed by us");
    EXPECT_EQ(result.merged.find(order[3])->name, "Renamed by them");
    // the editor loads it and saves it back unchanged
    const auto reloaded = editor::test::decodeSceneText(result.text, "merged.scene");
    EXPECT_EQ(editor::test::encodeSceneText(reloaded), result.text);
}

TEST(SceneMerge, ConflictsFile) {
    const MergeResult result = merge(scene(entity(10, "A")), scene(entity(10, "A", "[1, 0, 0]")), scene(entity(10, "A", "[2, 0, 0]")));
    const YAML::Node file = YAML::Load(conflictsText("Main.scene", result.conflicts));
    EXPECT_EQ(file["file"].Scalar(), "Main.scene");
    ASSERT_EQ(file["conflicts"].size(), 1u);
    EXPECT_EQ(file["conflicts"][0]["path"].Scalar(), "transform.position");
    EXPECT_EQ(file["conflicts"][0]["ours"].Scalar(), "[1, 0, 0]");
    EXPECT_EQ(file["conflicts"][0]["theirs"].Scalar(), "[2, 0, 0]");
}
