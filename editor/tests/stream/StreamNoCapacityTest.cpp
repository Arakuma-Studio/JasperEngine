// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A saved component reads back whole, however many elements its arrays hold (mesh-memory spec,
// Doriax 52179c98a): the decoders stopped at the arrays' starting sizes (16 submeshes, 128 frames,
// 200 tile rects, 2,048 tiles), so content an editor had grown past them was lost on reload.

#include "Stream.h"
#include "component/MeshComponent.h"
#include "component/PointsComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // what the scene file holds: the text, read back
    YAML::Node throughText(const YAML::Node& node){
        return YAML::Load(YAML::Dump(node));
    }

}

TEST(StreamNoCapacity, MeshKeepsEverySubmesh){
    MeshComponent mesh;
    mesh.numSubmeshes = 20;
    for (unsigned int s = 0; s < 20; s++)
        mesh.submeshes[s].material.name = "material" + std::to_string(s);
    mesh.submeshes[19].overrideFields = SubmeshOverride_BaseColorFactor;
    mesh.submeshes[19].material.baseColorFactor = Vector4(0.25f, 0.5f, 0.75f, 1.0f);

    const YAML::Node saved = throughText(Stream::encodeMeshComponent(mesh, false, false, false));
    MeshComponent read = Stream::decodeMeshComponent(saved);
    ASSERT_EQ(read.numSubmeshes, 20u);
    EXPECT_EQ(read.submeshes[16].material.name, "material16");
    EXPECT_EQ(read.submeshes[19].material.name, "material19");
    EXPECT_EQ(read.submeshes[19].overrideFields, (uint64_t)SubmeshOverride_BaseColorFactor);
    EXPECT_EQ(read.submeshes[19].material.baseColorFactor, Vector4(0.25f, 0.5f, 0.75f, 1.0f));

    // and saves the same again (the encode reads every submesh it counts)
    const MeshComponent& readOnly = read;
    EXPECT_EQ(YAML::Dump(Stream::encodeMeshComponent(readOnly, false, false, false)), YAML::Dump(saved));
}

TEST(StreamNoCapacity, SpriteTilemapPointsKeepEveryElement){
    SpriteComponent sprite;
    sprite.numFramesRect = 130;
    for (unsigned int i = 0; i < 130; i++)
        sprite.framesRect[i] = {"frame" + std::to_string(i), Rect((float)i, 0.0f, 8.0f, 8.0f)};
    SpriteComponent spriteRead = Stream::decodeSpriteComponent(throughText(Stream::encodeSpriteComponent(sprite)));
    ASSERT_EQ(spriteRead.numFramesRect, 130u);
    EXPECT_EQ(spriteRead.framesRect[129].name, "frame129");

    TilemapComponent tilemap;
    tilemap.numTilesRect = 201;
    for (unsigned int i = 0; i < 201; i++)
        tilemap.tilesRect[i] = {"rect" + std::to_string(i), 0, Rect(0.0f, 0.0f, 16.0f, 16.0f)};
    tilemap.numTiles = 2049;
    for (unsigned int i = 0; i < 2049; i++)
        tilemap.tiles[i] = {"tile" + std::to_string(i), (int)(i % 201), Vector2((float)i, 0.0f), 16.0f, 16.0f};
    TilemapComponent tilemapRead = Stream::decodeTilemapComponent(throughText(Stream::encodeTilemapComponent(tilemap)));
    ASSERT_EQ(tilemapRead.numTilesRect, 201u);
    EXPECT_EQ(tilemapRead.tilesRect[200].name, "rect200");
    ASSERT_EQ(tilemapRead.numTiles, 2049u);
    EXPECT_EQ(tilemapRead.tiles[2048].name, "tile2048");

    PointsComponent points;
    points.numFramesRect = 130;
    for (unsigned int i = 0; i < 130; i++)
        points.framesRect[i] = {"point" + std::to_string(i), Rect(0.0f, 0.0f, 4.0f, 4.0f)};
    PointsComponent pointsRead = Stream::decodePointsComponent(throughText(Stream::encodePointsComponent(points)));
    ASSERT_EQ(pointsRead.numFramesRect, 130u);
    EXPECT_EQ(pointsRead.framesRect[129].name, "point129");

    SpriteAnimationComponent animation;
    animation.framesSize = 130;
    animation.framesTimeSize = 130;
    for (int i = 0; i < 130; i++){
        animation.frames[i] = i;
        animation.framesTime[i] = 100 + i;
    }
    SpriteAnimationComponent animationRead =
        Stream::decodeSpriteAnimationComponent(throughText(Stream::encodeSpriteAnimationComponent(animation)));
    ASSERT_EQ(animationRead.framesSize, 130u);
    ASSERT_EQ(animationRead.framesTimeSize, 130u);
    EXPECT_EQ(animationRead.frames[129], 129);
    EXPECT_EQ(animationRead.framesTime[129], 229);
}

// Past ARRAY_INDEX_LIMIT a count is a damaged file, not content: the rest is left out
TEST(StreamNoCapacity, IndexLimitDropsTheRest){
    YAML::Node node;
    YAML::Node frames(YAML::NodeType::Sequence);
    for (size_t i = 0; i <= ARRAY_INDEX_LIMIT; i++) frames.push_back(1);
    node["frames"] = frames;
    SpriteAnimationComponent animation = Stream::decodeSpriteAnimationComponent(node);
    EXPECT_EQ(animation.framesSize, ARRAY_INDEX_LIMIT);
    EXPECT_EQ(animation.frames.stored(), ARRAY_INDEX_LIMIT);
}
