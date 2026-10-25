// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pure helpers of the Inspector's animation clip sections (editor/window/InspectorWindowClips.h,
// animation clips spec 4.2, 9.R7/R8/R45): field texts, the Bindings foldout lines, the Model table
// labels with sub-clip names and the action preview's PreviewOwner roots.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/Transform.h"
#include "window/InspectorWindowClips.h"

#include <limits>

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

}

TEST(InspectorClipHelpers, FieldTextsNameTheFileOrTheSubClip) {
    EXPECT_TRUE(inspectorclips::isSubClip("models/hero.gltf#Walk"));
    EXPECT_FALSE(inspectorclips::isSubClip("anim/walk.anim"));
    EXPECT_EQ(inspectorclips::clipFieldText("anim/walk.anim"), "walk.anim");
    EXPECT_EQ(inspectorclips::clipFieldText("models/hero.gltf#Walk"), "Walk");
    // a '#' in the model's file name stays in the model part
    EXPECT_EQ(inspectorclips::clipFieldText("models/a#b.glb#Run 2"), "Run 2");
    EXPECT_EQ(inspectorclips::clipFieldText(""), "");
    EXPECT_EQ(inspectorclips::missingClipText("anim/walk.anim"), "Missing (walk.anim)");
    EXPECT_EQ(inspectorclips::missingClipText("models/hero.gltf#Walk"), "Missing (Walk in hero.gltf)");
}

TEST(InspectorClipHelpers, WrapLabelsAndSeconds) {
    EXPECT_STREQ(inspectorclips::wrapLabel(ClipWrapMode::Default), "Default");
    EXPECT_STREQ(inspectorclips::wrapLabel(ClipWrapMode::Once), "Once");
    EXPECT_STREQ(inspectorclips::wrapLabel(ClipWrapMode::Loop), "Loop");
    EXPECT_STREQ(inspectorclips::wrapLabel(ClipWrapMode::PingPong), "Ping Pong");
    EXPECT_STREQ(inspectorclips::wrapLabel(ClipWrapMode::ClampForever), "Clamp Forever");
    EXPECT_EQ(inspectorclips::formatSeconds(1.25f), "1.25 s");
    EXPECT_EQ(inspectorclips::formatSeconds(std::numeric_limits<float>::quiet_NaN()), "0.00 s");
}

TEST(InspectorClipHelpers, UsersTextSaysOpenScenes) {
    EXPECT_EQ(inspectorclips::usersText(0), "0 users in open scenes");
    EXPECT_EQ(inspectorclips::usersText(1), "1 user in open scenes");
    EXPECT_EQ(inspectorclips::usersText(3), "3 users in open scenes");
}

TEST(InspectorClipHelpers, AnUncheckedReportSaysUnchecked) {
    // what the contract's stub (and a report without a root) gives: every track Unchecked
    ClipValidationReport report;
    report.tracks.push_back(track("Arm", ClipChannel::Rotation, TrackBindingStatus::Unchecked));
    EventReport event;
    event.function = "footstep";
    report.events.push_back(event);
    EXPECT_FALSE(inspectorclips::reportChecked(report));
    EXPECT_EQ(inspectorclips::bindingsSummary(report), "Unchecked");
    EXPECT_FALSE(inspectorclips::trackHasProblem(report.tracks[0]));
}

TEST(InspectorClipHelpers, ACheckedReportCountsBoundTracks) {
    ClipValidationReport report;
    report.tracks.push_back(track("Arm", ClipChannel::Rotation, TrackBindingStatus::Ok));
    report.tracks.push_back(track("Arm/Hnd", ClipChannel::Position, TrackBindingStatus::MissingPath));
    report.resolved = 1;
    report.total = 2;
    EXPECT_TRUE(inspectorclips::reportChecked(report));
    EXPECT_EQ(inspectorclips::bindingsSummary(report), "1/2 bound");
    EXPECT_FALSE(inspectorclips::trackHasProblem(report.tracks[0]));
    EXPECT_TRUE(inspectorclips::trackHasProblem(report.tracks[1]));
    EXPECT_FALSE(inspectorclips::trackHasProblem(track("", ClipChannel::Property, TrackBindingStatus::NotSampled)));

    report.legacyModelLayout = true;
    EXPECT_EQ(inspectorclips::bindingsSummary(report), "legacy model layout");
}

TEST(InspectorClipHelpers, AMissingPathSuggestsTheNearestNames) {
    TrackReport missing = track("Arm/Hnd", ClipChannel::Position, TrackBindingStatus::MissingPath);
    EXPECT_EQ(inspectorclips::trackProblemText(missing), "Arm/Hnd : position: no GameObject at this path");
    missing.nearest = {"Arm/Hand"};
    EXPECT_EQ(inspectorclips::trackProblemText(missing), "Arm/Hnd : position: no GameObject at this path (did you mean Arm/Hand?)");
    missing.nearest = {"Arm/Hand", "Arm/Hand2", ""};
    EXPECT_EQ(inspectorclips::trackProblemText(missing),
              "Arm/Hnd : position: no GameObject at this path (did you mean Arm/Hand, Arm/Hand2 or (root)?)");
    // the validation's own line wins, and is not suggested twice
    missing.message = "no 'Hnd' under Arm (did you mean Arm/Hand?)";
    EXPECT_EQ(inspectorclips::trackProblemText(missing), "Arm/Hnd : position: no 'Hnd' under Arm (did you mean Arm/Hand?)");
    EXPECT_EQ(inspectorclips::trackProblemText(track("", ClipChannel::Morph, TrackBindingStatus::MissingComponent)),
              "(root) : morph: the GameObject has nothing this channel animates");
}

TEST(InspectorClipHelpers, EventTexts) {
    EventReport event;
    event.function = "footstep";
    event.status = EventHandlerStatus::NotFound;
    EXPECT_EQ(inspectorclips::eventProblemText(event, 0.5f), "Event footstep at 0.50 s: no script on the root defines footstep()");
    EXPECT_EQ(inspectorclips::eventArgsText({AnimationEvent::numberArg(1), AnimationEvent::stringArg("left"), AnimationEvent::boolArg(true)}),
              "1, \"left\", true");
    EXPECT_EQ(inspectorclips::eventArgsText({}), "");
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
