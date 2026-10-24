// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation Player component's editor plumbing (animation clips spec 3.2, 3.6, 9.R8/R14/R22/
// R25/R26/R41): Catalog properties, Stream encode/decode (path hint + GUID per clip entry), prefab
// overrides of the clip list, copy/paste, ProjectUtils add/remove, and the C++ the exporter emits
// (compiled here as written). What needs a project on disk runs in the editor (MCP tests): the
// set_properties / add_component validation of clip paths (McpValues), the asset path visitor
// (moves of a clip or model) and the scene resources (Project's private helpers).

#include <gtest/gtest.h>

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

    Catalog::copyPropertyValue(&scene, source, &scene, target, type, "clips");
    EXPECT_EQ(scene.getComponent<AnimationPlayerComponent>(target).clips, authoredPlayer().clips);

    scene.getComponent<AnimationPlayerComponent>(source).clips[0].wrap = ClipWrapMode::PingPong;
    Catalog::copyPropertyValue(&scene, source, &scene, target, type, "clips[0].wrap");
    EXPECT_EQ(scene.getComponent<AnimationPlayerComponent>(target).clips[0].wrap, ClipWrapMode::PingPong);

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
    std::vector<std::string> changed = prefabinternal::diffComponentProperties(&asset, assetRoot, &instance, moreRoot, type);
    EXPECT_NE(std::find(changed.begin(), changed.end(), "clips"), changed.end());

    // one entry's wrap
    AnimationPlayerComponent looping = assetPlayer;
    looping.clips[0].wrap = ClipWrapMode::Loop;
    const Entity loopingRoot = playerEntity(instance, looping);
    changed = prefabinternal::diffComponentProperties(&asset, assetRoot, &instance, loopingRoot, type);
    EXPECT_NE(std::find(changed.begin(), changed.end(), "clips[0].wrap"), changed.end());

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
    EXPECT_FALSE(Stream::resolveAnimationPlayerClips(nullptr, &scene));
    // no project open (no AssetDatabase): nothing changes
    Project project;
    EXPECT_FALSE(Stream::resolveAnimationPlayerClips(&project, &scene));
    EXPECT_EQ(scene.getComponent<AnimationPlayerComponent>(entity).clips, authoredPlayer().clips);
}

// ---- C++ export ----------------------------------------------------------------------------

namespace {

    // the code below is Factory's output for authoredPlayer(), pasted: it must keep compiling
    // against the engine headers and build the same component
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
