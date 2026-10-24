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
    EXPECT_EQ(result.versionCurrent, 1);
    EXPECT_TRUE(result.migrated());
    EXPECT_FALSE(result.newer());
    ASSERT_TRUE(root.IsMap());
    EXPECT_EQ(root.begin()->first.Scalar(), "formatVersion");
    EXPECT_EQ(root["formatVersion"].as<int>(), 1);
    EXPECT_EQ(root["name"].as<std::string>(), "Level1");
    EXPECT_EQ(root["entities"][0]["name"].as<std::string>(), "A");
    EXPECT_EQ(root.size(), 3u);

    // current: nothing to do
    const MigrationResult again = StreamMigrations::migrate(DocumentKind::Scene, root);
    EXPECT_FALSE(again.migrated());
    EXPECT_EQ(again.versionRead, 1);
}

TEST(StreamMigrations, NewerIsLeftUntouched){
    YAML::Node root = YAML::Load("{formatVersion: 99, name: Future}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Scene, root);
    EXPECT_TRUE(result.newer());
    EXPECT_FALSE(result.migrated());
    EXPECT_EQ(root["formatVersion"].as<int>(), 99);
    EXPECT_EQ(StreamMigrations::newerFormatMessage(DocumentKind::Scene, 2),
            "Written by a newer JasperEngine (scene format 2; this editor writes 1).");
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
    EXPECT_EQ(it->second.as<int>(), 1);
    ++it;
    EXPECT_EQ(it->first.Scalar(), "a");
    ++it;
    EXPECT_EQ(it->first.Scalar(), "b");
    EXPECT_EQ(root.size(), 3u);
}
