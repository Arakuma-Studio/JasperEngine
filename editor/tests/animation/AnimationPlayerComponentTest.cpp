// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation Player component's editor plumbing (animation clips spec 3.2, 3.6, 9.R8/R14/R22/
// R25/R26/R41): Catalog properties, Stream encode/decode (path hint + GUID per clip entry), prefab
// overrides of the clip list, copy/paste, ProjectUtils add/remove, and the C++ the exporter emits
// (compiled here as written). What needs a project on disk (McpValues' clip validation, the asset
// path visitor, the scene resources) is in AnimationPlayerProjectTest.cpp.

#include <gtest/gtest.h>
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "animation/ClipTypes.h"
#include "component/AnimationPlayerComponent.h"

#include "Catalog.h"
#include "Factory.h"
#include "Project.h"
#include "Scene.h"
#include "Stream.h"
#include "project/ProjectPrefabInternal.h"
#include "util/EntityClipboard.h"
#include "util/ProjectUtils.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const char* const WALK_GUID = "0123456789abcdef0123456789abcdef";

    AnimationPlayerClip clipEntry(const std::string& clip, const std::string& guid = "", const std::string& name = "",
                                  ClipWrapMode wrap = ClipWrapMode::Default) {
        AnimationPlayerClip entry;
        entry.clip = clip;
        entry.guid = guid;
        entry.name = name;
        entry.wrap = wrap;
        return entry;
    }

    // every authored field away from its default
    AnimationPlayerComponent authoredPlayer() {
        AnimationPlayerComponent player;
        player.enabled = false;
        player.clips = {clipEntry("anim/walk.anim", WALK_GUID, "Walk", ClipWrapMode::Loop),
                        clipEntry("models/hero.gltf#Idle"),
                        clipEntry("anim/jump.anim", "", "", ClipWrapMode::ClampForever)};
        player.defaultClip = "Walk";
        player.playAutomatically = false;
        player.speed = 1.5f;
        player.defaultFadeTime = 0.4f;
        return player;
    }

    void expectSameAuthored(const AnimationPlayerComponent& a, const AnimationPlayerComponent& b) {
        EXPECT_EQ(a.enabled, b.enabled);
        EXPECT_EQ(a.clips, b.clips);
        EXPECT_EQ(a.defaultClip, b.defaultClip);
        EXPECT_EQ(a.playAutomatically, b.playAutomatically);
        EXPECT_FLOAT_EQ(a.speed, b.speed);
        EXPECT_FLOAT_EQ(a.defaultFadeTime, b.defaultFadeTime);
    }

    Entity playerEntity(Scene& scene, const AnimationPlayerComponent& player) {
        Entity entity = scene.createUserEntity();
        scene.addComponent<Transform>(entity, {});
        scene.addComponent<AnimationPlayerComponent>(entity, player);
        return entity;
    }

}

// ---- Stream --------------------------------------------------------------------------------

TEST(AnimationPlayerComponent, StreamRoundTripKeepsEveryAuthoredField) {
    const AnimationPlayerComponent player = authoredPlayer();
    const YAML::Node node = Stream::encodeAnimationPlayerComponent(player);

    // the entry keys only when set (a sub-clip has no GUID); the wrap by its file name
    ASSERT_TRUE(node["clips"].IsSequence());
    ASSERT_EQ(node["clips"].size(), 3u);
    EXPECT_EQ(node["clips"][0]["clip"].as<std::string>(), "anim/walk.anim");
    EXPECT_EQ(node["clips"][0]["guid"].as<std::string>(), WALK_GUID);
    EXPECT_EQ(node["clips"][0]["name"].as<std::string>(), "Walk");
    EXPECT_EQ(node["clips"][0]["wrap"].as<std::string>(), "loop");
    EXPECT_FALSE(node["clips"][1]["guid"]);
    EXPECT_FALSE(node["clips"][1]["name"]);
    EXPECT_FALSE(node["clips"][1]["wrap"]);
    EXPECT_EQ(node["clips"][2]["wrap"].as<std::string>(), "clamp_forever");

    const AnimationPlayerComponent decoded = Stream::decodeAnimationPlayerComponent(node);
    expectSameAuthored(decoded, player);
    EXPECT_TRUE(decoded.needsLoad);
    EXPECT_FALSE(decoded.started);
    EXPECT_FALSE(decoded.runtime);
    // a second trip writes the same text (scene files stay stable)
    EXPECT_EQ(YAML::Dump(Stream::encodeAnimationPlayerComponent(decoded)), YAML::Dump(node));
}

