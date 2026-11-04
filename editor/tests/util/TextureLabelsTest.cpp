// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What a texture slot shows (project-window spec 6.10, Doriax 2cb188fbc): the file's name, never
// the pool id; the path in the tooltip

#include "util/TextureLabels.h"

#include "texture/Texture.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(TextureLabels, AlphaBorderIdShowsTheFileName) {
    Texture texture;
    texture.setPath("sprites/ship.png");
    texture.setAlphaBorder(TextureAlphaBorder::FIX);
    ASSERT_EQ(texture.getId(), "sprites/ship.png|alphaborder");
    const TextureSlotLabel label = textureSlotLabel(texture, [](const std::string& path) { return "assets/" + path; });
    EXPECT_EQ(label.text, "ship.png");
    EXPECT_EQ(label.tooltip, "assets/sprites/ship.png");
}

TEST(TextureLabels, SvgScaleShowsTheFileName) {
    Texture texture;
    texture.setPath("ui/icon.svg?svgScale=2");
    EXPECT_EQ(textureSlotLabel(texture).text, "icon.svg");
}

TEST(TextureLabels, CubemapShowsFaceZeroAndAllPathsInTheTooltip) {
    Texture texture;
    // front, back, left, right, up, down: stored +X, -X, +Y, -Y, +Z, -Z
    texture.setCubePaths("sky/f.png", "sky/b.png", "sky/l.png", "sky/r.png", "sky/u.png", "sky/d.png");
    const TextureSlotLabel label = textureSlotLabel(texture);
    EXPECT_EQ(label.text, "r.png (Cubemap)");
    EXPECT_EQ(label.tooltip, "sky/r.png\nsky/l.png\nsky/u.png\nsky/d.png\nsky/f.png\nsky/b.png");
}

TEST(TextureLabels, NoPathShowsTheIdAndNothingShowsNotSet) {
    Texture texture;
    EXPECT_EQ(textureSlotLabel(texture).text, "< Not set >");
    texture.setId("generated:noise");
    EXPECT_EQ(textureSlotLabel(texture).text, "generated:noise");
}
