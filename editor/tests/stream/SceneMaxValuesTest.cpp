// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A scene's maxValues block (mesh-memory spec D8): the fixed array capacities and collider caps
// an older editor wrote. Nothing reads it any more; a scene that has one saves it as it was (no
// churn in a user's file), and a scene that has none (one made since) writes none.

#include "support/EditorTestSupport.h"

#include "Project.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const char* const OLD_SCENE = R"(formatVersion: 7
id: 1
name: Old
scene: {}
sceneType: scene_3d
mainCamera: 0
maxValues:
  maxSubmeshes: 24
  maxTilemapTilesRect: 0
  maxTilemapTiles: 0
  maxExternalBuffers: 31
  maxSpriteFrames: 0
  maxBones: 128
  maxColliders2D: 3
)";

}

TEST(SceneMaxValues, OldBlockSavedUnchanged) {
    test::DecodedScene decoded = test::decodeSceneText(OLD_SCENE, "Old.scene");
    ASSERT_TRUE(decoded.scene);
    const YAML::Node saved = test::encodeScene(decoded);
    ASSERT_TRUE(saved["maxValues"]) << "an older editor's block is kept";
    const YAML::Node old = YAML::Load(OLD_SCENE)["maxValues"];
    std::string why;
    EXPECT_TRUE(test::structurallyEqual(saved["maxValues"], old, &why)) << why;
}

TEST(SceneMaxValues, NewSceneWritesNone) {
    std::string text = OLD_SCENE;
    text = text.substr(0, text.find("maxValues:"));
    test::DecodedScene decoded = test::decodeSceneText(text, "New.scene");
    ASSERT_TRUE(decoded.scene);
    EXPECT_FALSE(test::encodeScene(decoded)["maxValues"]) << "no capacities to write";
}
