// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The sprite sheet keys' format bump (docs/specs/S44-aseprite-import.md R44, AC23): a scene and a
// prefab written before it load, and save at the new versions with their sprite sections as they
// were; a sheet sprite round-trips sheet, sheetGuid, frame and frameName. The versions come from
// StreamMigrations, so the test holds whichever number the bump takes.

#include "support/EditorTestSupport.h"

#include "DocumentFile.h"
#include "Stream.h"
#include "StreamMigrations.h"
#include "component/SpriteComponent.h"
#include "util/Prefab.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    YAML::Node componentOf(const YAML::Node& root, const std::string& entityName, const std::string& key) {
        for (const YAML::Node& entity : root["entities"]) {
            if (entity["name"] && entity["name"].as<std::string>() == entityName) return entity["components"][key];
        }
        return YAML::Node();
    }

}

TEST(SpriteSheetMigration, SceneBeforeTheBumpKeepsItsSprites) {
    const std::filesystem::path path = fixture("migrations/scene-v7-sprites.scene");
    const YAML::Node original = YAML::Load(readText(path));
    ASSERT_EQ(original["formatVersion"].as<int>(), 7);

    DecodedScene decoded = decodeSceneFile(path);
    EXPECT_EQ(decoded.report.dropped(), 0u);
    EXPECT_EQ(decoded.report.versionRead, 7);
    // the inline frames and the Sprite Animation load as before: a legacy sprite
    Scene& scene = *decoded.scene->scene;
    Entity hero = NULL_ENTITY;
    for (Entity e : decoded.scene->entities) {
        if (scene.getEntityName(e) == "Hero") hero = e;
    }
    ASSERT_NE(hero, NULL_ENTITY);
    const SpriteComponent& sprite = scene.getComponent<SpriteComponent>(hero);
    EXPECT_TRUE(sprite.sheet.empty());
    ASSERT_EQ(sprite.numFramesRect, 3u);
    EXPECT_EQ(sprite.framesRect[2].name, "walk_2");

    // saved at the current version; the sprite and Sprite Animation sections unchanged
    const YAML::Node saved = encodeScene(decoded);
    EXPECT_EQ(saved["formatVersion"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Scene));
    EXPECT_GT(StreamMigrations::currentVersion(DocumentKind::Scene), 7);
    std::string why;
    EXPECT_TRUE(structurallyEqual(componentOf(saved, "Hero", "sprite"), componentOf(original, "Hero", "sprite"), &why)) << why;
    EXPECT_TRUE(structurallyEqual(componentOf(saved, "Hero", "spriteanimation"), componentOf(original, "Hero", "spriteanimation"), &why)) << why;
    // the sprite's texture is still saved (a legacy sprite keeps its mesh texture)
    EXPECT_TRUE(componentOf(saved, "Hero", "mesh")["submeshes"][0]["material"]["baseColorTexture"]);
    // and a second save is byte for byte the first
    DecodedScene again = decodeSceneText(encodeSceneText(decoded));
    EXPECT_EQ(encodeSceneText(again), encodeSceneText(decoded));
}

TEST(SpriteSheetMigration, PrefabVersionsMoveTogether) {
    EXPECT_EQ(StreamMigrations::currentVersion(DocumentKind::Prefab), Prefab::FILE_VERSION);
    // an older prefab migrates through the identity step to the current version
    YAML::Node root = YAML::Load("{type: Prefab, version: 8, members: []}");
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Prefab, root);
    EXPECT_TRUE(result.migrated());
    EXPECT_EQ(root["version"].as<int>(), Prefab::FILE_VERSION);
    EXPECT_EQ(root["members"].size(), 0u);
}

// a prefab written before the bump (inline-frame Sprite and Sprite Animation) migrates to the
// current version with its sprite sections untouched, and decodes to the same frames
TEST(SpriteSheetMigration, PrefabBeforeTheBumpKeepsItsSprites) {
    const std::filesystem::path path = fixture("migrations/prefab-v8-sprites.prefab");
    const YAML::Node original = YAML::Load(readText(path));
    ASSERT_EQ(original["version"].as<int>(), 8);
    const DocumentFile prefab = DocumentFiles::read(path, DocumentKind::Prefab);
    EXPECT_EQ(prefab.migration.versionRead, 8);
    EXPECT_EQ(prefab.root["version"].as<int>(), Prefab::FILE_VERSION);
    const YAML::Node member = prefab.root["members"][0];
    std::string why;
    EXPECT_TRUE(structurallyEqual(member["components"]["sprite"], original["members"][0]["components"]["sprite"], &why)) << why;
    EXPECT_TRUE(structurallyEqual(member["components"]["spriteanimation"], original["members"][0]["components"]["spriteanimation"], &why)) << why;
    const SpriteComponent sprite = Stream::decodeSpriteComponent(member["components"]["sprite"]);
    EXPECT_TRUE(sprite.sheet.empty());
    ASSERT_EQ(sprite.numFramesRect, 3u);
    EXPECT_EQ(sprite.framesRect[2].name, "walk_2");
}

TEST(SpriteSheetMigration, SheetSpriteRoundTrips) {
    SpriteComponent sprite;
    sprite.sheet = AssetRef("sprites/hero.aseprite", Guid::parse("4b1e9c0d2a7f4e31b6c58d0f9e2a7c13"));
    sprite.frame = 3;
    sprite.frameName = "hero 3";
    sprite.pivotPreset = PivotPreset::FRAME;
    // inline frames of a sheet sprite are not saved
    sprite.numFramesRect = 1;
    sprite.framesRect[0].name = "ignored";
    const YAML::Node node = Stream::encodeSpriteComponent(sprite);
    EXPECT_EQ(node["sheet"].as<std::string>(), "sprites/hero.aseprite");
    EXPECT_EQ(node["sheetGuid"].as<std::string>(), "4b1e9c0d2a7f4e31b6c58d0f9e2a7c13");
    EXPECT_EQ(node["frame"].as<int>(), 3);
    EXPECT_EQ(node["frameName"].as<std::string>(), "hero 3");
    EXPECT_EQ(node["pivotPreset"].as<std::string>(), "frame");
    EXPECT_FALSE(node["framesRect"]);
    const SpriteComponent back = Stream::decodeSpriteComponent(node);
    EXPECT_EQ(back.sheet.path, sprite.sheet.path);
    EXPECT_EQ(back.sheet.guid, sprite.sheet.guid);
    EXPECT_EQ(back.frame, 3);
    EXPECT_EQ(back.frameName, "hero 3");
    EXPECT_EQ(back.pivotPreset, PivotPreset::FRAME);
    EXPECT_TRUE(back.resolveFrameName);
    // a legacy sprite writes none of the sheet keys
    const YAML::Node legacy = Stream::encodeSpriteComponent(SpriteComponent());
    EXPECT_FALSE(legacy["sheet"]);
    EXPECT_FALSE(legacy["frame"]);
    EXPECT_FALSE(legacy["frameName"]);
}