TEST(AnimationPlayerComponent, StreamDefaultsAndLenientEntries) {
    // a missing key is the default; a plain path is {clip}; an unknown wrap reads as default
    const YAML::Node node = YAML::Load(
        "clips:\n"
        "  - anim/walk.anim\n"
        "  - {clip: anim/run.anim, wrap: sideways}\n"
        "  - 42.5\n"
        "  - [not, an, entry]\n");
    const AnimationPlayerComponent decoded = Stream::decodeAnimationPlayerComponent(node);
    const AnimationPlayerComponent defaults;
    ASSERT_EQ(decoded.clips.size(), 3u);
    EXPECT_EQ(decoded.clips[0], clipEntry("anim/walk.anim"));
    EXPECT_EQ(decoded.clips[1], clipEntry("anim/run.anim"));
    EXPECT_EQ(decoded.clips[2].clip, "42.5");
    EXPECT_EQ(decoded.enabled, defaults.enabled);
    EXPECT_EQ(decoded.defaultClip, "");
    EXPECT_EQ(decoded.playAutomatically, defaults.playAutomatically);
    EXPECT_FLOAT_EQ(decoded.speed, defaults.speed);
    EXPECT_FLOAT_EQ(decoded.defaultFadeTime, defaults.defaultFadeTime);

    // a fade below zero is none
    const AnimationPlayerComponent negative = Stream::decodeAnimationPlayerComponent(YAML::Load("defaultFadeTime: -1\n"));
    EXPECT_FLOAT_EQ(negative.defaultFadeTime, 0.0f);

    // values of the wrong shape are the defaults (one bad key never fails a scene's load); a map
    // entry whose clip is not a path is left out
    AnimationPlayerComponent shapes;
    ASSERT_NO_THROW(shapes = Stream::decodeAnimationPlayerComponent(YAML::Load(
        "enabled: maybe\n"
        "playAutomatically: [1, 2]\n"
        "defaultClip: {a: b}\n"
        "clips:\n"
        "  - {clip: [anim/x.anim]}\n"
        "  - {clip: anim/y.anim, wrap: [loop]}\n")));
    EXPECT_EQ(shapes.enabled, defaults.enabled);
    EXPECT_EQ(shapes.playAutomatically, defaults.playAutomatically);
    EXPECT_EQ(shapes.defaultClip, "");
    ASSERT_EQ(shapes.clips.size(), 1u);
    EXPECT_EQ(shapes.clips[0], clipEntry("anim/y.anim"));
    // yaml-cpp's other spellings of a bool still read
    EXPECT_FALSE(Stream::decodeAnimationPlayerComponent(YAML::Load("enabled: no\n")).enabled);

    // an empty player writes an empty list: decoding it over a player with clips clears them
    const YAML::Node empty = Stream::encodeAnimationPlayerComponent(AnimationPlayerComponent());
    ASSERT_TRUE(empty["clips"].IsSequence());
    EXPECT_EQ(empty["clips"].size(), 0u);
    const AnimationPlayerComponent old = authoredPlayer();
    EXPECT_TRUE(Stream::decodeAnimationPlayerComponent(empty, &old).clips.empty());
}

TEST(AnimationPlayerComponent, DecodingOverAPlayerKeepsItsRunAndClearsCallbacks) {
    AnimationPlayerComponent old = authoredPlayer();
    old.needsLoad = false;
    old.started = true;
    int calls = 0;
    old.onFinish.add("script", std::function<void(const std::string&)>([&calls](const std::string&) { calls++; }));
    ASSERT_TRUE(old.onFinish.hasSubscribers());

    // the same clips: what it plays stays (paste, play-stop restore), the callbacks go
    AnimationPlayerComponent same = Stream::decodeAnimationPlayerComponent(Stream::encodeAnimationPlayerComponent(old), &old);
    EXPECT_FALSE(same.needsLoad);
    EXPECT_TRUE(same.started);
    EXPECT_FALSE(same.onFinish.hasSubscribers());
    EXPECT_FALSE(same.onEvent.hasSubscribers());

    // another clip list loads again
    AnimationPlayerComponent changed = old;
    changed.clips[1].wrap = ClipWrapMode::Loop;
    AnimationPlayerComponent reloaded = Stream::decodeAnimationPlayerComponent(Stream::encodeAnimationPlayerComponent(changed), &old);
    EXPECT_TRUE(reloaded.needsLoad);
    EXPECT_EQ(reloaded.clips[1].wrap, ClipWrapMode::Loop);
    EXPECT_EQ(calls, 0);
}

