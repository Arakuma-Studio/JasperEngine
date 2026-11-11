// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The CPU budgets of grid tilemaps (docs/specs/S50-grid-tilemap.md R57, R59, R60): one 32 x 32 chunk
// rebuild, the decode of a 1,000,000-cell layer and an animated chunk's patch. Printed as [bench]
// lines and enforced through testsupport::perfSlack() (desktop budgets; mobile and web are not
// measured on this host, spec Deviations "AC5, AC15 (measurement)").

#include "render/sprite/SpriteInstanceBatch.h"
#include "tilemap/TileCells.h"
#include "tilemap/TilemapChunkBuilder.h"
#include "../support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    using BenchClock = std::chrono::steady_clock;

    double benchMicros(BenchClock::time_point start, int repeats) {
        return std::chrono::duration<double, std::micro>(BenchClock::now() - start).count() / repeats;
    }

    // tiles 0-3 still, tile 4 animated over 0 and 1 (100 ms each), all 16 x 16 on one page
    std::shared_ptr<TilesetRuntime> benchTileset() {
        auto runtime = std::make_shared<TilesetRuntime>();
        runtime->data = std::make_shared<TilesetData>();
        runtime->pages = {"bench.png"};
        for (int i = 0; i < 5; i++) {
            TileRenderInfo info;
            info.valid = true;
            info.u0 = (float)i / 5.0f;
            info.u1 = (float)(i + 1) / 5.0f;
            info.v1 = 1.0f;
            info.x1 = 16.0f;
            info.y1 = 16.0f;
            info.width = 16;
            info.height = 16;
            runtime->tiles.push_back(info);
        }
        TileAnimationRuntime animation;
        animation.frames = {0, 1};
        animation.endsMs = {100, 200};
        animation.totalMs = 200;
        runtime->animations.push_back(animation);
        runtime->tiles[4].animation = 0;
        return runtime;
    }

    TilemapChunkParams benchParams(const TileCells& cells, const TilesetRuntime& tileset) {
        TilemapChunkParams params;
        params.cells = &cells;
        params.tileset = &tileset;
        params.pixelsPerUnit = 1.0;
        return params;
    }

}

// R57: one full 32 x 32 chunk rebuild <= 50 us desktop
TEST(TilemapBench, OneChunkRebuild) {
    const std::shared_ptr<TilesetRuntime> tileset = benchTileset();
    TileCells cells(32);
    std::mt19937 random(7);
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++)
            cells.setTile(CellCoord{x, y}, (int32_t)(random() % 4), (random() % 3 == 0) ? TILE_FLIP_X : 0u);
    }
    const TilemapChunkParams params = benchParams(cells, *tileset);
    SpriteInstanceBatch batch;
    TilemapChunkBuild build;
    tilechunk::build(params, ChunkCoord{0, 0}, batch, build);
    ASSERT_EQ(batch.getCount(), 1024u);
    // the best of 5 rounds of 40 builds: a shared machine's other work lands in some of them
    constexpr int repeats = 40;
    double us = 1e30;
    for (int round = 0; round < 5; round++) {
        const BenchClock::time_point start = BenchClock::now();
        for (int i = 0; i < repeats; i++)
            tilechunk::build(params, ChunkCoord{0, 0}, batch, build);
        us = std::min(us, benchMicros(start, repeats));
    }
    std::printf("[bench] tilemap: one 32x32 chunk rebuild %.2f us (budget 50 us desktop)\n", us);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0) {
        EXPECT_LT(us, 50.0 * slack);
    }
}

// R59: a 1,000,000-cell layer (about 980 chunks of 32 x 32, short runs) decodes in <= 40 ms desktop
TEST(TilemapBench, DecodeAMillionCellLayer) {
    TileCells source(32);
    std::mt19937 random(11);
    for (int y = 0; y < 1000; y++) {
        for (int x = 0; x < 1000; x++)
            source.setTile(CellCoord{x, y}, (int32_t)((x / 3 + y / 2 + (int)(random() % 2)) % 40));
    }
    ASSERT_EQ(source.cellCount(), 1000000u);
    std::vector<std::pair<ChunkCoord, std::string>> chunks;
    for (const ChunkCoord& coord : source.sortedChunks())
        chunks.emplace_back(coord, source.encodeChunk(coord));

    double bestMs = 1e30;
    for (int round = 0; round < 3; round++) {
        TileCells decoded;
        std::string error;
        const BenchClock::time_point start = BenchClock::now();
        for (const auto& [coord, text] : chunks) {
            ASSERT_TRUE(decoded.decodeChunk(coord, text, error)) << error;
        }
        bestMs = std::min(bestMs, benchMicros(start, 1) / 1000.0);
        ASSERT_EQ(decoded.cellCount(), 1000000u);
        if (round == 0) {
            EXPECT_TRUE(decoded == source);
        }
    }
    std::printf("[bench] tilemap: decode of a 1,000,000-cell layer (%zu chunks) %.2f ms (budget 40 ms desktop)\n", chunks.size(), bestMs);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0) {
        EXPECT_LT(bestMs, 40.0 * slack);
    }
}

// R60: a chunk whose 1,024 tiles all animate patches its texture rects in <= 30 us desktop; a
// frame without a frame change is one comparison
TEST(TilemapBench, AnimatedChunkPatch) {
    const std::shared_ptr<TilesetRuntime> tileset = benchTileset();
    TileCells cells(32);
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++)
            cells.setTile(CellCoord{x, y}, 4);
    }
    TilemapChunkParams params = benchParams(cells, *tileset);
    SpriteInstanceBatch batch;
    TilemapChunkBuild build;
    tilechunk::build(params, ChunkCoord{0, 0}, batch, build);
    ASSERT_EQ(build.animated.size(), 1024u);
    ASSERT_EQ(build.nextFrameMs, 100);

    constexpr int repeats = 200;
    double total = 0.0;
    int64_t ms = 0;
    for (int i = 0; i < repeats; i++) {
        ms = build.nextFrameMs;
        bool changed = false;
        const BenchClock::time_point start = BenchClock::now();
        ASSERT_TRUE(tilechunk::updateAnimation(params, batch, build, ms, changed));
        total += benchMicros(start, 1);
        ASSERT_TRUE(changed);
    }
    const double us = total / repeats;
    bool changed = true;
    ASSERT_TRUE(tilechunk::updateAnimation(params, batch, build, ms, changed));
    EXPECT_FALSE(changed) << "no frame change: nothing patched";
    std::printf("[bench] tilemap: animated 32x32 chunk patch %.2f us (budget 30 us desktop)\n", us);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0) {
        EXPECT_LT(us, 30.0 * slack);
    }
}
