// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inline frames and the Sliced key (docs/specs/S45-sprite-atlas.md AC8, R8, R20, R40): a legacy
// sprite with 300 inline frames loads and saves them all (no 128-frame cap), the generated scene
// code sizes the list before writing the rects, drawMode round-trips and is written only when
// not Simple (a file without it is Simple, and an editor without S45 keeps it as unknown data:
// the key is additive, no format bump). The version-bump migration test is S44's.

#include "support/EditorTestSupport.h"

#include "Factory.h"
#include "Stream.h"
#include "StreamMigrations.h"
#include "component/SpriteComponent.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    YAML::Node& spriteNode(YAML::Node& root) {
        static YAML::Node node;
        for (YAML::Node entity : root["entities"]) {
            if (entity["name"] && entity["name"].as<std::string>() == "Hero") {
                node.reset(entity["components"]["sprite"]);
                return node;
            }
        }
        return node;
    }

    Entity heroOf(const DecodedScene& decoded) {
        for (Entity e : decoded.scene->entities) {
            if (decoded.scene->scene->getEntityName(e) == "Hero") return e;
        }
        return NULL_ENTITY;
    }

}

TEST(SpriteInlineFrames, ThreeHundredFramesLoadAndSave) {
    YAML::Node root = YAML::Load(readText(fixture("migrations/scene-v7-sprites.scene")));
    YAML::Node& sprite = spriteNode(root);
    YAML::Node frames(YAML::NodeType::Sequence);
    for (int i = 0; i < 300; i++) {
        YAML::Node f;
        f["name"] = "f" + std::to_string(i);
        YAML::Node rect(YAML::NodeType::Sequence);
        rect.SetStyle(YAML::EmitterStyle::Flow);
        for (int v : {(i % 20) * 16, (i / 20) * 16, 16, 16}) rect.push_back(v);
        f["rect"] = rect;
        frames.push_back(f);
    }
    sprite["framesRect"] = frames;
    DecodedScene decoded = decodeSceneText(YAML::Dump(root));
    const Entity hero = heroOf(decoded);
    ASSERT_NE(hero, NULL_ENTITY);
    const SpriteComponent& component = decoded.scene->scene->getComponent<SpriteComponent>(hero);
    ASSERT_EQ(component.numFramesRect, 300u);
    EXPECT_EQ(component.framesRect[299].name, "f299");
    EXPECT_GE(component.framesRect.stored(), 300u);
    YAML::Node saved = encodeScene(decoded);
    YAML::Node& savedSprite = spriteNode(saved);
    ASSERT_TRUE(savedSprite["framesRect"]);
    EXPECT_EQ(savedSprite["framesRect"].size(), 300u);
    EXPECT_EQ(savedSprite["framesRect"][299]["name"].as<std::string>(), "f299");

    // the generated scene code sizes the list first, then writes every rect
    const std::string code = Factory::createSpriteComponent(4, decoded.scene->scene, hero, "scene", "hero", false);
    const size_t resize = code.find("sprite.framesRect.resize(300);");
    ASSERT_NE(resize, std::string::npos) << code.substr(0, 400);
    EXPECT_LT(resize, code.find("sprite.framesRect[0]"));
    EXPECT_NE(code.find("sprite.framesRect[299].rect"), std::string::npos);
    EXPECT_EQ(code.find("drawMode"), std::string::npos) << "a Simple sprite emits no draw mode";
}

TEST(SpriteInlineFrames, SlicedDrawModeIsAdditive) {
    YAML::Node root = YAML::Load(readText(fixture("migrations/scene-v7-sprites.scene")));
    root["formatVersion"] = StreamMigrations::currentVersion(DocumentKind::Scene);
    {
        // without the key: Simple, and saving writes none
        DecodedScene decoded = decodeSceneText(YAML::Dump(root));
        EXPECT_EQ(decoded.scene->scene->getComponent<SpriteComponent>(heroOf(decoded)).drawMode, SpriteDrawMode::SIMPLE);
        YAML::Node saved = encodeScene(decoded);
        EXPECT_FALSE(spriteNode(saved)["drawMode"]);
    }
    spriteNode(root)["drawMode"] = "sliced";
    DecodedScene decoded = decodeSceneText(YAML::Dump(root));
    EXPECT_EQ(decoded.report.dropped(), 0u);
    const Entity hero = heroOf(decoded);
    EXPECT_EQ(decoded.scene->scene->getComponent<SpriteComponent>(hero).drawMode, SpriteDrawMode::SLICED);
    YAML::Node saved = encodeScene(decoded);
    ASSERT_TRUE(spriteNode(saved)["drawMode"]);
    EXPECT_EQ(spriteNode(saved)["drawMode"].as<std::string>(), "sliced");
    // an optional key: the format version does not move for it
    EXPECT_EQ(saved["formatVersion"].as<int>(), StreamMigrations::currentVersion(DocumentKind::Scene));
    EXPECT_NE(Factory::createSpriteComponent(4, decoded.scene->scene, hero, "scene", "hero", false).find("sprite.drawMode = SpriteDrawMode::SLICED;"),
              std::string::npos);

    // an editor that does not know the key (S03): it stays as unknown data, written back as it was
    YAML::Node unknown = root;
    spriteNode(unknown)["drawMode"] = "sliced";
    spriteNode(unknown)["someFutureKey"] = 7;
    DecodedScene withFuture = decodeSceneText(YAML::Dump(unknown));
    YAML::Node again = encodeScene(withFuture);
    ASSERT_TRUE(spriteNode(again)["someFutureKey"]);
    EXPECT_EQ(spriteNode(again)["someFutureKey"].as<int>(), 7);
}
