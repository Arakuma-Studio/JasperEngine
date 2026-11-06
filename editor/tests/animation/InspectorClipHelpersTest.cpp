// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The helpers of the Inspector's animation clip sections (editor/window/InspectorWindowClips.h,
// animation clips spec 4.2, 9.R7/R8/R30/R45): the Missing rule of sub-clips, eliding of file
// labels and track paths, the Bindings counts and rows, the Model table labels with sub-clip
// names, the inspected-object tracker (R30) and the action preview's PreviewOwner acquisition
// against the registry (R7).

#include <gtest/gtest.h>

#include "Scene.h"
#include "animation/PreviewOwner.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/Transform.h"
#include "window/InspectorWindowClips.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    Entity createChild(Scene& scene, Entity parent, const std::string& name) {
        Entity entity = scene.createEntity();
        scene.setEntityName(entity, name);
        scene.addComponent<Transform>(entity, Transform());
        if (parent != NULL_ENTITY) {
            scene.addEntityChild(parent, entity, false);
        }
        return entity;
    }

    TrackReport track(const std::string& path, ClipChannel channel, TrackBindingStatus status) {
        TrackReport report;
        report.path = path;
        report.channel = channel;
        report.status = status;
        return report;
    }

    // one character = one unit wide
    float chars(const std::string& text) {
        return (float)text.size();
    }

}

TEST(InspectorClipHelpers, FieldTextsNameTheFileOrTheSubClip) {
    EXPECT_TRUE(inspectorclips::isSubClip("models/hero.gltf#Walk"));
    EXPECT_FALSE(inspectorclips::isSubClip("anim/walk.anim"));
    EXPECT_EQ(inspectorclips::clipFieldText("anim/walk.anim"), "walk.anim");
    EXPECT_EQ(inspectorclips::clipFieldText("models/hero.gltf#Walk"), "Walk");
    // a '#' in the model's file name stays in the model part
    EXPECT_EQ(inspectorclips::clipFieldText("models/a#b.glb#Run 2"), "Run 2");
    EXPECT_EQ(inspectorclips::clipFieldText(""), "");
    EXPECT_NE(inspectorclips::missingClipTooltip("anim/walk.anim").find("anim/walk.anim"), std::string::npos);
    EXPECT_NE(inspectorclips::missingClipTooltip("models/hero.gltf#Walk").find("has no animation \"Walk\""), std::string::npos);
}

TEST(InspectorClipHelpers, ASubClipIsMissingWhenTheModelNoLongerNamesIt) {
    const std::vector<std::string> names = {"Walk", "Idle", "Wave", "Wave 2"};
    EXPECT_TRUE(inspectorclips::subClipNamed(names, "Walk"));
    EXPECT_TRUE(inspectorclips::subClipNamed(names, "Wave 2"));
    // renamed or dropped in a re-export
    EXPECT_FALSE(inspectorclips::subClipNamed(names, "Run"));
    // the index form findClipUsers and the pool accept
    EXPECT_TRUE(inspectorclips::subClipNamed(names, "3"));
    EXPECT_FALSE(inspectorclips::subClipNamed(names, "4"));
    EXPECT_FALSE(inspectorclips::subClipNamed(names, ""));
    EXPECT_FALSE(inspectorclips::subClipNamed(names, "12345678901"));
}

TEST(InspectorClipHelpers, LabelsElideKeepingTheFileNameEnd) {
    EXPECT_EQ(inspectorclips::elideMiddle("walk.anim", 20, chars), "walk.anim");
    const std::string elided = inspectorclips::elideMiddle("hero_wave_left_arm.anim", 14, chars);
    EXPECT_LE(elided.size(), 14u);
    EXPECT_NE(elided.find("..."), std::string::npos);
    EXPECT_EQ(elided.substr(elided.size() - 5), ".anim") << elided;
    EXPECT_EQ(elided.substr(0, 1), "h") << elided;
    // a path keeps its end (the leaf and the channel)
    const std::string path = inspectorclips::elideStart("Hero/mixamorig:Hips/mixamorig:LeftUpLeg : Rotation", 25, chars);
    EXPECT_EQ(path, "...g:LeftUpLeg : Rotation");
    EXPECT_LE(path.size(), 25u);
    // never splits a UTF-8 sequence
    const std::string utf8 = inspectorclips::elideMiddle("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9.anim", 12, chars);
    for (size_t i = 0; i + 1 < utf8.size(); i++) {
        if ((unsigned char)utf8[i] == 0xc3) {
            EXPECT_EQ((unsigned char)utf8[i + 1], 0xa9) << utf8;
        }
    }
    EXPECT_EQ(inspectorclips::elideMiddle("abcdef", 2, chars), "...");
}

