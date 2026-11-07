// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A texture's id names its pool entry: a file with the alpha border fix is another entry than
// the same file without it, and a texture with no file has no id at all (Doriax 454d43ae5)

#include "texture/Texture.h"

#include <gtest/gtest.h>

using namespace jasperengine;

TEST(TextureId, EmptyPathWithAlphaBorderHasNoId) {
    Texture texture;
    texture.setAlphaBorder(TextureAlphaBorder::FIX);
    texture.setPath("");
    EXPECT_EQ(texture.getId(), "");
    texture.setAlphaBorderAuto(true);
    texture.setAlphaBorder(TextureAlphaBorder::AUTO);
    EXPECT_EQ(texture.getId(), "");
}

TEST(TextureId, PathWithAlphaBorderKeepsItsSuffix) {
    Texture texture;
    texture.setPath("sprites/ship.png");
    EXPECT_EQ(texture.getId(), "sprites/ship.png");
    texture.setAlphaBorder(TextureAlphaBorder::FIX);
    EXPECT_EQ(texture.getId(), "sprites/ship.png|alphaborder");
    EXPECT_EQ(texture.getPath(0), "sprites/ship.png");
}
