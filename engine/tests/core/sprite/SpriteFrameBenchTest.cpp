// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// R25-R26 (docs/specs/S45-sprite-atlas.md AC14): 1,000 sheet sprites switching between trimmed
// frames every frame cost at most 0.5 ms of CPU a frame (enforced with the benchmark run's
// perfSlack(), printed elsewhere) and allocate nothing; frame data is shared per sheet, so a
// sheet sprite's component holds no frames of its own, and a legacy sprite only the frames it has.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "object/Camera.h"
#include "object/Sprite.h"
#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetPool.h"
#include "subsystem/MeshSystem.h"

#include "AllocationCounter.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using namespace jasperengine;

namespace {

    // eight 16 x 16 frames, each trimmed differently (so each swap rebuilds the quad)
    std::shared_ptr<const SpriteSheetData> trimmedSheet() {
        auto sheet = std::make_shared<SpriteSheetData>();
        sheet->pages.push_back({"bench-page.png", 128, 32});
        for (int i = 0; i < 8; i++) {
            SpriteFrame f;
            f.name = "f" + std::to_string(i);
            f.sourceSize = Vector2(16, 16);
            f.offset = Vector2(static_cast<float>(i % 3), static_cast<float>(i % 4));
            f.rect = Rect(static_cast<float>(i * 16), 0.0f, static_cast<float>(16 - i % 3 - 1), static_cast<float>(16 - i % 4 - 2));
            f.pivot = Vector2(8, 16);
            sheet->frames.push_back(f);
        }
        return sheet;
    }

}

TEST(SpriteFrameBench, ThousandSpritesSwitchFramesWithoutAllocating) {
    const std::string path = "bench-test://trimmed";
    auto sheet = trimmedSheet();
    SpriteSheetPool::registerSource([path](const std::string& p) { return p == path; }, [sheet](const std::string&) { return sheet; });

    Scene scene;
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.activate();
    std::shared_ptr<MeshSystem> meshes = scene.getSystem<MeshSystem>();
    ASSERT_NE(meshes, nullptr);
    std::vector<std::unique_ptr<Sprite>> sprites;
    for (int i = 0; i < 1000; i++) {
        auto sprite = std::make_unique<Sprite>(&scene);
        sprite->setSheet(path);
        sprite->setFrame(i % 8);
        sprites.push_back(std::move(sprite));
    }
    auto step = [&](int frame) {
        for (size_t i = 0; i < sprites.size(); i++) {
            SpriteComponent& sprite = sprites[i]->getComponent<SpriteComponent>();
            sprite.frame = static_cast<int>((frame + i) % 8);
            meshes->createOrUpdateSprite(sprite, sprites[i]->getComponent<MeshComponent>());
        }
    };
    // warm: the sheet loaded, every buffer at its size
    for (int f = 0; f < 16; f++) step(f);
    for (const auto& sprite : sprites) {
        ASSERT_EQ(sprite->getComponent<SpriteComponent>().builtFrame, sprite->getComponent<SpriteComponent>().frame);
    }

    constexpr int kFrames = 120;
    uint64_t allocations = 0;
    const auto start = std::chrono::steady_clock::now();
    {
        testsupport::ThreadAllocationCount count;
        for (int f = 0; f < kFrames; f++) step(f + 1);
        allocations = count.count();
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kFrames;
    std::printf("[bench] SpriteFrameBench: 1000 trimmed frame swaps %.3f ms a frame (budget 0.5 ms), %llu allocations\n", ms,
                static_cast<unsigned long long>(allocations));
    if (testsupport::countsAllocations()) {
        EXPECT_EQ(allocations, 0u) << "a frame change allocates nothing";
    }
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LT(ms, 0.5 * testsupport::perfSlack());
    }
    sprites.clear();
    SpriteSheetPool::invalidate(path);
}

// R25: the frames are the sheet's (shared), a sheet sprite holds none; a legacy sprite pays for
// the frames it has only. AC14: a sheet sprite costs at most 64 bytes more than a frameless one
// (its component and what it holds outside it: the shared sheet is not its own)
namespace {

    // a component's own bytes: itself, its arrays, its strings past the small-string buffer
    size_t spriteBytes(const SpriteComponent& sprite) {
        const std::string empty;
        auto heap = [&](const std::string& text) { return text.capacity() > empty.capacity() ? text.capacity() + 1 : 0; };
        return sizeof(SpriteComponent) + sprite.arrayHeapBytes() + heap(sprite.sheet.path) + heap(sprite.frameName) + heap(sprite.loadedSheet) +
               heap(sprite.playingTag) + sprite.tagFrames.capacity() * sizeof(int) + (sprite.inlineAtlas ? sizeof(SpriteInlineAtlas) : 0);
    }

}

TEST(SpriteFrameBench, SheetSpritesShareTheirFrames) {
    SpriteComponent frameless;
    EXPECT_EQ(frameless.arrayHeapBytes(), 0u);
    SpriteComponent sheetSprite;
    sheetSprite.sheet = AssetRef("sprites/hero.aseprite");
    sheetSprite.sheetData = trimmedSheet();
    sheetSprite.loadedSheet = "sprites/hero.aseprite";
    sheetSprite.frame = 3;
    sheetSprite.frameName = "f3";
    sheetSprite.drawMode = SpriteDrawMode::SLICED;
    EXPECT_EQ(sheetSprite.arrayHeapBytes(), 0u);
    EXPECT_LE(spriteBytes(sheetSprite), spriteBytes(frameless) + 64u)
        << "a sheet sprite: " << spriteBytes(sheetSprite) << " bytes, a frameless one: " << spriteBytes(frameless);
    SpriteComponent legacy;
    legacy.framesRect.resize(3);
    legacy.numFramesRect = 3;
    EXPECT_EQ(legacy.arrayHeapBytes(), 3 * sizeof(SpriteFrameData));
    // the S45 runtime state of a legacy sprite is one pointer in the component, its mapping outside
    static_assert(sizeof(SpriteInlineAtlasRef) == sizeof(void*), "the inline atlas handle is one pointer");
}