TEST(AnimationPlayerComponent, SceneComponentsEncodeAndDecode) {
    Scene scene;
    const Entity entity = playerEntity(scene, authoredPlayer());
    const YAML::Node components = Stream::encodeComponents(entity, &scene, scene.getSignature(entity));
    ASSERT_TRUE(components["animationplayer"]);

    Scene other;
    const Entity copy = other.createUserEntity();
    Stream::decodeComponents(copy, NULL_ENTITY, &other, components);
    ASSERT_TRUE(other.findComponent<AnimationPlayerComponent>(copy));
    expectSameAuthored(other.getComponent<AnimationPlayerComponent>(copy), authoredPlayer());
    const std::vector<ComponentType> found = Catalog::findComponents(&other, copy);
    EXPECT_NE(std::find(found.begin(), found.end(), ComponentType::AnimationPlayerComponent), found.end());
}

// ---- Catalog -------------------------------------------------------------------------------

TEST(AnimationPlayerComponent, CatalogProperties) {
    Scene scene;
    const Entity entity = playerEntity(scene, authoredPlayer());
    const ComponentType type = ComponentType::AnimationPlayerComponent;
    AnimationPlayerComponent& player = scene.getComponent<AnimationPlayerComponent>(entity);

    EXPECT_TRUE(Catalog::hasEnabledProperty(type));
    PropertyData enabled = Catalog::findProperty(&scene, entity, type, "enabled");
    ASSERT_EQ(enabled.type, PropertyType::Bool);
    EXPECT_EQ(enabled.ref, &player.enabled);

    const PropertyData clips = Catalog::findProperty(&scene, entity, type, "clips");
    EXPECT_EQ(clips.type, PropertyType::Custom);
    EXPECT_EQ(clips.ref, &player.clips);
    EXPECT_TRUE(clips.updateFlags & UpdateFlags_AnimationPlayer_Reload);

    const PropertyData clip = Catalog::findProperty(&scene, entity, type, "clips[1].clip");
    EXPECT_EQ(clip.type, PropertyType::String);
    EXPECT_EQ(clip.ref, &player.clips[1].clip);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "clips[0].guid").ref, &player.clips[0].guid);
    EXPECT_EQ(Catalog::findProperty(&scene, entity, type, "clips[0].name").ref, &player.clips[0].name);
    // a one-byte enum: Custom, never read as an int
    const PropertyData wrap = Catalog::findProperty(&scene, entity, type, "clips[2].wrap");
    EXPECT_EQ(wrap.type, PropertyType::Custom);
    EXPECT_EQ(wrap.ref, &player.clips[2].wrap);
    EXPECT_FALSE(Catalog::findProperty(&scene, entity, type, "clips[3].clip").ref);
    EXPECT_FALSE(Catalog::findProperty(&scene, entity, type, "clips[x].clip").ref);
    // one spelling per index, and no index that wraps around to an entry
    EXPECT_FALSE(Catalog::findProperty(&scene, entity, type, "clips[00].clip").ref);
    EXPECT_FALSE(Catalog::findProperty(&scene, entity, type, "clips[18446744073709551616].clip").ref);
    EXPECT_FALSE(Catalog::findProperty(&scene, entity, type, "clips[4294967296].clip").ref);
    EXPECT_FALSE(Catalog::findProperty(&scene, entity, type, "clips[0].other").ref);

    for (const char* name : {"defaultClip", "playAutomatically", "speed", "defaultFadeTime"}) {
        EXPECT_TRUE(Catalog::findProperty(&scene, entity, type, name).ref) << name;
    }

    const std::map<std::string, PropertyData> all = Catalog::findEntityProperties(&scene, entity, type);
    for (const char* name : {"clips", "clips[0].clip", "clips[2].wrap", "defaultClip", "speed", "enabled"}) {
        EXPECT_TRUE(all.count(name)) << name;
    }
    // the templates, without a component
    const std::map<std::string, PropertyData> templates = Catalog::getProperties(type, nullptr);
    EXPECT_TRUE(templates.count("clips[].clip"));
    EXPECT_TRUE(templates.count("clips[].wrap"));

    // a clip list edit makes the player load and rebind
    player.needsLoad = false;
    Catalog::updateEntity(&scene, entity, UpdateFlags_AnimationPlayer_Reload);
    EXPECT_TRUE(player.needsLoad);
}

