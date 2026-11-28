// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sprite skin authoring (docs/specs/S94-rendering-anim-gaps.md F49, AC46): Auto Weights gives
// every vertex weights that sum to 1 with the nearest bone the heaviest; Auto Geometry covers the
// opaque pixels and nothing of the empty frame around them; removing a bone hands its weights to
// its parent; set_weights at 1 binds a part rigidly; the Weight Brush moves weight toward its
// bone; the meta form round-trips.

#include <gtest/gtest.h>

#include "sprite/SpriteSkinEdit.h"

#include <cmath>

namespace SpriteSkinEditTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // a 3 x 5 grid of vertices over a 16 x 32 frame, two bones up its middle
    SpriteSkinGeometry column() {
        SpriteSkinGeometry skin;
        skin.bones.push_back({"lower", -1, Vector2(8, 32), Vector2(8, 16)});
        skin.bones.push_back({"upper", 0, Vector2(8, 16), Vector2(8, 0)});
        for (int r = 0; r <= 4; r++) {
            for (int c = 0; c <= 2; c++) skin.vertices.emplace_back(static_cast<float>(c * 8), static_cast<float>(r * 8));
        }
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 2; c++) {
                const uint16_t a = static_cast<uint16_t>(r * 3 + c);
                skin.indices.insert(skin.indices.end(), {a, static_cast<uint16_t>(a + 1), static_cast<uint16_t>(a + 4), a,
                                                          static_cast<uint16_t>(a + 4), static_cast<uint16_t>(a + 3)});
            }
        }
        return skin;
    }

    float sum(const SpriteVertexWeights& w) {
        float s = 0.0f;
        for (float x : w.weights) s += x;
        return s;
    }

    float weightOf(const SpriteVertexWeights& w, int bone) {
        float s = 0.0f;
        for (int k = 0; k < SPRITE_SKIN_MAX_INFLUENCES; k++) {
            if (w.bones[k] == bone) s += w.weights[k];
        }
        return s;
    }

}

TEST(SpriteSkinTest, AutoWeightsNormalised) {
    SpriteSkinGeometry skin = column();
    std::string error;
    ASSERT_TRUE(sprite::autoWeights(skin, error)) << error;
    ASSERT_EQ(skin.weights.size(), skin.vertices.size());
    for (size_t v = 0; v < skin.weights.size(); v++) {
        EXPECT_NEAR(sum(skin.weights[v]), 1.0f, 1e-5f) << "vertex " << v;
    }
    // the top row follows the upper bone, the bottom row the lower one, the joint row both
    EXPECT_GT(weightOf(skin.weights[1], 1), 0.9f);
    EXPECT_GT(weightOf(skin.weights[13], 0), 0.9f);
    EXPECT_NEAR(weightOf(skin.weights[7], 0), weightOf(skin.weights[7], 1), 1e-4f);

    SpriteSkinGeometry boneless;
    boneless.vertices = skin.vertices;
    EXPECT_FALSE(sprite::autoWeights(boneless, error));
    EXPECT_NE(error.find("bones"), std::string::npos);
}

TEST(SpriteSkinTest, AutoGeometryCoversTheOpaquePixels) {
    // a 32 x 32 page whose frame (the whole page) has an opaque 8 x 12 block at (12, 10)
    editor::pixelart::PixelImage page(32, 32);
    for (int y = 10; y < 22; y++) {
        for (int x = 12; x < 20; x++) page.set(x, y, {255, 255, 255, 255});
    }
    SpriteFrame frame;
    frame.name = "f";
    frame.rect = Rect(0, 0, 32, 32);
    frame.sourceSize = Vector2(32, 32);
    SpriteSkinGeometry skin;
    sprite::AutoGeometrySettings settings;
    settings.cellSize = 4;
    std::string error;
    ASSERT_TRUE(sprite::autoGeometry(page, frame, settings, skin, error)) << error;
    ASSERT_GE(skin.indices.size(), 6u);
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (const Vector2& v : skin.vertices) {
        minX = std::min(minX, v.x); minY = std::min(minY, v.y);
        maxX = std::max(maxX, v.x); maxY = std::max(maxY, v.y);
    }
    // the cells holding the block, on the 4 px grid: (12, 8)..(20, 24)
    EXPECT_EQ(minX, 12.0f);
    EXPECT_EQ(minY, 8.0f);
    EXPECT_EQ(maxX, 20.0f);
    EXPECT_EQ(maxY, 24.0f);
    EXPECT_TRUE(skin.weights.empty());

    editor::pixelart::PixelImage empty(32, 32);
    EXPECT_FALSE(sprite::autoGeometry(empty, frame, settings, skin, error));
}

