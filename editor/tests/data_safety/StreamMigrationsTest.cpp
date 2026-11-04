// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The migration framework itself (docs/specs/S03-data-safety.md R20-R23); the loaders' use of
// it is MigrationsTest.cpp

#include "StreamMigrations.h"
#include "Stream.h"
#include "component/ParticleSystemComponent.h"
#include "pool/ShaderPool.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {
    const DocumentKind kKinds[] = {DocumentKind::Scene, DocumentKind::Project, DocumentKind::Material, DocumentKind::Prefab};
}

TEST(StreamMigrations, TablesAreOrderedAndReachCurrent){
    for (DocumentKind kind : kKinds){
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
    EXPECT_EQ(result.versionCurrent, 8);
    EXPECT_TRUE(result.migrated());
    EXPECT_FALSE(result.newer());
    ASSERT_TRUE(root.IsMap());
    EXPECT_EQ(root.begin()->first.Scalar(), "formatVersion");
    EXPECT_EQ(root["formatVersion"].as<int>(), 8);
    EXPECT_EQ(root["name"].as<std::string>(), "Level1");
    EXPECT_EQ(root["entities"][0]["name"].as<std::string>(), "A");
    EXPECT_EQ(root.size(), 3u);

    // current: nothing to do
    const MigrationResult again = StreamMigrations::migrate(DocumentKind::Scene, root);
    EXPECT_FALSE(again.migrated());
    EXPECT_EQ(again.versionRead, 8);
}

TEST(StreamMigrations, NewerIsLeftUntouched){
    YAML::Node root = YAML::Load("{formatVersion: 99, name: Future}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
    EXPECT_TRUE(result.newer());
    EXPECT_FALSE(result.migrated());
    EXPECT_EQ(root["formatVersion"].as<int>(), 99);
    EXPECT_EQ(StreamMigrations::newerFormatMessage(DocumentKind::Scene, 9),
            "Written by a newer JasperEngine (scene format 9; this editor writes 8).");
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
    // controllers: none here) and 6 -> 7 (S27's curve form: no lines)
    ASSERT_EQ(result.applied.size(), 7u);
    EXPECT_EQ(root["formatVersion"].as<int>(), 8);
    EXPECT_EQ(root["scene"]["shadowQuality"].as<std::string>(), "auto");
    EXPECT_FLOAT_EQ(root["entities"][0]["components"]["light"]["shadowBias"].as<float>(), 1.0f);
    EXPECT_FLOAT_EQ(root["entities"][1]["children"][0]["components"]["light"]["shadowBias"].as<float>(), 1.0f);
    EXPECT_FLOAT_EQ(root["entities"][2]["components"]["light"]["shadowBias"].as<float>(), 1.0f);
    // one line for the quality, one for the biases (two lights reset)
    ASSERT_EQ(result.log.lines.size(), 2u);
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
    EXPECT_EQ(root["version"].as<int>(), 9);
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
    // 2 -> 3, then S29's 3 -> 4, S18's identity 4 -> 5, S21's 5 -> 6 and S27's curve form 6 -> 7
    ASSERT_EQ(result.applied.size(), 6u);
    EXPECT_EQ(root["formatVersion"].as<int>(), 8);

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

TEST(StreamMigrations, PrefabV4ToV5ParticleSystems){
    YAML::Node root = YAML::Load(
        "{type: Prefab, version: 4, members: ["
        "{type: Entity, entity: 1001, name: Dust, components: {transform: {position: [0, 0, 0]}, points: {points: []}}},"
        "{type: Entity, entity: 1002, name: Dust FX, components: {action: {target: 1001}, particlesystem: {rate: 9}}}]}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Prefab, root);
    EXPECT_TRUE(result.migrated());
    EXPECT_EQ(root["version"].as<int>(), 9) << "migrated to the current version";
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
    ASSERT_EQ(result.applied.size(), 3u) << "5 -> 6, S27's curve form 6 -> 7 (no lines), then S44's sheet sprites 7 -> 8";
    EXPECT_EQ(root["formatVersion"].as<int>(), 8);

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
    ASSERT_EQ(result.log.lines.size(), 1u);
    EXPECT_NE(result.log.lines[0].find("2 players got an Animator"), std::string::npos) << result.log.lines[0];

    // a prefab's members the same way (6 -> 7, then the curve form 7 -> 8)
    YAML::Node prefab = YAML::Load(
        "{type: Prefab, version: 6, members: [{type: Entity, entity: 1001, components: {animationplayer: {enabled: false, controller: anim/c.controller}}}]}");
    const MigrationResult prefabResult = StreamMigrations::migrate(DocumentKind::Prefab, prefab);
    EXPECT_TRUE(prefabResult.migrated());
    EXPECT_EQ(prefab["version"].as<int>(), 9);
    EXPECT_EQ(prefab["members"][0]["components"]["animator"]["controller"].as<std::string>(), "anim/c.controller");
    EXPECT_FALSE(prefab["members"][0]["components"]["animator"]["enabled"].as<bool>()) << "a disabled player: a disabled Animator";
}
