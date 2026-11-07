// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// ActionPreviewSnapshot (runtime-core spec 6.6): the authored state an edit-mode preview of
// actions restores. A command's write during the preview (here the ClipRecordHooks calls a
// command makes around its write) becomes authored: the restore keeps it, and before the write
// the value goes back to its authored one, so the command's undo never records a previewed pose.

#include <gtest/gtest.h>

#include "Catalog.h"
#include "Scene.h"
#include "animation/ActionPreviewSnapshot.h"
#include "animation/ClipRecordHooks.h"
#include "component/ActionComponent.h"
#include "component/Transform.h"
#include "component/KeyframeTracksComponent.h"
#include "component/TweenComponent.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr uint32_t kSceneId = 7;

    Entity transformEntity(Scene& scene) {
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, Transform());
        return entity;
    }

    Transform& transformOf(Scene& scene, Entity entity) {
        return scene.getComponent<Transform>(entity);
    }

    // a command writing one property: the hooks around the write, as PropertyCmd calls them
    template<typename Write>
    void commandWrite(Entity entity, ComponentType type, const std::string& property, Write write, uint32_t sceneId = kSceneId) {
        ClipRecordHooks::beforeComponentWrite(sceneId, entity, type, property);
        write();
        ClipRecordHooks::afterComponentWrite(sceneId, entity, type, property);
    }

}

TEST(ActionPreviewSnapshotTest, RestoreKeepsAWrittenProperty) {
    Scene scene;
    const Entity target = transformEntity(scene);
    ActionPreviewSnapshot snapshot;
    snapshot.add(kSceneId, &scene, target);

    transformOf(scene, target).position = Vector3(5, 0, 0);   // the preview's pose
    commandWrite(target, ComponentType::Transform, "scale", [&]() { transformOf(scene, target).scale = Vector3(2, 2, 2); });
    EXPECT_TRUE(snapshot.takeChanged());
    EXPECT_FALSE(snapshot.takeChanged());

    snapshot.restore(&scene);
    EXPECT_EQ(transformOf(scene, target).position, Vector3(0, 0, 0)) << "the preview's pose goes";
    EXPECT_EQ(transformOf(scene, target).scale, Vector3(2, 2, 2)) << "the edit made during the preview stays";
}

TEST(ActionPreviewSnapshotTest, BeforeWritePutsTheAuthoredValueBack) {
    Scene scene;
    const Entity target = transformEntity(scene);
    ActionPreviewSnapshot snapshot;
    snapshot.add(kSceneId, &scene, target);

    transformOf(scene, target).position = Vector3(5, 0, 0);
    Vector3 recorded;
    commandWrite(target, ComponentType::Transform, "position", [&]() {
        recorded = transformOf(scene, target).position;   // what a command keeps for its undo
        transformOf(scene, target).position = Vector3(1, 2, 3);
    });
    EXPECT_EQ(recorded, Vector3(0, 0, 0)) << "the command records the authored value, not the preview's";

    transformOf(scene, target).position = Vector3(9, 9, 9);   // the preview shows its pose again
    snapshot.restore(&scene);
    EXPECT_EQ(transformOf(scene, target).position, Vector3(1, 2, 3));
}

TEST(ActionPreviewSnapshotTest, WholeComponentWriteIsKept) {
    Scene scene;
    const Entity target = transformEntity(scene);
    ActionPreviewSnapshot snapshot;
    snapshot.add(kSceneId, &scene, target);

    transformOf(scene, target).position = Vector3(5, 0, 0);
    Vector3 recorded;
    commandWrite(target, ComponentType::Transform, "", [&]() {
        recorded = transformOf(scene, target).position;
        transformOf(scene, target).scale = Vector3(3, 3, 3);   // a gizmo scale drag writes the whole Transform
    });
    EXPECT_EQ(recorded, Vector3(0, 0, 0)) << "the whole component was put back before the write";

    transformOf(scene, target).position = Vector3(5, 0, 0);
    snapshot.restore(&scene);
    EXPECT_EQ(transformOf(scene, target).scale, Vector3(3, 3, 3));
    EXPECT_EQ(transformOf(scene, target).position, Vector3(0, 0, 0)) << "the preview's pose was not taken with it";
}

TEST(ActionPreviewSnapshotTest, LastWriteWins) {
    Scene scene;
    const Entity target = transformEntity(scene);
    ActionPreviewSnapshot snapshot;
    snapshot.add(kSceneId, &scene, target);

    commandWrite(target, ComponentType::Transform, "position", [&]() { transformOf(scene, target).position = Vector3(1, 0, 0); });
    commandWrite(target, ComponentType::Transform, "position", [&]() { transformOf(scene, target).position = Vector3(2, 0, 0); });
    // an undo writes the value back the same way
    commandWrite(target, ComponentType::Transform, "position", [&]() { transformOf(scene, target).position = Vector3(1, 0, 0); });

    transformOf(scene, target).position = Vector3(7, 7, 7);
    snapshot.restore(&scene);
    EXPECT_EQ(transformOf(scene, target).position, Vector3(1, 0, 0));
}