TEST(SpriteSkinTest, RemovingABoneHandsItsWeightsToItsParent) {
    SpriteSkinGeometry skin = column();
    skin.bones.push_back({"hat", 1, Vector2(8, 0), Vector2(8, -4)});
    std::string error;
    ASSERT_TRUE(sprite::autoWeights(skin, error));
    ASSERT_TRUE(sprite::removeBone(skin, "upper", error)) << error;
    ASSERT_EQ(skin.bones.size(), 2u);
    EXPECT_EQ(skin.bones[1].name, "hat");
    EXPECT_EQ(skin.bones[1].parent, 0) << "the child goes to the removed bone's parent";
    for (const SpriteVertexWeights& w : skin.weights) {
        EXPECT_NEAR(sum(w), 1.0f, 1e-5f);
        for (int k = 0; k < SPRITE_SKIN_MAX_INFLUENCES; k++) {
            if (w.weights[k] > 0.0f) {
                EXPECT_LT(w.bones[k], 2);
            }
        }
    }
    EXPECT_FALSE(sprite::removeBone(skin, "nope", error));
}

TEST(SpriteSkinTest, SetWeightsBindsAtFullWeight) {
    SpriteSkinGeometry skin = column();
    std::string error;
    ASSERT_TRUE(sprite::autoWeights(skin, error));
    ASSERT_TRUE(sprite::setWeights(skin, {}, 1, 1.0f, error)) << error;
    for (const SpriteVertexWeights& w : skin.weights) EXPECT_FLOAT_EQ(weightOf(w, 1), 1.0f);
    ASSERT_TRUE(sprite::setWeights(skin, {0}, 0, 0.25f, error));
    EXPECT_NEAR(weightOf(skin.weights[0], 0), 0.25f, 1e-5f);
    EXPECT_NEAR(weightOf(skin.weights[0], 1), 0.75f, 1e-5f);
    EXPECT_FALSE(sprite::setWeights(skin, {99}, 0, 1.0f, error));
}

TEST(SpriteSkinTest, WeightBrushMovesWeightTowardItsBone) {
    SpriteSkinGeometry skin = column();
    std::string error;
    ASSERT_TRUE(sprite::setWeights(skin, {}, 0, 1.0f, error));
    const std::vector<int> changed = sprite::brushWeights(skin, Vector2(8, 0), 6.0f, 1, 0.5f, sprite::WeightBrushMode::Add);
    ASSERT_FALSE(changed.empty());
    EXPECT_GT(weightOf(skin.weights[1], 1), 0.2f) << "the vertex under the brush";
    EXPECT_FLOAT_EQ(weightOf(skin.weights[13], 1), 0.0f) << "a vertex outside it";
    for (const SpriteVertexWeights& w : skin.weights) EXPECT_NEAR(sum(w), 1.0f, 1e-5f);
}

TEST(SpriteSkinTest, MetaRoundTrip) {
    SpriteSkinGeometry skin = column();
    std::string error;
    ASSERT_TRUE(sprite::autoWeights(skin, error));
    sprite::SpriteSkinSet set;
    set.sprites["hero"] = std::make_shared<SpriteSkinGeometry>(skin);
    const YAML::Node node = sprite::skinSetToYaml(set);
    sprite::SpriteSkinSet read;
    ASSERT_TRUE(sprite::skinSetFromYaml(YAML::Load(YAML::Dump(node)), read, error)) << error;
    ASSERT_TRUE(read.find("hero"));
    EXPECT_EQ(sprite::skinSetKey(read), sprite::skinSetKey(set));

    YAML::Node newer = YAML::Clone(node);
    newer["version"] = 99;
    EXPECT_FALSE(sprite::skinSetFromYaml(newer, read, error));
    EXPECT_NE(error.find("newer"), std::string::npos);
}

}  // namespace SpriteSkinEditTest_cpp