TEST(AnimationPlayerComponent, CatalogCopiesTheCustomProperties) {
    Scene scene;
    const Entity source = playerEntity(scene, authoredPlayer());
    const Entity target = playerEntity(scene, AnimationPlayerComponent());
    const ComponentType type = ComponentType::AnimationPlayerComponent;

    // (a prefab revert copies values this way: the player reloads, through the property's flag)
    scene.getComponent<AnimationPlayerComponent>(target).needsLoad = false;
    Catalog::copyPropertyValue(&scene, source, &scene, target, type, "clips");
    EXPECT_EQ(scene.getComponent<AnimationPlayerComponent>(target).clips, authoredPlayer().clips);
    EXPECT_TRUE(scene.getComponent<AnimationPlayerComponent>(target).needsLoad);

    scene.getComponent<AnimationPlayerComponent>(source).clips[0].wrap = ClipWrapMode::PingPong;
    scene.getComponent<AnimationPlayerComponent>(target).needsLoad = false;
    Catalog::copyPropertyValue(&scene, source, &scene, target, type, "clips[0].wrap");
    EXPECT_EQ(scene.getComponent<AnimationPlayerComponent>(target).clips[0].wrap, ClipWrapMode::PingPong);
    EXPECT_TRUE(scene.getComponent<AnimationPlayerComponent>(target).needsLoad);

    // the whole component (Paste Component Values): decoded over the target
    const Entity third = playerEntity(scene, AnimationPlayerComponent());
    Catalog::copyComponent(&scene, source, &scene, third, type);
    expectSameAuthored(scene.getComponent<AnimationPlayerComponent>(third), scene.getComponent<AnimationPlayerComponent>(source));
}

// ---- prefab overrides ----------------------------------------------------------------------

TEST(AnimationPlayerComponent, PrefabOverrideOfTheClipList) {
    const ComponentType type = ComponentType::AnimationPlayerComponent;
    Scene asset;
    AnimationPlayerComponent assetPlayer;
    assetPlayer.clips = {clipEntry("anim/walk.anim", WALK_GUID)};
    const Entity assetRoot = playerEntity(asset, assetPlayer);

    // a scalar is its own override; the list as it was is none
    Scene instance;
    AnimationPlayerComponent faster = assetPlayer;
    faster.speed = 2.0f;
    const Entity fasterRoot = playerEntity(instance, faster);
    EXPECT_EQ(prefabinternal::diffComponentProperties(&asset, assetRoot, &instance, fasterRoot, type),
              std::vector<std::string>{"speed"});

    // another list: the whole list is overridden
    AnimationPlayerComponent more = assetPlayer;
    more.clips.push_back(clipEntry("models/hero.gltf#Run", "", "Run", ClipWrapMode::Loop));
    const Entity moreRoot = playerEntity(instance, more);
    // (and the new entry's own fields, which the prefab does not have)
    EXPECT_EQ(prefabinternal::diffComponentProperties(&asset, assetRoot, &instance, moreRoot, type),
              (std::vector<std::string>{"clips", "clips[1].clip", "clips[1].footIK", "clips[1].guid", "clips[1].layer", "clips[1].mirror",
                                        "clips[1].name", "clips[1].retarget", "clips[1].wrap"}));

    // one entry's wrap: the list and that field (the Overrides list shows both, as for other lists)
    AnimationPlayerComponent looping = assetPlayer;
    looping.clips[0].wrap = ClipWrapMode::Loop;
    const Entity loopingRoot = playerEntity(instance, looping);
    EXPECT_EQ(prefabinternal::diffComponentProperties(&asset, assetRoot, &instance, loopingRoot, type),
              (std::vector<std::string>{"clips", "clips[0].wrap"}));

    // applied as the instance loads: the prefab's player with the instance's overridden list
    prefabinternal::ScratchEntity values;
    values.takeComponents(&instance, moreRoot, Catalog::componentTypeToSignature(&instance, type));
    Scene loaded;
    const Entity loadedRoot = playerEntity(loaded, assetPlayer);
    std::set<std::string> names{"clips"};
    prefabinternal::copyProperties(values, &loaded, loadedRoot, type, names, "test");
    EXPECT_EQ(loaded.getComponent<AnimationPlayerComponent>(loadedRoot).clips, more.clips);
    EXPECT_FLOAT_EQ(loaded.getComponent<AnimationPlayerComponent>(loadedRoot).speed, assetPlayer.speed);
}

