// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's clip rows on a prefab instance (animation clips spec 9.R8, T2c review I1): the
// clip field's Apply/Revert covers the whole entry, so the path and the GUID that follows it go
// back (or to the prefab) together; a row covering the path alone would leave the overridden
// GUID, which wins on the next load. Runs a headless project, as AnimationClipAssetsTest does.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "JasperEngine.h"
#include "Platform.h"
#include "Project.h"
#include "Stream.h"
#include "animation/AnimationClipDocument.h"
#include "command/CommandHandle.h"
#include "command/type/CreatePrefabCmd.h"
#include "component/MeshComponent.h"
#include "window/InspectorWindow.h"

#include "support/PosixCompat.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-inspector-clip-prefab-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            std::ofstream(out.dir / "project.yaml") << "name: inspectorclipprefab\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            // the engine runs already (TestMain, its TestSystem): this project's folders are its paths
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        // every test of the project (the paths end with each test)
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    Project* project() { return headless().project.get(); }

    ClipData oneTrack() {
        ClipData data;
        ClipTrackData track;
        track.path = "Arm";
        track.channel = ClipChannel::Position;
        ClipKey key;
        key.value = {0.0f, 0.0f, 0.0f};
        track.keys = {key};
        data.tracks.push_back(track);
        return data;
    }

    std::string createClip(const std::string& path) {
        std::string error;
        std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::create(project(), path, oneTrack(), "Create", &error);
        EXPECT_TRUE(document) << error;
        return document ? document->data().guid : std::string();
    }

    bool contains(const std::vector<std::string>& names, const std::string& name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    }

}

TEST(InspectorClipPrefab, RevertingAClipRowBringsThePathAndTheGuidBack) {
    ASSERT_TRUE(headless().ready);
    const std::string dir = "inspector_prefab";
    fs::remove_all(headless().dir / dir);
    fs::create_directories(headless().dir / dir);
    const std::string guidA = createClip(dir + "/a.anim");
    const std::string guidB = createClip(dir + "/b.anim");
    ASSERT_FALSE(guidA.empty());
    ASSERT_NE(guidA, guidB);

    // a prefab instance whose player lists a.anim
    project()->createNewScene("Prefab Clips", SceneType::SCENE_3D);
    SceneProject* sceneProject = project()->getScene(project()->getSelectedSceneId());
    const uint32_t sceneId = sceneProject->id;
    Scene* scene = sceneProject->scene;
    const Entity entity = scene->createEntity();
    scene->addComponent<Transform>(entity);
    sceneProject->entities.push_back(entity);
    AnimationPlayerComponent player;
    AnimationPlayerClip entry;
    entry.clip = dir + "/a.anim";
    entry.guid = guidA;
    player.clips = {entry};
    scene->addComponent<AnimationPlayerComponent>(entity, player);
    CommandHandle::get(sceneId)->addCommandNoMerge(new CreatePrefabCmd(project(), sceneId, fs::path(dir) / "Hero.prefab",
        Stream::encodeEntity(entity, scene, project(), sceneProject)));
    ASSERT_TRUE(project()->isPrefabInstanceRoot(sceneId, entity));

    // the Inspector's clip field sets the path and its GUID (one MultiPropertyCmd, an override each)
    AnimationPlayerComponent& instance = scene->getComponent<AnimationPlayerComponent>(entity);
    instance.clips[0].clip = dir + "/b.anim";
    instance.clips[0].guid = guidB;
    ASSERT_TRUE(project()->recordPrefabEdit(sceneId, entity, ComponentType::AnimationPlayerComponent, {"clips[0].clip"}));
    ASSERT_TRUE(project()->recordPrefabEdit(sceneId, entity, ComponentType::AnimationPlayerComponent, {"clips[0].guid"}));
    const std::set<std::string> overridden = project()->getOverriddenProperties(sceneId, entity, ComponentType::AnimationPlayerComponent);

    // a row covering the path alone would leave the GUID
    EXPECT_FALSE(contains(InspectorWindow::matchingPrefabOverrides(overridden, "clips[0].clip"), "clips[0].guid"));
    // the clip row is the entry: both
    const std::vector<std::string> covered = InspectorWindow::matchingPrefabOverrides(overridden, "clips[0]");
    EXPECT_TRUE(contains(covered, "clips[0].clip"));
    EXPECT_TRUE(contains(covered, "clips[0].guid"));
    // the Clips header row covers every entry
    EXPECT_TRUE(contains(InspectorWindow::matchingPrefabOverrides(overridden, "clips"), "clips[0].guid"));
    // another entry's row covers none of them
    EXPECT_TRUE(InspectorWindow::matchingPrefabOverrides(overridden, "clips[1]").empty());

    // Revert of the row (what its context menu runs): both come back
    ASSERT_TRUE(project()->revertPropertyOverride(sceneId, entity, ComponentType::AnimationPlayerComponent, covered));
    const AnimationPlayerComponent& reverted = scene->getComponent<AnimationPlayerComponent>(entity);
    ASSERT_EQ(reverted.clips.size(), 1u);
    EXPECT_EQ(reverted.clips[0].clip, dir + "/a.anim");
    EXPECT_EQ(reverted.clips[0].guid, guidA);
    EXPECT_TRUE(project()->getOverriddenProperties(sceneId, entity, ComponentType::AnimationPlayerComponent).empty());
    sceneProject->isModified = false;
}

