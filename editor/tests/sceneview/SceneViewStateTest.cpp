// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S26 R36 (AC20): the Scene view's workspace.yaml keys. An older entry loads with today's
// behaviour (the fly speed offset migrated), unknown keys survive a save, and GameObjects that
// are gone are not written back.

#include "Workspace.h"
#include "sceneview/SceneViewState.h"
#include "support/EditorTestSupport.h"

#include "Scene.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(Workspace, LoadsLegacyWithoutViewKeys) {
    // what an editor before S26 wrote: display settings and the editor camera (walkSpeed = the
    // offset from the default speed 10)
    const YAML::Node node = YAML::Load(
        "showGrid3D: false\n"
        "editorCamera:\n"
        "  type: perspective\n"
        "  position: [1, 2, 3]\n"
        "  target: [0, 0, 0]\n"
        "  yfov: 1.0471975512\n"
        "  nearClip: 0.5\n"
        "  farClip: 2000\n"
        "  walkSpeed: 5\n");
    const Workspace::SceneState state = Workspace::decodeSceneEntry(node);

    EXPECT_FLOAT_EQ(state.viewState.camera.speed, 15.0f) << "Camera Speed is 10 + the legacy offset";
    EXPECT_NEAR(state.viewState.camera.fieldOfView, 60.0f, 1e-3f);
    EXPECT_FLOAT_EQ(state.viewState.camera.nearClip, 0.5f);
    EXPECT_FLOAT_EQ(state.viewState.camera.farClip, 2000.0f);
    EXPECT_TRUE(state.viewState.camera.easing);
    EXPECT_TRUE(state.viewState.camera.acceleration);
    EXPECT_FALSE(state.editorCamera["walkSpeed"].IsDefined()) << "the offset is not read twice";
    EXPECT_TRUE(state.editorCamera["position"].IsDefined());

    // everything else as before S26
    EXPECT_EQ(state.viewState.layout, SceneViewLayout::SINGLE);
    EXPECT_FALSE(state.viewState.orthographic);
    EXPECT_EQ(state.viewState.layerFilter.visible, LAYER_MASK_EVERYTHING);
    EXPECT_EQ(state.viewState.layerFilter.pickable, LAYER_MASK_EVERYTHING);
    EXPECT_TRUE(state.viewState.hiddenEntities.empty());
    EXPECT_TRUE(state.viewState.measures.empty());
    for (const CameraBookmark& bookmark : state.viewState.bookmarks) {
        EXPECT_FALSE(bookmark.defined);
    }
    EXPECT_TRUE(!state.viewState.unknownKeys.IsMap() || state.viewState.unknownKeys.size() == 0)
        << "display settings and the editor camera are known keys: " << YAML::Dump(state.viewState.unknownKeys);
}

TEST(Workspace, PreservesUnknownKeys) {
    const YAML::Node node = YAML::Load(
        "futureFeature:\n"
        "  level: 3\n"
        "  names: [a, b]\n"
        "cameraSettings:\n"
        "  speed: 25\n"
        "  futureSetting: true\n");
    const Workspace::SceneState state = Workspace::decodeSceneEntry(node);
    EXPECT_FLOAT_EQ(state.viewState.camera.speed, 25.0f);

    YAML::Node out;
    SceneViewStateStream::encode(state.viewState, out);
    ASSERT_TRUE(out["futureFeature"].IsMap()) << YAML::Dump(out);
    EXPECT_EQ(out["futureFeature"]["level"].as<int>(), 3);
    EXPECT_EQ(out["futureFeature"]["names"][1].as<std::string>(), "b");
    EXPECT_FLOAT_EQ(out["cameraSettings"]["speed"].as<float>(), 25.0f);
}

TEST(Workspace, DropsDeadEntityIds) {
    test::ensureEngine();
    Scene scene;
    const Entity alive = scene.createEntity();
    const Entity gone = scene.createEntity();
    scene.destroyEntity(gone);

    SceneProject sceneProject;
    sceneProject.scene = &scene;
    sceneProject.sceneType = SceneType::SCENE_3D;
    sceneProject.viewState.hiddenEntities = {alive, gone, 999999};
    sceneProject.viewState.pickLockedEntities = {gone};

    const YAML::Node node = Workspace::encodeSceneEntry(sceneProject);
    sceneProject.scene = nullptr;

    ASSERT_TRUE(node["hiddenEntities"].IsSequence()) << YAML::Dump(node);
    ASSERT_EQ(node["hiddenEntities"].size(), 1u);
    EXPECT_EQ(node["hiddenEntities"][0].as<Entity>(), alive);
    EXPECT_TRUE(!node["pickLockedEntities"].IsDefined() || node["pickLockedEntities"].size() == 0);
}