TEST(InspectorClipHelpers, AnUncheckedReportSaysUnchecked) {
    // a report without a root: every track Unchecked
    ClipValidationReport report;
    report.tracks.push_back(track("Arm", ClipChannel::Rotation, TrackBindingStatus::Unchecked));
    EventReport event;
    event.function = "footstep";
    report.events.push_back(event);
    EXPECT_FALSE(inspectorclips::reportChecked(report));
    EXPECT_EQ(inspectorclips::bindingsSummary(report), "Unchecked");
    EXPECT_EQ(inspectorclips::problemCount(report), 0u);
}

TEST(InspectorClipHelpers, TheBindingsHeaderCountsProblems) {
    ClipValidationReport report;
    report.tracks.push_back(track("Arm", ClipChannel::Rotation, TrackBindingStatus::Ok));
    report.tracks.push_back(track("Arm/Hnd", ClipChannel::Position, TrackBindingStatus::MissingPath));
    report.tracks.push_back(track("", ClipChannel::Property, TrackBindingStatus::NotSampled));
    EventReport event;
    event.function = "footstep";
    event.status = EventHandlerStatus::NotFound;
    report.events.push_back(event);
    report.resolved = 1;
    report.total = 2;
    EXPECT_TRUE(inspectorclips::reportChecked(report));
    EXPECT_EQ(inspectorclips::bindingsSummary(report), "1/2 bound");
    EXPECT_EQ(inspectorclips::problemCount(report), 2u) << "the missing path and the handler";

    // the legacy layout counts once, not per track it hides
    report.legacyModelLayout = true;
    report.tracks.push_back(track("Hero/Hips", ClipChannel::Rotation, TrackBindingStatus::LegacyModelLayout));
    report.tracks.push_back(track("Hero/Hips/Spine", ClipChannel::Rotation, TrackBindingStatus::LegacyModelLayout));
    EXPECT_EQ(inspectorclips::problemCount(report), 3u);
    EXPECT_EQ(inspectorclips::bindingsSummary(report), "legacy model layout");
}

TEST(InspectorClipHelpers, ModelRowsShowSubClipNames) {
    // two glTF animations named "Wave": the loader's entities are "Wave", "Wave"; the sub-clips
    // "Wave", "Wave 2" (ModelSubClips naming)
    const std::vector<std::string> names = {"Wave", "Wave 2", "Animation 2"};
    EXPECT_EQ(inspectorclips::modelAnimationRowLabel("Wave", names, 0, "Animation 0"), "Wave");
    EXPECT_EQ(inspectorclips::modelAnimationRowLabel("Wave", names, 1, "Animation 1"), "Wave 2");
    // a renamed entity, or a row the user added, keeps its own name
    EXPECT_EQ(inspectorclips::modelAnimationRowLabel("Hello", names, 1, "Animation 1"), "Hello");
    EXPECT_EQ(inspectorclips::modelAnimationRowLabel("Wave", names, 5, "Animation 5"), "Wave");
    EXPECT_EQ(inspectorclips::modelAnimationRowLabel("", names, 2, "Animation 2"), "Animation 2");
    // "Wave 2b" is not a duplicate suffix
    EXPECT_EQ(inspectorclips::modelAnimationRowLabel("Wave", {"Wave 2b"}, 0, "Animation 0"), "Wave");
}

TEST(InspectorClipHelpers, PreviewRootsAreTheTopLevelEntities) {
    Scene scene;
    Entity hero = createChild(scene, NULL_ENTITY, "Hero");
    Entity hips = createChild(scene, hero, "Hips");
    Entity arm = createChild(scene, hips, "Arm");
    Entity prop = createChild(scene, NULL_ENTITY, "Prop");
    Entity animation = createChild(scene, NULL_ENTITY, "Walk");

    // the animation entity, bones under the hero (and the hero) and an unrelated prop
    const std::vector<Entity> roots = inspectorclips::topLevelEntities(&scene, {animation, arm, hips, prop, hero, arm});
    EXPECT_EQ(roots, (std::vector<Entity>{animation, prop, hero}));

    // without their common ancestor, siblings and cousins are separate roots
    EXPECT_EQ(inspectorclips::topLevelEntities(&scene, {arm, prop}), (std::vector<Entity>{arm, prop}));
    // destroyed and null entities are dropped
    scene.destroyEntity(prop);
    EXPECT_EQ(inspectorclips::topLevelEntities(&scene, {NULL_ENTITY, prop, arm}), (std::vector<Entity>{arm}));
    EXPECT_TRUE(inspectorclips::topLevelEntities(nullptr, {arm}).empty());
}