// FX6: a prefab instance's override of a blend shape weight past the targets the asset's weights
// cover is the instance's own: the re-import keeps it, and so does a model load (the count
// follows the overridden index, MeshComponent::morphTargetCount)
TEST(InspectorClipPrefab, AnOverriddenBlendShapeWeightRaisesTheTargetCount) {
    ASSERT_TRUE(headless().ready);
    const std::string dir = "inspector_prefab_morph";
    fs::remove_all(headless().dir / dir);
    fs::create_directories(headless().dir / dir);
    project()->createNewScene("Prefab Weights", SceneType::SCENE_3D);
    SceneProject* sceneProject = project()->getScene(project()->getSelectedSceneId());
    const uint32_t sceneId = sceneProject->id;
    Scene* scene = sceneProject->scene;
    const Entity entity = scene->createEntity();
    scene->addComponent<Transform>(entity);
    MeshComponent mesh;
    mesh.morphWeightsSet = true;
    mesh.morphTargetCount = 1;
    mesh.morphWeights[0] = 0.2f;
    scene->addComponent<MeshComponent>(entity, mesh);
    sceneProject->entities.push_back(entity);
    CommandHandle::get(sceneId)->addCommandNoMerge(new CreatePrefabCmd(project(), sceneId, fs::path(dir) / "Jelly.prefab",
        Stream::encodeEntity(entity, scene, project(), sceneProject)));
    ASSERT_TRUE(project()->isPrefabInstanceRoot(sceneId, entity));

    scene->getComponent<MeshComponent>(entity).morphWeights[3] = 0.6f;
    ASSERT_TRUE(project()->recordPrefabEdit(sceneId, entity, ComponentType::MeshComponent, {"morphWeights[3]"}));
    ASSERT_TRUE(project()->reimportPrefabInstance(sceneProject, entity, YAML::Node()));
    const MeshComponent& instance = scene->getComponent<MeshComponent>(entity);
    EXPECT_FLOAT_EQ(instance.morphWeights[3], 0.6f) << "the override applies";
    EXPECT_FLOAT_EQ(instance.morphWeights[0], 0.2f) << "the asset's weight";
    EXPECT_TRUE(instance.morphWeightsSet);
    EXPECT_GE(instance.morphTargetCount, 4) << "a load keeps the overridden weight";
    sceneProject->isModified = false;
}