// ---- copy / paste --------------------------------------------------------------------------

TEST(AnimationPlayerComponent, CopyAndPasteComponentValues) {
    Scene scene;
    const Entity source = playerEntity(scene, authoredPlayer());
    AnimationPlayerComponent other;
    other.clips = {clipEntry("anim/a.anim"), clipEntry("anim/b.anim"), clipEntry("anim/c.anim"), clipEntry("anim/d.anim")};
    other.defaultClip = "c";
    const Entity target = playerEntity(scene, other);

    const YAML::Node copied = EntityClipboard::encodeComponent(&scene, source, ComponentType::AnimationPlayerComponent);
    EntityClipboard::applyComponentValue(&scene, target, ComponentType::AnimationPlayerComponent, copied);
    expectSameAuthored(scene.getComponent<AnimationPlayerComponent>(target), authoredPlayer());

    // Add Component gives the defaults
    const AnimationPlayerComponent added = Stream::decodeAnimationPlayerComponent(
        EntityClipboard::encodeDefaultComponent(ComponentType::AnimationPlayerComponent));
    expectSameAuthored(added, AnimationPlayerComponent());
}

// ---- ProjectUtils --------------------------------------------------------------------------

TEST(AnimationPlayerComponent, ProjectUtilsAddsAndRemoves) {
    Scene scene;
    std::vector<Entity> entities;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    entities.push_back(entity);
    const ComponentType type = ComponentType::AnimationPlayerComponent;

    ProjectUtils::addEntityComponent(&scene, entity, type, entities);
    ASSERT_TRUE(scene.findComponent<AnimationPlayerComponent>(entity));
    expectSameAuthored(scene.getComponent<AnimationPlayerComponent>(entity), AnimationPlayerComponent());

    const YAML::Node removed = ProjectUtils::removeEntityComponent(&scene, entity, type, entities, true);
    EXPECT_FALSE(scene.findComponent<AnimationPlayerComponent>(entity));

    EXPECT_TRUE(removed["clips"].IsSequence());

    // undo of a removal: added back from its encode
    ProjectUtils::addEntityComponent(&scene, entity, type, entities, Stream::encodeAnimationPlayerComponent(authoredPlayer()));
    ASSERT_TRUE(scene.findComponent<AnimationPlayerComponent>(entity));
    expectSameAuthored(scene.getComponent<AnimationPlayerComponent>(entity), authoredPlayer());
}

// ---- GUID references -----------------------------------------------------------------------

TEST(AnimationPlayerComponent, ClipReferencesNeedAProjectToResolve) {
    Scene scene;
    const Entity entity = playerEntity(scene, authoredPlayer());
    // no project open (no AssetDatabase): nothing changes (Project::resolveRegistryClipRefs, the
    // one resolver; AnimationClipAssetsTest covers it with a project)
    Project project;
    EXPECT_FALSE(project.resolveRegistryClipRefs(&scene));
    EXPECT_EQ(scene.getComponent<AnimationPlayerComponent>(entity).clips, authoredPlayer().clips);
}

// ---- C++ export ----------------------------------------------------------------------------

namespace {

    // the code below is Factory's output for authoredPlayer(), pasted: it must keep compiling
    // against the engine headers and build the same component. It and EMITTED_ANIMATION_PLAYER
    // are one text: change them together (FactoryEmitsTheAuthoredFields compares the emitted code
    // with the string; a real export compile runs in the MCP build_project checks)
    void emittedAnimationPlayer(Scene* scene, Entity entity) {
        AnimationPlayerComponent animationplayer;
        animationplayer.enabled = false;
        animationplayer.clips.resize(3);
        animationplayer.clips[0].clip = "anim/walk.anim";
        animationplayer.clips[0].name = "Walk";
        animationplayer.clips[0].wrap = ClipWrapMode::Loop;
        animationplayer.clips[1].clip = "models/hero.gltf#Idle";
        animationplayer.clips[2].clip = "anim/jump.anim";
        animationplayer.clips[2].wrap = ClipWrapMode::ClampForever;
        animationplayer.defaultClip = "Walk";
        animationplayer.playAutomatically = false;
        animationplayer.speed = 1.500000f;
        animationplayer.defaultFadeTime = 0.400000f;
        scene->addComponent<AnimationPlayerComponent>(entity, animationplayer);
    }

