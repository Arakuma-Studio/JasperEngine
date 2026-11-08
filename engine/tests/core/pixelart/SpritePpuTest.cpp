// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S41 AC12 (R6): Pixels Per Unit. A sprite's and a tilemap's quads are their pixel sizes over
// their Pixels Per Unit (0: the project's); at PPU 0 and 1 the vertices are the pixel sizes, as
// before S41, and spriteWorldSize is what world-size readers use. A sheet Sprite (S44) takes its
// sheet's Pixels Per Unit unless it has its own.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "object/Camera.h"
#include "object/Sprite.h"
#include "object/Tilemap.h"
#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetPool.h"
#include "subsystem/MeshSystem.h"
#include "texture/TextureData.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;

namespace {

    const std::vector<uint8_t>& ppuTestPixels(){
        static const std::vector<uint8_t> pixels(16 * 16 * 4, 255);
        return pixels;
    }

    TextureData ppuTestTexture(){
        return TextureData(16, 16, 16 * 16 * 4, ColorFormat::RGBA, 4, (void*)ppuTestPixels().data());
    }

    // restores the project's Pixels Per Unit whatever the test sets
    struct ProjectPixelsPerUnit {
        explicit ProjectPixelsPerUnit(int ppu){ Engine::setPixelsPerUnit(ppu); }
        ~ProjectPixelsPerUnit(){ Engine::setPixelsPerUnit(1); }
    };

    // the sprite's quad, built by its MeshSystem
    std::vector<uint8_t> spriteVertices(float spritePpu, int projectPpu, PivotPreset pivot, AABB* bounds = nullptr){
        ProjectPixelsPerUnit project(projectPpu);
        Scene scene;
        Camera camera(&scene);
        camera.setType(CameraType::CAMERA_ORTHO);
        camera.activate();
        Sprite sprite(&scene);
        sprite.setTexture("pixelart|ppu|white", ppuTestTexture());
        sprite.setSize(16, 16);
        sprite.setPivotPreset(pivot);
        sprite.setPixelsPerUnit(spritePpu);
        scene.getSystem<MeshSystem>()->update(0);
        const MeshComponent& mesh = scene.getComponent<MeshComponent>(sprite.getEntity());
        if (bounds)
            *bounds = mesh.aabb;
        return std::vector<uint8_t>(mesh.buffer.getData(), mesh.buffer.getData() + mesh.buffer.getSize());
    }

}

TEST(SpritePpuTest, QuadIsPixelsOverPixelsPerUnit){
    AABB bounds;
    // project PPU 16: a 16 x 16 sprite is one unit
    spriteVertices(0.0f, 16, PivotPreset::BOTTOM_LEFT, &bounds);
    EXPECT_FLOAT_EQ(bounds.getMinimum().x, 0.0f);
    EXPECT_FLOAT_EQ(bounds.getMaximum().x, 1.0f);
    EXPECT_FLOAT_EQ(bounds.getMaximum().y, 1.0f);
    // its own PPU 32 wins: half a unit
    spriteVertices(32.0f, 16, PivotPreset::BOTTOM_LEFT, &bounds);
    EXPECT_FLOAT_EQ(bounds.getMaximum().x, 0.5f);
    EXPECT_FLOAT_EQ(bounds.getMaximum().y, 0.5f);
    // the pivot is in the same units
    spriteVertices(16.0f, 1, PivotPreset::CENTER, &bounds);
    EXPECT_FLOAT_EQ(bounds.getMinimum().x, -0.5f);
    EXPECT_FLOAT_EQ(bounds.getMaximum().x, 0.5f);
}

// PPU 0 (the project's 1) and 1 give the vertices of before: the pixel sizes, byte for byte
TEST(SpritePpuTest, DefaultPixelsPerUnitKeepsTheVertices){
    const std::vector<uint8_t> unset = spriteVertices(0.0f, 1, PivotPreset::CENTER);
    const std::vector<uint8_t> one = spriteVertices(1.0f, 1, PivotPreset::CENTER);
    ASSERT_FALSE(unset.empty());
    EXPECT_EQ(unset, one);
    AABB bounds;
    spriteVertices(0.0f, 1, PivotPreset::CENTER, &bounds);
    EXPECT_FLOAT_EQ(bounds.getMinimum().x, -8.0f);
    EXPECT_FLOAT_EQ(bounds.getMaximum().x, 8.0f);
    EXPECT_FLOAT_EQ(bounds.getMinimum().y, -8.0f);
}