TEST(ActionPreviewSnapshotTest, SettingsPathWriteKeepsThePreviewRunning) {
    Scene scene;
    const Entity action = scene.createEntity();
    scene.addComponent<ActionComponent>(action, ActionComponent());
    TweenComponent tween;
    tween.duration = 1.0f;
    scene.addComponent<TweenComponent>(action, tween);
    ActionPreviewSnapshot snapshot;
    snapshot.add(kSceneId, &scene, action);

    // the preview runs it
    ActionComponent& live = scene.getComponent<ActionComponent>(action);
    live.state = ActionState::Running;
    live.timecount = 0.4f;
    commandWrite(action, ComponentType::TweenComponent, "duration", [&]() { scene.getComponent<TweenComponent>(action).duration = 3.0f; });
    commandWrite(action, ComponentType::ActionComponent, "speed", [&]() { scene.getComponent<ActionComponent>(action).speed = 2.0f; });
    EXPECT_EQ(scene.getComponent<ActionComponent>(action).state, ActionState::Running) << "a field write leaves the preview's state";
    EXPECT_FLOAT_EQ(scene.getComponent<ActionComponent>(action).timecount, 0.4f);

    snapshot.restore(&scene);
    EXPECT_EQ(scene.getComponent<ActionComponent>(action).state, ActionState::Stopped) << "the authored state comes back";
    EXPECT_FLOAT_EQ(scene.getComponent<ActionComponent>(action).speed, 2.0f) << "with the edits";
    EXPECT_FLOAT_EQ(scene.getComponent<TweenComponent>(action).duration, 3.0f);
}

TEST(ActionPreviewSnapshotTest, WritesOutsideTheSnapshotAreIgnored) {
    Scene scene;
    const Entity target = transformEntity(scene);
    const Entity other = transformEntity(scene);
    ActionPreviewSnapshot snapshot;
    snapshot.add(kSceneId, &scene, target);

    commandWrite(other, ComponentType::Transform, "position", [&]() { transformOf(scene, other).position = Vector3(4, 0, 0); });
    commandWrite(target, ComponentType::Transform, "position", [&]() { transformOf(scene, target).position = Vector3(4, 0, 0); },
                 kSceneId + 1);
    EXPECT_FALSE(snapshot.takeChanged()) << "another GameObject, another scene";

    snapshot.restore(&scene);
    EXPECT_EQ(transformOf(scene, target).position, Vector3(0, 0, 0));
    EXPECT_EQ(transformOf(scene, other).position, Vector3(4, 0, 0)) << "the restore leaves what it did not take";
}

TEST(ActionPreviewSnapshotTest, ClearStopsListening) {
    Scene scene;
    const Entity target = transformEntity(scene);
    {
        ActionPreviewSnapshot snapshot;
        snapshot.add(kSceneId, &scene, target);
        EXPECT_TRUE(snapshot.contains(target));
        snapshot.clear();
        EXPECT_FALSE(snapshot.contains(target));
        commandWrite(target, ComponentType::Transform, "position", [&]() { transformOf(scene, target).position = Vector3(4, 0, 0); });
        EXPECT_FALSE(snapshot.takeChanged());
        snapshot.add(kSceneId, &scene, target);
    }
    // the destroyed snapshot listens no more
    commandWrite(target, ComponentType::Transform, "position", [&]() { transformOf(scene, target).position = Vector3(5, 0, 0); });
    EXPECT_EQ(transformOf(scene, target).position, Vector3(5, 0, 0));
}

// the key data is left out of the snapshot (keys authored while a scrub preview is paused stay), but
// the playhead the preview moves in it (index, interpolation: saved fields) goes back
TEST(ActionPreviewSnapshotTest, KeyframePlayheadGoesBackAndKeysStay) {
    Scene scene;
    const Entity track = scene.createEntity();
    KeyframeTracksComponent keyframe;
    keyframe.times = {0.0f, 1.0f};
    scene.addComponent<KeyframeTracksComponent>(track, keyframe);
    ActionPreviewSnapshot snapshot;
    snapshot.add(kSceneId, &scene, track);

    KeyframeTracksComponent& live = scene.getComponent<KeyframeTracksComponent>(track);
    live.index = 1;                     // the preview sampled the second segment
    live.interpolation = 0.5f;
    live.times.push_back(2.0f);         // a key authored meanwhile
    snapshot.restore(&scene);

    EXPECT_EQ(live.index, 0) << "the previewed playhead is not saved";
    EXPECT_FLOAT_EQ(live.interpolation, 0.0f);
    EXPECT_EQ(live.times.size(), 3u) << "the key authored during the preview stays";
}
