// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The migration framework itself (docs/specs/S03-data-safety.md R20-R23); the loaders' use of
// it is MigrationsTest.cpp

#include "StreamMigrations.h"
#include "Stream.h"
#include "component/AnimationPlayerComponent.h"
#include "component/AnimatorComponent.h"
#include "component/ParticleSystemComponent.h"
#include "pool/ShaderPool.h"
#include "support/EditorTestSupport.h"
#include "component/TextComponent.h"
#include "model/ModelImportSettings.h"
#include "reflect/Reflect.h"
#include "registry/SortingLayers.h"

#include <gtest/gtest.h>

namespace StreamMigrationsTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {
    const DocumentKind kStreamMigrationsKinds[] = {DocumentKind::Scene, DocumentKind::Project, DocumentKind::Material, DocumentKind::Prefab};
}

TEST(StreamMigrations, TablesAreOrderedAndReachCurrent){
    for (DocumentKind kind : kStreamMigrationsKinds){
        SCOPED_TRACE(StreamMigrations::kindName(kind));
        const auto& table = StreamMigrations::table(kind);
        int previous = -1;
        for (const Migration& migration : table){
            EXPECT_EQ(migration.to, migration.from + 1) << migration.name;
            if (previous >= 0){
                EXPECT_EQ(migration.from, previous) << migration.name;
            }
            EXPECT_NE(migration.apply, nullptr);
            EXPECT_NE(std::string(migration.name), "");
            previous = migration.to;
        }
        if (!table.empty()){
            EXPECT_EQ(table.back().to, StreamMigrations::currentVersion(kind));
        }
    }
}

TEST(StreamMigrations, ReadVersion){
    EXPECT_EQ(StreamMigrations::readVersion(DocumentKind::Scene, YAML::Load("{name: A}")), 0);
    EXPECT_EQ(StreamMigrations::readVersion(DocumentKind::Scene, YAML::Load("{formatVersion: 7}")), 7);
    EXPECT_EQ(StreamMigrations::readVersion(DocumentKind::Scene, YAML::Load("{formatVersion: abc}")), 0);
    EXPECT_EQ(StreamMigrations::readVersion(DocumentKind::Prefab, YAML::Load("{type: Prefab}")), 1);
    EXPECT_EQ(StreamMigrations::readVersion(DocumentKind::Prefab, YAML::Load("{version: 3}")), 3);
    EXPECT_EQ(StreamMigrations::readVersion(DocumentKind::Material, YAML::Load("[1, 2]")), 0);
}