// R7 against the PreviewOwner registry: the Inspector's action preview of an animation whose
// tracks move the hero's bones stops the hero's other owner (the Animation window) before it
// snapshots, and is itself stopped (and restores) when another owner takes the hero
TEST(InspectorClipHelpers, TheActionPreviewOwnsWhatItWritesBeforeItsSnapshot) {
    const uint32_t sceneId = 7101;
    Scene scene;
    Entity hero = createChild(scene, NULL_ENTITY, "Hero");
    Entity hips = createChild(scene, hero, "Hips");
    Entity arm = createChild(scene, hips, "Arm");
    // an entity Animation (not under the hero) with two tracks targeting bones
    Entity walk = createChild(scene, NULL_ENTITY, "Walk");
    Entity hipsTrack = createChild(scene, walk, "Hips track");
    Entity armTrack = createChild(scene, walk, "Arm track");
    ActionComponent hipsAction;
    hipsAction.target = hips;
    scene.addComponent<ActionComponent>(hipsTrack, hipsAction);
    ActionComponent armAction;
    armAction.target = arm;
    scene.addComponent<ActionComponent>(armTrack, armAction);
    ActionComponent walkAction;
    scene.addComponent<ActionComponent>(walk, walkAction);
    AnimationComponent animation;
    animation.actions = {ActionFrame{0.0f, 1.0f, hipsTrack, 0}, ActionFrame{0.0f, 1.0f, armTrack, 1}};
    scene.addComponent<AnimationComponent>(walk, animation);

    EXPECT_EQ(inspectorclips::actionPreviewEntities(&scene, walk), (std::vector<Entity>{walk, hipsTrack, hips, armTrack, arm}));

    int windowStops = 0;
    const PreviewOwner::Token window = PreviewOwner::acquire(sceneId, hero, &scene, "Animation window", [&]() { windowStops++; });
    ASSERT_GT(window, 0);

    int inspectorStops = 0;
    const std::vector<int> tokens = inspectorclips::acquirePreviewOwners(sceneId, &scene, walk, [&]() { inspectorStops++; });
    // roots: the Animation entity (its tracks are under it) and the hips (the arm is under it)
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(windowStops, 1) << "the hero's owner stops before the Inspector snapshots";
    EXPECT_FALSE(PreviewOwner::owns(window));
    for (int token : tokens) EXPECT_TRUE(PreviewOwner::owns(token));
    EXPECT_EQ(PreviewOwner::ownerOf(sceneId, hips).name, "Inspector action preview");
    EXPECT_EQ(inspectorStops, 0) << "its own roots don't stop one another";

    // the Pose window takes the hero: the Inspector preview (on the hips) stops and restores
    const PreviewOwner::Token pose = PreviewOwner::acquire(sceneId, hero, &scene, "Pose Animation", []() {});
    EXPECT_EQ(inspectorStops, 1);
    EXPECT_FALSE(PreviewOwner::owns(tokens[1]));
    // what the Inspector's stop does with its tokens: released (a stale one is a no-op)
    for (int token : tokens) PreviewOwner::release(token);
    for (int token : tokens) EXPECT_FALSE(PreviewOwner::owns(token));
    EXPECT_TRUE(PreviewOwner::owns(pose));
    PreviewOwner::release(pose);
    PreviewOwner::releaseScene(sceneId);
}

// R30: the Project window sets the asset on a click; a programmatic selection change after it
// shows the selection again; an asset set in the same frame as a selection change wins
TEST(InspectorClipHelpers, TheInspectedObjectFollowsTheLatestChoice) {
    inspectorclips::InspectedObjectTracker tracker;
    const std::filesystem::path clip = "/project/assets/anim/walk.anim";
    const std::vector<Entity> hero = {5};
    const std::vector<Entity> robot = {6};

    // the first frame never clears
    EXPECT_FALSE(tracker.update(clip, 1, hero).clearAsset);
    // nothing changes: the asset stays
    auto result = tracker.update(clip, 1, hero);
    EXPECT_FALSE(result.clearAsset);
    EXPECT_FALSE(result.changed);
    // MCP select_entities (or an undo) changes the selection: the selection shows
    result = tracker.update(clip, 1, robot);
    EXPECT_TRUE(result.clearAsset);
    EXPECT_TRUE(result.changed);
    // the caller cleared it; the next frame sees no asset
    EXPECT_FALSE(tracker.update({}, 1, robot).clearAsset);
    // a Project click in the same frame as a selection change: the asset is the latest choice
    result = tracker.update(clip, 1, hero);
    EXPECT_FALSE(result.clearAsset);
    EXPECT_TRUE(result.changed);
    // another scene selected: a selection change too
    EXPECT_TRUE(tracker.update(clip, 2, hero).clearAsset);
    // a new asset replaces the old one without clearing
    tracker.update({}, 2, hero);
    tracker.update(clip, 2, hero);
    result = tracker.update("/project/assets/anim/run.anim", 2, hero);
    EXPECT_FALSE(result.clearAsset);
    EXPECT_TRUE(result.changed);
}