TEST(SpritePpuTest, WorldSize){
    SpriteComponent sprite;
    sprite.width = 16;
    sprite.height = 8;
    EXPECT_EQ(spriteWorldSize(sprite), Vector2(16.0f, 8.0f));
    {
        ProjectPixelsPerUnit project(16);
        EXPECT_EQ(spriteWorldSize(sprite), Vector2(1.0f, 0.5f));
        sprite.pixelsPerUnit = 32.0f;
        EXPECT_EQ(spriteWorldSize(sprite), Vector2(0.5f, 0.25f));
    }
    EXPECT_FLOAT_EQ(resolvePixelsPerUnit(0.0f), 1.0f);
    EXPECT_FLOAT_EQ(resolvePixelsPerUnit(4.0f), 4.0f);
    // the project's is clamped to 1..256
    Engine::setPixelsPerUnit(0);
    EXPECT_EQ(Engine::getPixelsPerUnit(), 1);
    Engine::setPixelsPerUnit(1000);
    EXPECT_EQ(Engine::getPixelsPerUnit(), 256);
    Engine::setPixelsPerUnit(1);
}

TEST(SpritePpuTest, TilemapQuadsArePixelsOverPixelsPerUnit){
    ProjectPixelsPerUnit project(16);
    Scene scene;
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.activate();
    Tilemap tilemap(&scene);
    tilemap.setTexture("pixelart|ppu|white", ppuTestTexture());
    // its tileset is submesh 0 (a scene lists a tilemap's submeshes; addRect with a path adds one)
    scene.getComponent<MeshComponent>(tilemap.getEntity()).numSubmeshes = 1;
    tilemap.addRect(0, "tile", Rect(0, 0, 16, 16));
    tilemap.addTile(0, Vector2(0, 0), 16, 16);
    tilemap.addTile(0, Vector2(16, 0), 16, 16);
    // a tilemap builds once its texture's size is known (its load may finish on a worker)
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(tilemap.getEntity());
    const TilemapComponent& built = scene.getComponent<TilemapComponent>(tilemap.getEntity());
    for (int frame = 0; frame < 500 && built.needUpdateTilemap; frame++){
        scene.getSystem<MeshSystem>()->update(0);
        if (built.needUpdateTilemap)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_FALSE(built.needUpdateTilemap) << "the tilemap never built";
    SCOPED_TRACE(::testing::Message() << "tiles " << built.numTiles << ", rects " << built.numTilesRect << ", rendered "
                 << built.renderedTiles << ", vertices " << mesh.buffer.getCount() << ", submeshes " << mesh.numSubmeshes
                 << ", texture " << mesh.submeshes[0].material.baseColorTexture.getWidth());
    EXPECT_FLOAT_EQ(mesh.aabb.getMaximum().x, 2.0f);
    EXPECT_FLOAT_EQ(mesh.aabb.getMaximum().y, 1.0f);
}

TEST(SpritePpuTest, SheetSpriteTakesItsOwnOverTheSheets){
    const std::string path = "pixelart-test://ppu-sheet";
    SpriteSheetData data;
    data.pixelsPerUnit = 16.0f;
    data.pages.push_back({"pixelart|ppu|sheet-page", 16, 16});
    SpriteFrame frame;
    frame.name = "f0";
    frame.rect = Rect(0.0f, 0.0f, 16.0f, 16.0f);
    frame.sourceSize = Vector2(16.0f, 16.0f);
    data.frames.push_back(frame);
    SpriteSlice slice;
    slice.name = "box";
    SpriteSliceKey key;
    key.bounds = Rect(0.0f, 0.0f, 16.0f, 16.0f);
    slice.keys.push_back(key);
    data.slices.push_back(slice);
    auto sheet = std::make_shared<const SpriteSheetData>(data);
    SpriteSheetPool::registerSource([path](const std::string& p){ return p == path; }, [sheet](const std::string&){ return sheet; });

    // the quad's and the slice "box"'s width (a collider's slice is in the quad's units)
    auto bounds = [&](float spritePpu, float& sliceWidth){
        Scene scene;
        Camera camera(&scene);
        camera.setType(CameraType::CAMERA_ORTHO);
        camera.activate();
        Sprite sprite(&scene);
        sprite.setSheet(path);
        sprite.setFrame(0);
        sprite.setPivotPreset(PivotPreset::BOTTOM_LEFT);
        sprite.setPixelsPerUnit(spritePpu);
        scene.getSystem<MeshSystem>()->update(0);
        Rect slice;
        sliceWidth = sprite.getSliceRect("box", slice) ? slice.getWidth() : -1.0f;
        return scene.getComponent<MeshComponent>(sprite.getEntity()).aabb;
    };
    float sliceWidth = 0.0f;
    // PPU 0: the sheet's 16, a 16 x 16 frame is one unit (the project's 1 is not used)
    AABB box = bounds(0.0f, sliceWidth);
    EXPECT_FLOAT_EQ(box.getMaximum().x, 1.0f);
    EXPECT_FLOAT_EQ(box.getMaximum().y, 1.0f);
    EXPECT_FLOAT_EQ(sliceWidth, 1.0f);
    // its own 32 wins: half a unit
    box = bounds(32.0f, sliceWidth);
    EXPECT_FLOAT_EQ(box.getMaximum().x, 0.5f);
    EXPECT_FLOAT_EQ(box.getMaximum().y, 0.5f);
    EXPECT_FLOAT_EQ(sliceWidth, 0.5f);
    SpriteSheetPool::invalidate(path);
}