TEST(StreamMigrations, MigrateStampsVersionFirst){
    YAML::Node root = YAML::Load("{name: Level1, entities: [{name: A}]}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
    EXPECT_EQ(result.versionRead, 0);
    EXPECT_EQ(result.versionCurrent, StreamMigrations::currentVersion(DocumentKind::Scene));
    EXPECT_TRUE(result.migrated());
    EXPECT_FALSE(result.newer());
    ASSERT_TRUE(root.IsMap());
    EXPECT_EQ(root.begin()->first.Scalar(), "formatVersion");
    EXPECT_EQ(root["formatVersion"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Scene));
    EXPECT_EQ(root["name"].as<std::string>(), "Level1");
    EXPECT_EQ(root["entities"][0]["name"].as<std::string>(), "A");
    EXPECT_EQ(root.size(), 3u);

    // current: nothing to do
    const MigrationResult again = StreamMigrations::migrate(DocumentKind::Scene, root);
    EXPECT_FALSE(again.migrated());
    EXPECT_EQ(again.versionRead, StreamMigrations::currentVersion(DocumentKind::Scene));
}

TEST(StreamMigrations, NewerIsLeftUntouched){
    YAML::Node root = YAML::Load("{formatVersion: 99, name: Future}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
    EXPECT_TRUE(result.newer());
    EXPECT_FALSE(result.migrated());
    EXPECT_EQ(root["formatVersion"].as<int>(), 99);
    const int scene = StreamMigrations::currentVersion(DocumentKind::Scene);
    EXPECT_EQ(StreamMigrations::newerFormatMessage(DocumentKind::Scene, scene + 1),
            "Written by a newer JasperEngine (scene format " + std::to_string(scene + 1) + "; this editor writes " +
            std::to_string(scene) + ").");
}

TEST(StreamMigrations, OldPrefabsAreLeftToThePrefabLoader){
    YAML::Node root = YAML::Load("{type: Prefab, version: 2}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Prefab, root);
    EXPECT_FALSE(result.migrated());
    EXPECT_EQ(result.versionRead, 2);
    EXPECT_EQ(root["version"].as<int>(), 2);
}

TEST(StreamMigrations, StampReplacesAnExistingKey){
    YAML::Node root = YAML::Load("{a: 1, formatVersion: 0, b: 2}");
    StreamMigrations::stampCurrentVersion(DocumentKind::Material, root);
    auto it = root.begin();
    EXPECT_EQ(it->first.Scalar(), "formatVersion");
    EXPECT_EQ(it->second.as<int>(), 1);   // material: still format 1
    ++it;
    EXPECT_EQ(it->first.Scalar(), "a");
    ++it;
    EXPECT_EQ(it->first.Scalar(), "b");
    EXPECT_EQ(root.size(), 3u);
}

TEST(StreamMigrations, SceneV1ToV2ShadowBiasInTexels){
    // S10: a light's shadowBias was in depth units (default 0.002), it is in shadow texels now;
    // the old default quality Low becomes Auto. Nested children and every light are reached.
    YAML::Node root = YAML::Load(
        "{formatVersion: 1, scene: {shadowQuality: low}, entities: ["
        "{name: Sun, components: {light: {type: directional, shadowBias: 0.002}}},"
        "{name: Group, children: [{name: Lamp, components: {light: {type: spot, shadowBias: 0.01}}}]},"
        "{name: Tuned, components: {light: {type: point, shadowBias: 1}}}]}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
    EXPECT_EQ(result.versionRead, 1);
    // 1 -> 2, then 2 -> 3 (S20's particle systems: none here), 3 -> 4 (S29's derived transform
    // values: none here), 4 -> 5 (S18's splines: identity), 5 -> 6 (S21's Animation Player
    // controllers: none here), 6 -> 7 (S27's curve form: no lines), 7 -> 8 (S44's sheet sprites:
    // identity), 8 -> 9 (S98's mesh assets: identity), 9 -> 10 (S92's) and 10 -> 11 (S94's Text
    // alignment: no texts)
    ASSERT_EQ(result.applied.size(), 10u);
    EXPECT_EQ(root["formatVersion"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Scene));
    EXPECT_EQ(root["scene"]["shadowQuality"].as<std::string>(), "auto");
    EXPECT_FLOAT_EQ(root["entities"][0]["components"]["light"]["shadowBias"].as<float>(), 1.0f);
    EXPECT_FLOAT_EQ(root["entities"][1]["children"][0]["components"]["light"]["shadowBias"].as<float>(), 1.0f);
    EXPECT_FLOAT_EQ(root["entities"][2]["components"]["light"]["shadowBias"].as<float>(), 1.0f);
    // one line for the quality, one for the biases (two lights reset), one for the HTML UI render mode
    ASSERT_EQ(result.log.lines.size(), 3u);
    EXPECT_NE(result.log.lines[1].find("2 lights' Bias reset to 1"), std::string::npos) << result.log.lines[1];

    // an explicit quality other than Low stays
    YAML::Node medium = YAML::Load("{formatVersion: 1, scene: {shadowQuality: medium}, entities: []}");
    StreamMigrations::migrate(DocumentKind::Scene, medium);
    EXPECT_EQ(medium["scene"]["shadowQuality"].as<std::string>(), "medium");
    EXPECT_TRUE(StreamMigrations::migrate(DocumentKind::Scene, medium).applied.empty());
}

TEST(StreamMigrations, PrefabV3ToV4ShadowBiasInTexels){
    YAML::Node root = YAML::Load(
        "{type: Prefab, version: 3, members: [{type: Entity, entity: 1001, components: {light: {type: spot, shadowBias: 0.002}}}]}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Prefab, root);
    EXPECT_TRUE(result.migrated());
    EXPECT_EQ(root["version"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Prefab));
    EXPECT_FLOAT_EQ(root["members"][0]["components"]["light"]["shadowBias"].as<float>(), 1.0f);
}

TEST(StreamMigrations, SceneV1ToV2ClearsTheRetiredMeshBit){
    // mesh bit 5 was the PCF filter ('Pcf') before it was retired, upstream's Stochastic Tiling
    // ('Sto') in a keyVersion 2 key: an old key loses it, a version 2 key and other types' bit 5
    // keep theirs (S10's 'Shq' is bit 33)
    const uint64_t old = (1u << 3) | (1u << 4) | (1u << 5);
    YAML::Node root = YAML::Load(
        "{formatVersion: 1, scene: {shadowQuality: high}, entities: [], shaderKeys: ["
        "{type: mesh, properties: " + std::to_string(old) + "}, {type: blit, properties: 32}, "
        "{type: mesh, properties: " + std::to_string(old) + ", keyVersion: 2}]}");
    StreamMigrations::migrate(DocumentKind::Scene, root);
    EXPECT_EQ(root["shaderKeys"][0]["properties"].as<uint64_t>(), uint64_t((1u << 3) | (1u << 4)));
    EXPECT_EQ(root["shaderKeys"][1]["properties"].as<uint32_t>(), 32u);
    EXPECT_EQ(root["shaderKeys"][2]["properties"].as<uint64_t>(), old);
}

TEST(StreamMigrations, SceneV2ToV3ParticleSystemsMoveToTheirTargets){
    // S20: an old Particle System action drawing into a Points GameObject becomes an inline
    // effect on that GameObject; the action stays its controller (Play On Awake off), the empty
    // Points go, the log says what happened
    YAML::Node root = YAML::Load(
        "{formatVersion: 2, entities: ["
        "{type: Entity, entity: 1001, name: Fire, components: {transform: {position: [0, 0, 0]}, points: {maxPoints: 50, points: []}}},"
        "{type: Entity, entity: 1002, name: Fire Particles, components: {action: {state: Running, speed: 2, target: 1001},"
        " particlesystem: {maxParticles: 50, rate: 25, loop: true, lifeInitializer: {minLife: 1, maxLife: 2},"
        " bursts: [{time: 0, minCount: 5, maxCount: 7}]}}},"
        "{type: Entity, entity: 1003, name: Lonely, components: {particlesystem: {rate: 3}}}]}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
    EXPECT_EQ(result.versionRead, 2);
    // 2 -> 3, then S29's 3 -> 4, S18's identity 4 -> 5, S21's 5 -> 6, S27's curve form 6 -> 7, the
    // identities 7 -> 8 (S44) and 8 -> 9 (S98), S92's 9 -> 10 and S94's 10 -> 11
    ASSERT_EQ(result.applied.size(), 9u);
    EXPECT_EQ(root["formatVersion"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Scene));

    const YAML::Node fire = root["entities"][0]["components"];
    ASSERT_TRUE(fire["particlesystem"]);
    EXPECT_FALSE(fire["points"]) << "the empty Points of its only system go";
    EXPECT_FALSE(fire["particlesystem"]["playOnAwake"].as<bool>()) << "the action plays it";
    EXPECT_FLOAT_EQ(fire["particlesystem"]["playbackSpeed"].as<float>(), 2.0f);
    const std::string effect = fire["particlesystem"]["inlineEffect"].as<std::string>();
    EXPECT_NE(effect.find("\"rateOverTime\": 25"), std::string::npos) << effect;
    EXPECT_NE(effect.find("\"bursts\""), std::string::npos) << effect;

    const YAML::Node controller = root["entities"][1]["components"];
    EXPECT_TRUE(controller["action"]);
    EXPECT_FALSE(controller["particlesystem"]);

    // without a target the system stays where it was, and gets a Transform to emit from
    const YAML::Node lonely = root["entities"][2]["components"];
    EXPECT_TRUE(lonely["particlesystem"]);
    EXPECT_TRUE(lonely["transform"]);
    EXPECT_TRUE(lonely["particlesystem"]["playOnAwake"].as<bool>());

    ASSERT_EQ(result.log.lines.size(), 2u);
    EXPECT_NE(result.log.lines[0].find("Particle System of Fire Particles: converted to an inline effect on its target Fire"), std::string::npos)
        << result.log.lines[0];
    EXPECT_NE(result.log.lines[1].find("no Transform"), std::string::npos) << result.log.lines[1];

    // a format 3 scene is not touched again
    EXPECT_TRUE(StreamMigrations::migrate(DocumentKind::Scene, root).applied.empty());
}

TEST(StreamMigrations, OldUIParticleSystemKeepsItsUnscaledTime){
    // origin/main's (ui-scaling-text) UI particles: a Points emitter under a UI element and its old
    // Particle System child with useUnscaledTime; the converted system on the emitter keeps it
    YAML::Node root = YAML::Load(
        "{formatVersion: 2, entities: ["
        "{type: Entity, entity: 1001, name: Sparkle, components: {transform: {position: [0, 0, 0]}, points: {points: []}}},"
        "{type: Entity, entity: 1002, name: Particle System, components: {action: {state: Running, target: 1001},"
        " particlesystem: {maxParticles: 100, rate: 25, localSpace: true, useUnscaledTime: true}}},"
        "{type: Entity, entity: 1003, name: Scaled, components: {particlesystem: {rate: 3}}}]}");
    StreamMigrations::migrate(DocumentKind::Scene, root);
    const YAML::Node sparkle = root["entities"][0]["components"]["particlesystem"];
    ASSERT_TRUE(sparkle);
    ASSERT_TRUE(sparkle["useUnscaledTime"]);
    EXPECT_TRUE(sparkle["useUnscaledTime"].as<bool>());
    const ParticleSystemComponent decoded = Stream::decodeParticleSystemComponent(sparkle);
    EXPECT_TRUE(decoded.useUnscaledTime);
    EXPECT_EQ(decoded.inlineEffect.simulationSpace, particles::SimulationSpace::Local);
    // one without it is written as before (no key)
    EXPECT_FALSE(root["entities"][2]["components"]["particlesystem"]["useUnscaledTime"]);
}

TEST(StreamMigrations, SceneV2ToV3MovedSystemNamesItsOldGameObject){
    // S94 F2: an old script wraps the old system's GameObject (ParticleSystem(scene, e)); the
    // system moved to its target records it as legacyFrom, which reads back on the component.
    // One that stays where it was records nothing
    YAML::Node root = YAML::Load(
        "{formatVersion: 2, entities: ["
        "{type: Entity, entity: 1575, name: FxConfetti, components: {transform: {position: [0, 0, 0]}, points: {points: ~}}},"
        "{type: Entity, entity: 1576, name: FxConfettiSystem, components: {action: {state: Stopped, target: 1575},"
        " particlesystem: {maxParticles: 45, rate: 0, bursts: [{time: 0.001, minCount: 40, maxCount: 40}]}}},"
        "{type: Entity, entity: 1577, name: Lonely, components: {particlesystem: {rate: 3}}}]}");
    StreamMigrations::migrate(DocumentKind::Scene, root);
    const YAML::Node moved = root["entities"][0]["components"]["particlesystem"];
    ASSERT_TRUE(moved);
    ASSERT_TRUE(moved["legacyFrom"]);
    EXPECT_EQ(moved["legacyFrom"].as<uint64_t>(), 1576u);
    EXPECT_EQ(Stream::decodeParticleSystemComponent(moved).legacyFrom, (Entity)1576);
    EXPECT_FALSE(root["entities"][2]["components"]["particlesystem"]["legacyFrom"]) << "a system that did not move";
    EXPECT_EQ(Stream::decodeParticleSystemComponent(root["entities"][2]["components"]["particlesystem"]).legacyFrom, NULL_ENTITY);
    // written back only when set
    EXPECT_FALSE(Stream::encodeParticleSystemComponent(ParticleSystemComponent())["legacyFrom"]);
    EXPECT_EQ(Stream::encodeParticleSystemComponent(Stream::decodeParticleSystemComponent(moved))["legacyFrom"].as<uint64_t>(), 1576u);
}

TEST(StreamMigrations, PrefabV4ToV5ParticleSystems){
    YAML::Node root = YAML::Load(
        "{type: Prefab, version: 4, members: ["
        "{type: Entity, entity: 1001, name: Dust, components: {transform: {position: [0, 0, 0]}, points: {points: []}}},"
        "{type: Entity, entity: 1002, name: Dust FX, components: {action: {target: 1001}, particlesystem: {rate: 9}}}]}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Prefab, root);
    EXPECT_TRUE(result.migrated());
    EXPECT_EQ(root["version"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Prefab)) << "migrated to the current version";
    EXPECT_TRUE(root["members"][0]["components"]["particlesystem"]);
    EXPECT_FALSE(root["members"][1]["components"]["particlesystem"]);
}

TEST(StreamMigrations, SceneV5ToV6AnimationPlayerControllersMoveToAnimators){
    // upstream's (anim-layers) controller on the Animation Player: an Animator on the same
    // GameObject plays it (the player's checkbox, no retargeting: upstream had none) and the
    // player hands over to it, disabled with its clips and layers kept; a prefab instance's
    // override of the controller becomes an added Animator; layers are read as they are
    YAML::Node root = YAML::Load(
        "{formatVersion: 5, entities: ["
        "{type: Entity, entity: 1001, name: Granjero, components: {animationplayer: {enabled: true, clips: [{clip: anim/idle.anim, name: Idle}],"
        " layers: [{name: Base Layer}, {name: Holding, weight: 0.5, mask: anim/arm.mask}],"
        " controller: anim/granjero.controller, controllerGuid: aaaabbbbccccddddeeeeffff00001111}}},"
        "{type: Entity, entity: 1002, name: Plain, components: {animationplayer: {clips: [{clip: anim/walk.anim}], controller: ''}}},"
        "{type: Entity, entity: 1003, name: Instance, prefabInstance: {overrides: [{registryEntity: 1001,"
        " properties: {animationplayer: [controller, controllerGuid]},"
        " components: {animationplayer: {controller: anim/other.controller, controllerGuid: 0000111122223333444455556666aaaa}}}]}}]}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
    EXPECT_EQ(result.versionRead, 5);
    ASSERT_EQ(result.applied.size(), 6u) << "5 -> 6, S27's curve form 6 -> 7 (no lines), S44's sheet sprites 7 -> 8, S98's mesh assets 8 -> 9, S92's 9 -> 10, then S94's 10 -> 11";
    EXPECT_EQ(root["formatVersion"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Scene));

    const YAML::Node granjero = root["entities"][0]["components"];
    ASSERT_TRUE(granjero["animator"]);
    EXPECT_EQ(granjero["animator"]["controller"].as<std::string>(), "anim/granjero.controller");
    EXPECT_EQ(granjero["animator"]["controllerGuid"].as<std::string>(), "aaaabbbbccccddddeeeeffff00001111");
    EXPECT_TRUE(granjero["animator"]["enabled"].as<bool>());
    EXPECT_EQ(granjero["animator"]["retarget"].as<std::string>(), "off");
    EXPECT_FALSE(granjero["animationplayer"]["controller"]);
    EXPECT_FALSE(granjero["animationplayer"]["controllerGuid"]);
    EXPECT_FALSE(granjero["animationplayer"]["enabled"].as<bool>()) << "the player hands over";
    EXPECT_EQ(granjero["animationplayer"]["layers"].size(), 2u) << "its layers kept";
    EXPECT_EQ(granjero["animationplayer"]["clips"].size(), 1u) << "its clips kept";

    const YAML::Node plain = root["entities"][1]["components"];
    EXPECT_FALSE(plain["animator"]) << "an empty controller is no Animator";
    EXPECT_FALSE(plain["animationplayer"]["controller"]);

    const YAML::Node entry = root["entities"][2]["prefabInstance"]["overrides"][0];
    bool added = false;
    for (const YAML::Node& name : entry["addedComponents"]) added = added || name.as<std::string>() == "animator";
    EXPECT_TRUE(added);
    EXPECT_EQ(entry["components"]["animator"]["controller"].as<std::string>(), "anim/other.controller");
    bool enabledOverride = false, controllerOverride = false;
    for (const YAML::Node& name : entry["properties"]["animationplayer"]){
        enabledOverride = enabledOverride || name.as<std::string>() == "enabled";
        controllerOverride = controllerOverride || name.as<std::string>() == "controller";
    }
    EXPECT_TRUE(enabledOverride);
    EXPECT_FALSE(controllerOverride);
    EXPECT_FALSE(entry["components"]["animationplayer"]["enabled"].as<bool>());
    // then S94's 10 -> 11: the two Animators keep their root drift (a deliberate addition, S94 F33)
    ASSERT_EQ(result.log.lines.size(), 2u);
    EXPECT_NE(result.log.lines[0].find("2 players got an Animator"), std::string::npos) << result.log.lines[0];
    EXPECT_NE(result.log.lines[1].find("keep"), std::string::npos) << result.log.lines[1];

    // a prefab's members the same way (6 -> 7, then the curve form 7 -> 8)
    YAML::Node prefab = YAML::Load(
        "{type: Prefab, version: 6, members: [{type: Entity, entity: 1001, components: {animationplayer: {enabled: false, controller: anim/c.controller}}}]}");
    const MigrationResult prefabResult = StreamMigrations::migrate(DocumentKind::Prefab, prefab);
    EXPECT_TRUE(prefabResult.migrated());
    EXPECT_EQ(prefab["version"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Prefab));
    EXPECT_EQ(prefab["members"][0]["components"]["animator"]["controller"].as<std::string>(), "anim/c.controller");
    EXPECT_FALSE(prefab["members"][0]["components"]["animator"]["enabled"].as<bool>()) << "a disabled player: a disabled Animator";
}

// S94 F24 (AC24): scene 8 -> 9 and prefab 9 -> 10 turn a Text's pivotBaseline / pivotCentered into
// its alignment (false: the old defaults, Bottom and Left, so an instance's override keeps its
// value), children and prefab instance overrides included, the overridden names renamed
TEST(StreamMigrations, SceneV8TextPivotsBecomeAnchor){
    YAML::Node root = YAML::Load(jasperengine::editor::test::readText(jasperengine::editor::test::fixture("migrations/scene-v8-text.scene")));
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
    ASSERT_TRUE(result.migrated());
    EXPECT_EQ(root["formatVersion"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Scene));
    // S92's 9 -> 10 logs the scene's HTML UI render mode first
    ASSERT_EQ(result.log.lines.size(), 2u);
    EXPECT_NE(result.log.lines[0].find("Screen Space - Camera"), std::string::npos) << result.log.lines[0];
    EXPECT_NE(result.log.lines[1].find("3 Texts'"), std::string::npos) << result.log.lines[1];

    const YAML::Node plaque = root["entities"][0]["components"]["text"];
    EXPECT_EQ(plaque["anchorVertical"].as<std::string>(), "baseline");
    EXPECT_EQ(plaque["anchorHorizontal"].as<std::string>(), "center");
    EXPECT_FALSE(plaque["pivotBaseline"]);
    EXPECT_FALSE(plaque["pivotCentered"]);
    EXPECT_EQ(plaque["text"].as<std::string>(), "12");

    const YAML::Node caption = root["entities"][0]["children"][0]["components"]["text"];
    EXPECT_EQ(caption["anchorVertical"].as<std::string>(), "bottom");
    EXPECT_EQ(caption["anchorHorizontal"].as<std::string>(), "left");

    const YAML::Node instance = root["entities"][1]["prefabInstance"]["overrides"][0];
    EXPECT_EQ(instance["components"]["text"]["anchorHorizontal"].as<std::string>(), "left");
    EXPECT_FALSE(instance["components"]["text"]["anchorVertical"]);
    EXPECT_EQ(instance["properties"]["text"][0].as<std::string>(), "anchorHorizontal");
    EXPECT_EQ(instance["properties"]["text"][1].as<std::string>(), "text");

    // what the Text decodes to
    TextComponent text;
    const YAML::Node component = root["entities"][0]["components"]["text"];
    reflect::decodeInto(ComponentType::TextComponent, component, &text, nullptr);
    EXPECT_EQ(text.anchorVertical, TextAnchorVertical::BASELINE);
    EXPECT_EQ(text.anchorHorizontal, TextAnchorHorizontal::CENTER);
}

TEST(StreamMigrations, PrefabV9TextPivotsBecomeAnchor){
    YAML::Node root = YAML::Load(jasperengine::editor::test::readText(jasperengine::editor::test::fixture("migrations/prefab-v9-text.prefab")));
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Prefab, root);
    ASSERT_TRUE(result.migrated());
    EXPECT_EQ(root["version"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Prefab));
    const YAML::Node sign = root["members"][0]["components"]["text"];
    EXPECT_EQ(sign["anchorVertical"].as<std::string>(), "baseline");
    EXPECT_EQ(sign["anchorHorizontal"].as<std::string>(), "center");
    EXPECT_FALSE(sign["pivotBaseline"]);
}

// S94 F33, F34: with Apply Root Motion off a clip with a root_motion block plays in place now. An
// Animator or Animation Player of scene 8 or prefab 9 (children included) keeps the drift it played
// with (legacyRootDrift); one with Apply Root Motion on and an instance's overrides are left as they
// are; a component written now has none
TEST(StreamMigrations, SceneV8AnimatorsKeepTheirRootDrift){
    YAML::Node root = YAML::Load(jasperengine::editor::test::readText(jasperengine::editor::test::fixture("migrations/scene-v8-root-motion.scene")));
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
    ASSERT_TRUE(result.migrated());
    EXPECT_EQ(root["formatVersion"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Scene));
    // S92's 9 -> 10 logs the scene's HTML UI render mode first
    ASSERT_EQ(result.log.lines.size(), 2u);
    EXPECT_NE(result.log.lines[0].find("Screen Space - Camera"), std::string::npos) << result.log.lines[0];
    EXPECT_NE(result.log.lines[1].find("2 Animators and Animation Players keep"), std::string::npos) << result.log.lines[1];

    const YAML::Node hero = root["entities"][0]["components"]["animator"];
    ASSERT_TRUE(hero["legacyRootDrift"]);
    EXPECT_TRUE(hero["legacyRootDrift"].as<bool>());
    AnimatorComponent animator;
    reflect::decodeInto(ComponentType::AnimatorComponent, hero, &animator, nullptr);
    EXPECT_TRUE(animator.legacyRootDrift);
    EXPECT_FALSE(animator.applyRootMotion);

    const YAML::Node dancer = root["entities"][0]["children"][0]["components"]["animationplayer"];
    ASSERT_TRUE(dancer["legacyRootDrift"]);
    EXPECT_TRUE(Stream::decodeAnimationPlayerComponent(dancer).legacyRootDrift);

    EXPECT_FALSE(root["entities"][1]["components"]["animator"]["legacyRootDrift"]) << "Apply Root Motion on: nothing changed";
    const YAML::Node overrides = root["entities"][2]["prefabInstance"]["overrides"][0];
    EXPECT_FALSE(overrides["components"]["animator"]["legacyRootDrift"]) << "an override follows its prefab";
    EXPECT_EQ(overrides["properties"]["animator"].size(), 1u);

    // written back only when set
    EXPECT_FALSE(Stream::encodeAnimationPlayerComponent(AnimationPlayerComponent())["legacyRootDrift"]);
    EXPECT_TRUE(Stream::encodeAnimationPlayerComponent(Stream::decodeAnimationPlayerComponent(dancer))["legacyRootDrift"].as<bool>());
}

TEST(StreamMigrations, PrefabV9AnimationPlayersKeepTheirRootDrift){
    YAML::Node root = YAML::Load(jasperengine::editor::test::readText(jasperengine::editor::test::fixture("migrations/prefab-v9-root-motion.prefab")));
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Prefab, root);
    ASSERT_TRUE(result.migrated());
    EXPECT_EQ(root["version"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Prefab));
    const YAML::Node walker = root["members"][0]["components"]["animationplayer"];
    ASSERT_TRUE(walker["legacyRootDrift"]);
    EXPECT_TRUE(Stream::decodeAnimationPlayerComponent(walker).legacyRootDrift);

    YAML::Node current = YAML::Load("{type: Prefab, version: " + std::to_string(StreamMigrations::currentVersion(DocumentKind::Prefab)) +
                                    ", members: [{type: Entity, entity: 1001, components: {animator: {enabled: true}}}]}");
    EXPECT_FALSE(StreamMigrations::migrate(DocumentKind::Prefab, current).migrated());
    EXPECT_FALSE(current["members"][0]["components"]["animator"]["legacyRootDrift"]);
}

// S94 F5 (AC5), F42: a project written before Transparency Sort Mode Default followed the projection
// sorted every camera by the distance: it loads as Perspective; one that named a mode keeps it,
// and a project of format 2 without the key is Default. Its FBX submeshes were the used material
// slots in order: it keeps FBX Submesh Order Used Materials beside its other Mesh settings, and a
// project of format 2 without the key follows the material slots
TEST(StreamMigrations, ProjectV1KeepsPerspectiveSort){
    YAML::Node root = YAML::LoadFile(jasperengine::editor::test::fixture("migrations/project-v1-sort.yaml").string());
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Project, root);
    EXPECT_EQ(result.versionRead, 1);
    EXPECT_EQ(result.versionCurrent, 2);
    EXPECT_TRUE(result.migrated());
    EXPECT_EQ(root["formatVersion"].as<int>(), 2);
    ASSERT_TRUE(root["transparencySortMode"]);
    TransparencySortMode mode = TransparencySortMode::DEFAULT;
    EXPECT_TRUE(SortingLayers::transparencySortModeFromToken(root["transparencySortMode"].as<std::string>(), mode));
    EXPECT_EQ(mode, TransparencySortMode::PERSPECTIVE);
    EXPECT_EQ(root["sortingLayers"].size(), 2u);
    EXPECT_EQ(result.log.lines.size(), 2u);
    ASSERT_TRUE(root["mesh"]["fbxSubmeshOrder"]);
    ModelProjectSettings meshSettings;
    decodeModelProjectSettings(root["mesh"], meshSettings);
    EXPECT_EQ(meshSettings.fbxSubmeshOrder, FbxSubmeshOrder::UsedMaterials);
    EXPECT_EQ(meshSettings.cacheSizeMB, 4096) << "the other Mesh settings stay";
    EXPECT_EQ(encodeModelProjectSettings(meshSettings)["fbxSubmeshOrder"].as<std::string>(), "used_materials");

    YAML::Node axis = YAML::Load("{formatVersion: 1, name: A, transparencySortMode: custom_axis}");
    StreamMigrations::migrate(DocumentKind::Project, axis);
    EXPECT_EQ(axis["transparencySortMode"].as<std::string>(), "custom_axis");

    EXPECT_EQ(axis["mesh"]["fbxSubmeshOrder"].as<std::string>(), "used_materials");

    YAML::Node current = YAML::Load("{formatVersion: 2, name: B}");
    EXPECT_FALSE(StreamMigrations::migrate(DocumentKind::Project, current).migrated());
    EXPECT_FALSE(current["transparencySortMode"]);
    EXPECT_FALSE(current["mesh"]);
    decodeModelProjectSettings(current["mesh"], meshSettings);
    EXPECT_EQ(meshSettings.fbxSubmeshOrder, FbxSubmeshOrder::MaterialSlots);
    EXPECT_FALSE(encodeModelProjectSettings(meshSettings)["fbxSubmeshOrder"]) << "the default is not written";
}

TEST(StreamMigrations, SceneV9ToV10KeepsTheCameraRenderMode){
    // S92: an absent htmlUiRenderMode is Screen Space - Overlay now; a scene saved before it drew
    // its screen documents in the camera pass, so it keeps Screen Space - Camera
    YAML::Node root = YAML::Load("{formatVersion: 9, scene: {backgroundColor: [0, 0, 0, 1]}, entities: []}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
    ASSERT_EQ(result.applied.size(), 2u) << "S92's 9 -> 10, then S94's 10 -> 11 (no texts or animators)";
    EXPECT_EQ(root["formatVersion"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Scene));
    EXPECT_EQ(root["scene"]["htmlUiRenderMode"].as<std::string>(), "camera");
    ASSERT_EQ(result.log.lines.size(), 1u);
    EXPECT_NE(result.log.lines[0].find("Screen Space - Camera"), std::string::npos) << result.log.lines[0];

    // a mode already written stays (a hand edit, or a key copied from a newer scene)
    YAML::Node overlay = YAML::Load("{formatVersion: 9, scene: {htmlUiRenderMode: overlay}, entities: []}");
    StreamMigrations::migrate(DocumentKind::Scene, overlay);
    EXPECT_EQ(overlay["scene"]["htmlUiRenderMode"].as<std::string>(), "overlay");

    // without a settings map nothing is added: the map would change how its other absent keys decode
    YAML::Node bare = YAML::Load("{formatVersion: 9, entities: []}");
    StreamMigrations::migrate(DocumentKind::Scene, bare);
    EXPECT_FALSE(bare["scene"]);

    // a current scene is not touched: absent is Overlay
    YAML::Node current = YAML::Load("{formatVersion: " + std::to_string(StreamMigrations::currentVersion(DocumentKind::Scene)) +
                                    ", scene: {backgroundColor: [0, 0, 0, 1]}, entities: []}");
    EXPECT_FALSE(StreamMigrations::migrate(DocumentKind::Scene, current).migrated());
    EXPECT_FALSE(current["scene"]["htmlUiRenderMode"]);
}

}  // namespace StreamMigrationsTest_cpp