    const char* const EMITTED_ANIMATION_PLAYER =
        "AnimationPlayerComponent animationplayer;\n"
        "animationplayer.enabled = false;\n"
        "animationplayer.clips.resize(3);\n"
        "animationplayer.clips[0].clip = \"anim/walk.anim\";\n"
        "animationplayer.clips[0].name = \"Walk\";\n"
        "animationplayer.clips[0].wrap = ClipWrapMode::Loop;\n"
        "animationplayer.clips[1].clip = \"models/hero.gltf#Idle\";\n"
        "animationplayer.clips[2].clip = \"anim/jump.anim\";\n"
        "animationplayer.clips[2].wrap = ClipWrapMode::ClampForever;\n"
        "animationplayer.defaultClip = \"Walk\";\n"
        "animationplayer.playAutomatically = false;\n"
        "animationplayer.speed = 1.500000f;\n"
        "animationplayer.defaultFadeTime = 0.400000f;\n"
        "scene->addComponent<AnimationPlayerComponent>(entity, animationplayer);\n";

}

TEST(AnimationPlayerComponent, FactoryEmitsTheAuthoredFields) {
    Scene scene;
    const Entity entity = playerEntity(scene, authoredPlayer());
    const std::string code = Factory::createComponent(0, &scene, entity, ComponentType::AnimationPlayerComponent, "", "scene", "entity");
    EXPECT_EQ(code, EMITTED_ANIMATION_PLAYER);
    // the editor's GUIDs stay out of the build
    EXPECT_EQ(code.find(WALK_GUID), std::string::npos);
    EXPECT_NE(Factory::createAllComponents(0, &scene, entity, "", "scene", "entity").find("AnimationPlayerComponent animationplayer;"),
              std::string::npos);

    Scene built;
    const Entity builtEntity = built.createUserEntity();
    emittedAnimationPlayer(&built, builtEntity);
    AnimationPlayerComponent expected = authoredPlayer();
    for (AnimationPlayerClip& entry : expected.clips) entry.guid.clear();
    expectSameAuthored(built.getComponent<AnimationPlayerComponent>(builtEntity), expected);

    // a default player: no list, the settings
    Scene plain;
    const Entity plainEntity = playerEntity(plain, AnimationPlayerComponent());
    EXPECT_EQ(Factory::createComponent(0, &plain, plainEntity, ComponentType::AnimationPlayerComponent, "", "scene", "entity"),
              "AnimationPlayerComponent animationplayer;\n"
              "animationplayer.playAutomatically = true;\n"
              "animationplayer.speed = 1.000000f;\n"
              "animationplayer.defaultFadeTime = 0.250000f;\n"
              "scene->addComponent<AnimationPlayerComponent>(entity, animationplayer);\n");
}

// ---- ModelComponent.animationEntities (spec 9.R15) -----------------------------------------

namespace {

    Entity modelEntity(Scene& scene, bool animationEntities) {
        Entity entity = scene.createUserEntity();
        scene.addComponent<Transform>(entity, {});
        ModelComponent model;
        model.filename = "models/hero.gltf";
        // (ModelComponent leaves it uninitialized; the editor's add/clear set it)
        model.skeleton = NULL_ENTITY;
        model.animationEntities = animationEntities;
        scene.addComponent<ModelComponent>(entity, model);
        return entity;
    }

}

TEST(AnimationPlayerComponent, ModelAnimationEntitiesRoundTrip) {
    ModelComponent off;
    off.filename = "models/hero.gltf";
    off.skeleton = NULL_ENTITY;
    off.animationEntities = false;
    const YAML::Node offNode = Stream::encodeModelComponent(off);
    ASSERT_TRUE(offNode["animationEntities"]);
    EXPECT_EQ(offNode["animationEntities"].as<std::string>(), "false");
    EXPECT_FALSE(Stream::decodeModelComponent(offNode).animationEntities);
    EXPECT_EQ(YAML::Dump(Stream::encodeModelComponent(Stream::decodeModelComponent(offNode))), YAML::Dump(offNode));

    // on (the default) is not written: files with models save as before
    ModelComponent on;
    on.filename = "models/hero.gltf";
    on.skeleton = NULL_ENTITY;
    const YAML::Node onNode = Stream::encodeModelComponent(on);
    EXPECT_FALSE(onNode["animationEntities"]);
    // decoded over a Model that had it off (ModelLoadCmd undo, a paste, a prefab revert), the
    // missing key is on, not the old value; an emptied key too
    EXPECT_TRUE(Stream::decodeModelComponent(onNode, &off).animationEntities);
    EXPECT_TRUE(Stream::decodeModelComponent(YAML::Load("animationEntities: \"\"\n"), &off).animationEntities);
    EXPECT_TRUE(Stream::decodeModelComponent(YAML::Load("animationEntities: maybe\n"), &off).animationEntities);
    EXPECT_FALSE(Stream::decodeModelComponent(YAML::Load("animationEntities: no\n")).animationEntities);

    // a whole-component copy (Paste Component Values) carries it both ways
    Scene scene;
    const Entity source = modelEntity(scene, false);
    const Entity target = modelEntity(scene, true);
    EntityClipboard::applyComponentValue(&scene, target, ComponentType::ModelComponent,
                                         EntityClipboard::encodeComponent(&scene, source, ComponentType::ModelComponent));
    EXPECT_FALSE(scene.getComponent<ModelComponent>(target).animationEntities);
    const Entity back = modelEntity(scene, true);
    EntityClipboard::applyComponentValue(&scene, target, ComponentType::ModelComponent,
                                         EntityClipboard::encodeComponent(&scene, back, ComponentType::ModelComponent));
    EXPECT_TRUE(scene.getComponent<ModelComponent>(target).animationEntities);
}

