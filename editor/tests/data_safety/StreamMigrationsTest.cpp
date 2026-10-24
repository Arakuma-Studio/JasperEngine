// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The migration framework itself (docs/specs/S03-data-safety.md R20-R23); the loaders' use of
// it is MigrationsTest.cpp

#include "StreamMigrations.h"

#include <gtest/gtest.h>

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
    EXPECT_EQ(result.versionCurrent, 2);
    EXPECT_TRUE(result.migrated());
    EXPECT_FALSE(result.newer());
    ASSERT_TRUE(root.IsMap());
    EXPECT_EQ(root.begin()->first.Scalar(), "formatVersion");
    EXPECT_EQ(root["formatVersion"].as<int>(), 2);
    EXPECT_EQ(root["name"].as<std::string>(), "Level1");
    EXPECT_EQ(root["entities"][0]["name"].as<std::string>(), "A");
    EXPECT_EQ(root.size(), 3u);

    // current: nothing to do
    const MigrationResult again = StreamMigrations::migrate(DocumentKind::Scene, root);
    EXPECT_FALSE(again.migrated());
    EXPECT_EQ(again.versionRead, 2);
}

TEST(StreamMigrations, NewerIsLeftUntouched){
    YAML::Node root = YAML::Load("{formatVersion: 99, name: Future}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
    EXPECT_TRUE(result.newer());
    EXPECT_FALSE(result.migrated());
    EXPECT_EQ(root["formatVersion"].as<int>(), 99);
    EXPECT_EQ(StreamMigrations::newerFormatMessage(DocumentKind::Scene, 3),
            "Written by a newer JasperEngine (scene format 3; this editor writes 2).");
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
    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_EQ(root["formatVersion"].as<int>(), 2);
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
    EXPECT_EQ(root["version"].as<int>(), 4);
    EXPECT_FLOAT_EQ(root["members"][0]["components"]["light"]["shadowBias"].as<float>(), 1.0f);
}