TEST(Workspace, RoundTripsViewState) {
    SceneViewState state;
    state.setLayout(SceneViewLayout::QUAD);
    state.activePane = 2;
    state.orthographic = true;
    state.orthoSize = 7.5f;
    state.panes[1].shading = PaneShading::SHADED_SHADOWS;
    state.panes[0].pose.position = Vector3(4, 5, 6);
    state.panes[0].posed = true;
    state.camera.fieldOfView = 45.0f;
    state.camera.easing = false;
    state.camera.speed = 3.5f;
    state.bookmarks[3].defined = true;
    state.bookmarks[3].name = "Gate";
    state.bookmarks[3].pose.position = Vector3(1, 2, 3);
    state.bookmarks[3].pose.orthographic = true;
    state.bookmarks[3].pose.orthoSize = 4.0f;
    state.measures.push_back({7, Vector3(0, 0, 0), Vector3(4, 0, 0)});
    state.nextMeasureId = 8;
    state.layerFilter.visible = 0xFFFFFFF0u;
    state.layerFilter.pickable = 0x0000000Fu;
    state.hiddenEntities = {3, 9};
    state.pickLockedEntities = {4};

    YAML::Node node;
    SceneViewStateStream::encode(state, node);
    const SceneViewState loaded = SceneViewStateStream::decode(YAML::Load(YAML::Dump(node)));

    EXPECT_EQ(loaded.layout, SceneViewLayout::QUAD);
    EXPECT_EQ(loaded.activePane, 2);
    EXPECT_TRUE(loaded.orthographic);
    EXPECT_FLOAT_EQ(loaded.orthoSize, 7.5f);
    EXPECT_EQ(loaded.panes[1].shading, PaneShading::SHADED_SHADOWS);
    EXPECT_EQ(loaded.panes[1].axis, SceneViewAxis::TOP);
    EXPECT_EQ(loaded.panes[2].axis, SceneViewAxis::FRONT);
    EXPECT_EQ(loaded.panes[3].axis, SceneViewAxis::RIGHT);
    EXPECT_TRUE(loaded.panes[0].posed) << "the free pane's pose is kept while another pane is active";
    EXPECT_FLOAT_EQ(loaded.panes[0].pose.position.y, 5.0f);
    EXPECT_FLOAT_EQ(loaded.camera.fieldOfView, 45.0f);
    EXPECT_FALSE(loaded.camera.easing);
    EXPECT_FLOAT_EQ(loaded.camera.speed, 3.5f);
    ASSERT_TRUE(loaded.bookmarks[3].defined);
    EXPECT_EQ(loaded.bookmarks[3].name, "Gate");
    EXPECT_FLOAT_EQ(loaded.bookmarks[3].pose.position.z, 3.0f);
    EXPECT_TRUE(loaded.bookmarks[3].pose.orthographic);
    EXPECT_FLOAT_EQ(loaded.bookmarks[3].pose.orthoSize, 4.0f);
    EXPECT_FALSE(loaded.bookmarks[2].defined);
    ASSERT_EQ(loaded.measures.size(), 1u);
    EXPECT_EQ(loaded.measures[0].id, 7u);
    EXPECT_FLOAT_EQ(loaded.measures[0].to.x, 4.0f);
    EXPECT_EQ(loaded.nextMeasureId, 8u);
    EXPECT_EQ(loaded.layerFilter.visible, 0xFFFFFFF0u);
    EXPECT_EQ(loaded.layerFilter.pickable, 0x0000000Fu);
    EXPECT_EQ(loaded.hiddenEntities, (std::set<Entity>{3, 9}));
    EXPECT_EQ(loaded.pickLockedEntities, (std::set<Entity>{4}));
}

// AC20: opening and saving a scene never touches its file (the view state is the workspace's)
TEST(Workspace, SceneFileBytesUnchanged) {
    test::ensureEngine();
    const std::filesystem::path directory = test::fixture("scenes");
    int checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".scene") {
            continue;
        }
        test::DecodedScene decoded = test::decodeSceneFile(entry.path());
        ASSERT_TRUE(decoded.scene) << entry.path();
        // what the Scene view keeps for it, as a user would leave it
        decoded.scene->viewState.setLayout(SceneViewLayout::QUAD);
        decoded.scene->viewState.bookmarks[1].defined = true;
        decoded.scene->viewState.measures.push_back({1, Vector3(0, 0, 0), Vector3(1, 0, 0)});
        decoded.scene->viewState.hiddenEntities.insert(1);
        const std::string saved = test::encodeSceneText(decoded);
        for (const char* key : {"cameraSettings", "bookmarks", "measures", "layerFilter", "hiddenEntities", "pickLockedEntities"}) {
            EXPECT_EQ(saved.find(std::string(key) + ":"), std::string::npos) << key << " written into " << entry.path();
        }
        test::DecodedScene again = test::decodeSceneText(saved, entry.path().filename().string());
        EXPECT_EQ(test::encodeSceneText(again), saved) << entry.path() << ": a second save differs";
        checked++;
    }
    EXPECT_GT(checked, 0);
}