TEST(AnimationPlayerComponent, ModelAnimationEntitiesPropertyFactoryAndPrefabOverride) {
    Scene scene;
    const Entity entity = modelEntity(scene, false);
    const PropertyData property = Catalog::findProperty(&scene, entity, ComponentType::ModelComponent, "animationEntities");
    ASSERT_EQ(property.type, PropertyType::Bool);
    EXPECT_EQ(property.ref, &scene.getComponent<ModelComponent>(entity).animationEntities);
    ASSERT_TRUE(property.def);
    EXPECT_TRUE(*static_cast<bool*>(property.def));  // Reset gives on
    EXPECT_TRUE(Catalog::findEntityProperties(&scene, entity, ComponentType::ModelComponent).count("animationEntities"));

    // the exporter: written only when off
    const std::string code = Factory::createComponent(0, &scene, entity, ComponentType::ModelComponent, "", "scene", "entity");
    EXPECT_NE(code.find("modelcomp.mergeStaticMeshes = false;\nmodelcomp.animationEntities = false;\n"), std::string::npos) << code;
    Scene plain;
    const Entity plainEntity = modelEntity(plain, true);
    EXPECT_EQ(Factory::createComponent(0, &plain, plainEntity, ComponentType::ModelComponent, "", "scene", "entity").find("animationEntities"),
              std::string::npos);

    // a prefab instance turning it off overrides that one property, applied as the instance loads
    Scene asset;
    const Entity assetRoot = modelEntity(asset, true);
    EXPECT_EQ(prefabinternal::diffComponentProperties(&asset, assetRoot, &scene, entity, ComponentType::ModelComponent),
              std::vector<std::string>{"animationEntities"});
    prefabinternal::ScratchEntity values;
    values.takeComponents(&scene, entity, Catalog::componentTypeToSignature(&scene, ComponentType::ModelComponent));
    Scene loaded;
    const Entity loadedRoot = modelEntity(loaded, true);
    std::set<std::string> names{"animationEntities"};
    prefabinternal::copyProperties(values, &loaded, loadedRoot, ComponentType::ModelComponent, names, "test");
    EXPECT_FALSE(loaded.getComponent<ModelComponent>(loadedRoot).animationEntities);
    EXPECT_EQ(loaded.getComponent<ModelComponent>(loadedRoot).filename, "models/hero.gltf");
}

// Saved blend shape weights are the mesh's own (MeshComponent::morphWeightsSet): decoding them
// (all zero too) sets the flag the model load reads, the exporter writes it; a mesh saved
// without weights (an older or hand-written file) has its file's defaults on load
TEST(AnimationPlayerComponent, SavedMorphWeightsAreKeptByTheModelLoad) {
    MeshComponent zeros;
    zeros.morphWeightsSet = true;
    const YAML::Node node = Stream::encodeMeshComponent(zeros, false, false, false);
    ASSERT_TRUE(node["morphWeights"]);
    const MeshComponent decoded = Stream::decodeMeshComponent(node);
    EXPECT_TRUE(decoded.morphWeightsSet);
    EXPECT_EQ(decoded.morphWeights[0], 0.0f);
    YAML::Node without = YAML::Clone(node);
    without.remove("morphWeights");
    EXPECT_FALSE(Stream::decodeMeshComponent(without).morphWeightsSet);

    // (integration review M1) a mesh whose model never loaded saves no weights (its file's
    // defaults apply when it loads); a set mesh saves the target count its weights cover, and
    // weights saved before the count existed cover every target
    EXPECT_FALSE(Stream::encodeMeshComponent(MeshComponent(), false, false, false)["morphWeights"]) << "never loaded: none saved";
    MeshComponent covered;
    covered.morphWeightsSet = true;
    covered.morphTargetCount = 2;
    covered.morphWeights[1] = 0.4f;
    const MeshComponent back = Stream::decodeMeshComponent(Stream::encodeMeshComponent(covered, false, false, false));
    EXPECT_TRUE(back.morphWeightsSet);
    EXPECT_EQ(back.morphTargetCount, 2);
    EXPECT_FLOAT_EQ(back.morphWeights[1], 0.4f);
    // (FX6) weights saved without their count (an older version) cover up to their last non-zero
    // weight: the zeros after it get the file's defaults on load, as they always did then
    MeshComponent oldSaved;
    oldSaved.morphWeightsSet = true;
    oldSaved.morphWeights[2] = 0.3f;
    YAML::Node older = Stream::encodeMeshComponent(oldSaved, false, false, false);
    older.remove("morphTargetCount");
    EXPECT_EQ(Stream::decodeMeshComponent(older).morphTargetCount, 3);
    // and such a mesh re-saves as it was (no count: the weights say it), so an old scene re-saves
    // byte for byte (test_stochastic_tiling's old scene: zero weights, no count); a count beyond the
    // last non-zero weight is written
    EXPECT_FALSE(Stream::encodeMeshComponent(Stream::decodeMeshComponent(older), false, false, false)["morphTargetCount"]);
    MeshComponent oldZeros;
    oldZeros.morphWeightsSet = true;
    YAML::Node zerosNode = Stream::encodeMeshComponent(oldZeros, false, false, false);
    EXPECT_FALSE(zerosNode["morphTargetCount"]) << "zero weights covering no target: no count";
    EXPECT_FALSE(Stream::encodeMeshComponent(Stream::decodeMeshComponent(zerosNode), false, false, false)["morphTargetCount"]);
    EXPECT_FALSE(Stream::encodeMeshComponent(covered, false, false, false)["morphTargetCount"]) << "2 targets, the last weight at index 1";
    MeshComponent wider = covered;
    wider.morphTargetCount = 4;
    const YAML::Node widerNode = Stream::encodeMeshComponent(wider, false, false, false);
    ASSERT_TRUE(widerNode["morphTargetCount"]) << "4 targets, zeros after the last non-zero weight";
    EXPECT_EQ(widerNode["morphTargetCount"].as<int>(), 4);
    EXPECT_EQ(Stream::decodeMeshComponent(widerNode).morphTargetCount, 4);
    // a non-zero weight set before any load: Stream and the exporter both keep it, with that count
    MeshComponent edited;
    edited.morphWeights[0] = 0.5f;
    const YAML::Node editedNode = Stream::encodeMeshComponent(edited, false, false, false);
    ASSERT_TRUE(editedNode["morphWeights"]) << "a weight set before any load is kept";
    EXPECT_EQ(Stream::decodeMeshComponent(editedNode).morphTargetCount, 1);
    {
        Scene exported;
        const Entity entity = exported.createEntity();
        exported.addComponent<Transform>(entity, {});
        exported.addComponent<MeshComponent>(entity, edited);
        const std::string code = Factory::createComponent(0, &exported, entity, ComponentType::MeshComponent, "", "scene", "entity");
        EXPECT_NE(code.find("mesh.morphWeightsSet = true;\n"), std::string::npos) << code;
        EXPECT_NE(code.find("mesh.morphTargetCount = 1;\n"), std::string::npos) << code;
    }

    Scene scene;
    const Entity entity = scene.createEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<MeshComponent>(entity, zeros);
    const std::string code = Factory::createComponent(0, &scene, entity, ComponentType::MeshComponent, "", "scene", "entity");
    EXPECT_NE(code.find("mesh.morphWeightsSet = true;\n"), std::string::npos) << code;
    scene.getComponent<MeshComponent>(entity).morphWeightsSet = false;
    EXPECT_EQ(Factory::createComponent(0, &scene, entity, ComponentType::MeshComponent, "", "scene", "entity").find("morphWeightsSet"),
              std::string::npos);
}
