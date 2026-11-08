// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1's store over a Terrain World's tiles (T4 R28-R30, R51, D11, D12; AC26, AC27's store half, AC33's)

#include "TerrainWorldTestSupport.h"
#include "terrain/OverlayMapStore.h"
#include "terrain/RawMapStore.h"
#include "terrain/TerrainOps.h"

#include <gtest/gtest.h>

using namespace terrainworldtest;
using jasperengine::Vector2;

namespace {

    constexpr MapId HEIGHT{MapKind::Height, 0};

    struct World {
        WorldLattice lattice;
        WorldMaps maps;
        MemoryTileAccess access;
        TiledMapStore store;

        explicit World(bool autoAdd = true, int tileQuads = 64): lattice(smallLattice(tileQuads, 1.0f)), store(lattice, maps, access, TiledStoreOptions{autoAdd}) {
            maps.maxHeight = 16.0f;
            store.maps().maxHeight = 16.0f;
        }

        int m() const { return lattice.heightOrigin(); }
        int n() const { return lattice.tileQuads; }
    };

    std::vector<uint8_t> fileOf(const TileData& tile) {
        std::vector<uint8_t> bytes;
        encodeTile(tile, TileCodec{}, TileEncodeOptions{}, bytes);
        return bytes;
    }

    Op circle(OpType type, float x, float z, float radius) {
        Op op;
        op.type = type;
        op.shape.type = ShapeType::Circle;
        op.shape.center = Vector2(x, z);
        op.shape.radius = radius;
        return op;
    }

}

TEST(TiledMapStore, WritesEveryCopy) {
    World world;
    for (int z = -1; z <= 0; z++) {
        for (int x = -1; x <= 0; x++) ASSERT_TRUE(world.store.addTile(TileCoord(x, z)));
    }
    // a 4-tile corner: the sample at local (0, 0) is in all four tiles, its neighbours in gutters
    const TexelRect corner{world.m() - 3, world.m() - 3, 7, 7};
    std::vector<float> values(static_cast<size_t>(corner.area()));
    for (size_t i = 0; i < values.size(); i++) values[i] = 0.1f + 0.01f * static_cast<float>(i);
    ASSERT_TRUE(world.store.write(HEIGHT, corner, values).ok);

    const int n = world.n();
    for (int sz = -3; sz <= 3; sz++) {
        for (int sx = -3; sx <= 3; sx++) {
            const uint16_t expected = static_cast<uint16_t>(quantizeChannel(values[(sz + 3) * 7 + (sx + 3)], 16));
            int copies = 0;
            for (const TileCoord tile : world.access.coords()) {
                const auto data = world.access.tile(tile);
                const int ti = sx - tile.x * n + 1, tj = sz - tile.z * n + 1;
                if (ti < 0 || tj < 0 || ti > n + 2 || tj > n + 2) continue;
                EXPECT_EQ(data->sample(ti, tj), expected) << "tile (" << tile.x << ", " << tile.z << ") sample " << sx << ", " << sz;
                copies++;
            }
            EXPECT_GE(copies, 1);
            if (sx == 0 && sz == 0) {
                EXPECT_EQ(copies, 4) << "the corner sample is in every tile";
            }
        }
    }
}

TEST(TiledMapStore, ReadStitches) {
    World world;
    for (int z = -1; z <= 0; z++) {
        for (int x = -1; x <= 0; x++) world.access.add(TileCoord(x, z), fixtureTile(world.lattice, TileCoord(x, z)));
    }
    const TexelRect rect{world.m() - 40, world.m() - 10, 80, 30};
    const std::vector<uint16_t> samples = readSamples(world.store, rect);
    ASSERT_EQ(samples.size(), static_cast<size_t>(rect.area()));
    for (int y = 0; y < rect.h; y++) {
        for (int x = 0; x < rect.w; x++) {
            ASSERT_EQ(samples[y * rect.w + x], fixtureSample(rect.x + x - world.m(), rect.y + y - world.m()));
        }
    }
    // over an absent tile: the base height
    world.store.maps().baseHeight = 4321;
    const std::vector<uint16_t> beyond = readSamples(world.store, TexelRect{world.m() + 70, world.m() + 5, 3, 3});
    for (uint16_t value : beyond) EXPECT_EQ(value, 4321);
}

TEST(TiledMapStore, WorldOfTexelOnLattice) {
    World world(false, 256);
    world.lattice.spacing = 0.5f;
    TiledMapStore store(world.lattice, world.maps, world.access);
    const int m = world.lattice.heightOrigin();
    for (int sample : {-65536, -257, -1, 0, 1, 255, 256, 65535}) {
        const Vector2 at = store.worldOfTexel(HEIGHT, sample + m, -sample + m);
        EXPECT_EQ(at.x, static_cast<float>(sample) * 0.5f);
        EXPECT_EQ(at.y, static_cast<float>(-sample) * 0.5f);
        const Vector2 back = store.texelOfWorld(HEIGHT, at.x, at.y);
        EXPECT_EQ(back.x, static_cast<float>(sample + m));
        EXPECT_EQ(back.y, static_cast<float>(-sample + m));
    }
    // blend samples are shared corners too, at the blend's own spacing
    const MapId blend{MapKind::Blend, 0};
    const int mb = world.lattice.blendOrigin();
    EXPECT_EQ(store.worldOfTexel(blend, mb + 1, mb).x, world.lattice.tileSize() / static_cast<float>(world.lattice.blendQuadCount()));
    EXPECT_EQ(store.worldOfTexel(blend, mb + world.lattice.blendQuadCount(), mb).x, world.lattice.tileSize());
    // area maps: texel centres at half steps, tile 0's first texel right of its corner
    const MapId hole{MapKind::Hole, 0};
    const int mh = world.lattice.holeOrigin();
    const float holeTexel = world.lattice.tileSize() / static_cast<float>(world.lattice.holeSide());
    EXPECT_EQ(store.worldOfTexel(hole, mh, mh).x, holeTexel * 0.5f);
    EXPECT_EQ(store.worldOfTexel(hole, mh - 1, mh).x, -holeTexel * 0.5f);
    EXPECT_EQ(store.texelOfWorld(hole, holeTexel * 0.5f, 0.0f).x, static_cast<float>(mh));
    // the format is the addressable lattice, with no negative texel
    EXPECT_EQ(store.format(HEIGHT).bounds, (TexelRect{0, 0, 2 * m + 1, 2 * m + 1}));
    EXPECT_EQ(store.preferredBlock(HEIGHT), (TexelRect{m, m, 256, 256}));
}

TEST(TiledMapStore, DirtyRectsAndTilesReported) {
    World world;
    ASSERT_TRUE(world.store.addTile(TileCoord(0, 0)));
    std::vector<std::pair<MapId, TexelRect>> dirty;
    world.store.takeDirty(dirty);
    world.access.takeChanged();
    dirty.clear();

    // a write across the high-x border: one tile made, both reported
    const TexelRect rect{world.m() + 60, world.m() + 10, 8, 4};
    std::vector<float> values(static_cast<size_t>(rect.area()), 0.5f);
    const WriteResult wrote = world.store.write(HEIGHT, rect, values);
    ASSERT_TRUE(wrote.ok);
    EXPECT_EQ(wrote.tilesCreated, 1);
    EXPECT_TRUE(world.access.present(TileCoord(1, 0)));
    world.store.takeDirty(dirty);
    ASSERT_EQ(dirty.size(), 1u);
    EXPECT_EQ(dirty[0].second, rect);
    const std::vector<TileCoord> changed = world.access.takeChanged();
    EXPECT_EQ(changed.size(), 2u);
    // the same write again changes nothing and reports nothing
    ASSERT_TRUE(world.store.write(HEIGHT, rect, values).ok);
    dirty.clear();
    world.store.takeDirty(dirty);
    EXPECT_TRUE(dirty.empty());
    EXPECT_TRUE(world.access.takeChanged().empty());
}

TEST(TiledMapStore, RefusedWholeByValidateOps) {
    World world;
    ASSERT_TRUE(world.store.addTile(TileCoord(0, 0)));
    world.store.options().maxBoxTiles = 3;
    // a stroke that would make the world four tiles wide is refused before anything runs
    Op far = circle(OpType::Raise, 64.0f * 3.5f, 32.0f, 10.0f);
    far.delta = 1.0f;
    const TerrainOpResult refused = applyOps(world.store, std::span<const Op>(&far, 1));
    EXPECT_FALSE(refused.ok);
    EXPECT_NE(refused.error.find("3 tiles"), std::string::npos) << refused.error;
    EXPECT_EQ(world.access.count(), 1u) << "all or nothing";
    // inside the box it goes through and makes its tile
    Op near = circle(OpType::Raise, 64.0f * 1.5f, 32.0f, 10.0f);
    near.delta = 1.0f;
    const TerrainOpResult made = applyOps(world.store, std::span<const Op>(&near, 1));
    ASSERT_TRUE(made.ok) << made.error;
    EXPECT_EQ(made.tilesCreated, 1);
    EXPECT_TRUE(made.hasWarning("tile_created"));
    // past the world's limit of 16,384 m
    world.store.options().maxBoxTiles = 256;
    Op edge = circle(OpType::Raise, 16384.0f + 10.0f, 0.0f, 4.0f);
    edge.delta = 1.0f;
    EXPECT_FALSE(applyOps(world.store, std::span<const Op>(&edge, 1)).ok);
}

TEST(TiledMapStore, OpsMatchOneMap) {
    // the ops engine over a 2 x 2 world and over one map on the same samples: same bytes (AC26's
    // stand-in for T1's golden hashes, spec Deviations)
    World world;
    for (int z = -1; z <= 0; z++) {
        for (int x = -1; x <= 0; x++) world.access.add(TileCoord(x, z), fixtureTile(world.lattice, TileCoord(x, z)));
    }
    constexpr int SIDE = 129;
    RawMapStore plain(static_cast<float>(SIDE), 16.0f);
    plain.addMap(HEIGHT, SIDE, SIDE, 1, 16);
    std::vector<uint8_t> seed(static_cast<size_t>(SIDE) * SIDE * 2);
    for (int y = 0; y < SIDE; y++) {
        for (int x = 0; x < SIDE; x++) {
            const uint16_t value = fixtureSample(x - 64, y - 64);
            seed[(static_cast<size_t>(y) * SIDE + x) * 2] = static_cast<uint8_t>(value & 0xFF);
            seed[(static_cast<size_t>(y) * SIDE + x) * 2 + 1] = static_cast<uint8_t>(value >> 8);
        }
    }
    ASSERT_TRUE(plain.writeBytes(HEIGHT, TexelRect{0, 0, SIDE, SIDE}, seed).ok);

    std::vector<Op> ops;
    Op raise = circle(OpType::Raise, 3.0f, -2.0f, 20.0f);
    raise.delta = 2.0f;
    ops.push_back(raise);
    Op smooth = circle(OpType::Smooth, -6.0f, 5.0f, 18.0f);
    smooth.iterations = 3;
    ops.push_back(smooth);
    // (add_noise anchors its field at the map's first texel, which a world puts at its addressable
    // corner: the same op, another field, so it is left out of this comparison)
    Op terrace = circle(OpType::Terrace, 10.0f, 10.0f, 15.0f);
    terrace.steps = 5;
    ops.push_back(terrace);
    Op set = circle(OpType::SetHeight, -20.0f, -20.0f, 9.0f);
    set.height = 8.0f;
    ops.push_back(set);
    const TerrainOpResult plainResult = applyOps(plain, ops);
    const TerrainOpResult worldResult = applyOps(world.store, ops);
    ASSERT_TRUE(plainResult.ok) << plainResult.error;
    ASSERT_TRUE(worldResult.ok) << worldResult.error;

    std::vector<uint8_t> plainBytes(seed.size());
    ASSERT_TRUE(plain.readBytes(HEIGHT, TexelRect{0, 0, SIDE, SIDE}, plainBytes));
    std::vector<uint8_t> worldBytes(seed.size());
    ASSERT_TRUE(world.store.readBytes(HEIGHT, TexelRect{world.m() - 64, world.m() - 64, SIDE, SIDE}, worldBytes));
    EXPECT_EQ(worldBytes, plainBytes);
    for (size_t i = 0; i < ops.size(); i++) EXPECT_EQ(worldResult.ops[i].changedTexels, plainResult.ops[i].changedTexels) << i;
}

TEST(TiledMapStore, UndoRemovesCreatedTilesBytesIdentical) {
    World world;
    ASSERT_TRUE(world.store.addTile(TileCoord(0, 0)));
    world.access.add(TileCoord(0, 0), fixtureTile(world.lattice, TileCoord(0, 0)));
    world.store.syncGutters(TileCoord(0, 0));
    const std::vector<uint8_t> before = fileOf(*world.access.tile(TileCoord(0, 0)));

    // a stroke over the border, as T1's commands run it: an overlay, its delta, the store's hook
    OverlayMapStore overlay(world.store);
    Op raise = circle(OpType::Raise, 64.0f, 30.0f, 12.0f);
    raise.delta = 3.0f;
    OpOptions inPlace;
    inPlace.inPlace = true;
    ASSERT_TRUE(applyOps(overlay, std::span<const Op>(&raise, 1), inPlace).ok);
    struct Delta {
        MapId map;
        TexelRect rect;
        std::vector<uint8_t> before, after;
    };
    std::vector<Delta> deltas;
    overlay.visitTiles([&](MapId map, TexelRect rect, std::span<const uint8_t> b, std::span<const uint8_t> a) {
        deltas.push_back(Delta{map, rect, std::vector<uint8_t>(b.begin(), b.end()), std::vector<uint8_t>(a.begin(), a.end())});
    });
    ASSERT_TRUE(overlay.commit().ok);
    std::unique_ptr<TerrainStoreUndoHook> hook = world.store.takeUndoHook();
    ASSERT_TRUE(hook);
    ASSERT_TRUE(world.access.present(TileCoord(1, 0)));
    const std::vector<uint8_t> afterOwn = fileOf(*world.access.tile(TileCoord(0, 0)));
    const std::vector<uint8_t> afterMade = fileOf(*world.access.tile(TileCoord(1, 0)));
    EXPECT_NE(afterOwn, before);

    // undo as TerrainMapDeltaCmd does with a world's store: hooks first, then the bytes, never making a tile
    world.store.options().autoAddTiles = false;
    hook->undo();
    for (const Delta& delta : deltas) {
        std::vector<uint8_t> current(delta.before.size());
        ASSERT_TRUE(world.store.readBytes(delta.map, delta.rect, current));
        for (size_t i = 0; i < current.size(); i++) current[i] ^= delta.before[i] ^ delta.after[i];
        ASSERT_TRUE(world.store.writeBytes(delta.map, delta.rect, current).ok);
    }
    EXPECT_FALSE(world.access.present(TileCoord(1, 0)));
    EXPECT_EQ(fileOf(*world.access.tile(TileCoord(0, 0))), before) << "one undo restores the file bytes";

    // redo: the bytes, then the hook puts the tile back as it was
    for (const Delta& delta : deltas) {
        std::vector<uint8_t> current(delta.before.size());
        ASSERT_TRUE(world.store.readBytes(delta.map, delta.rect, current));
        for (size_t i = 0; i < current.size(); i++) current[i] ^= delta.before[i] ^ delta.after[i];
        ASSERT_TRUE(world.store.writeBytes(delta.map, delta.rect, current).ok);
    }
    hook->redo();
    EXPECT_EQ(fileOf(*world.access.tile(TileCoord(0, 0))), afterOwn);
    EXPECT_EQ(fileOf(*world.access.tile(TileCoord(1, 0))), afterMade);
}

TEST(TiledMapStore, LargeEditSnapshots) {
    World world;
    for (int z = 0; z < 2; z++) {
        for (int x = 0; x < 2; x++) world.access.add(TileCoord(x, z), fixtureTile(world.lattice, TileCoord(x, z)));
    }
    // the outer gutters repeat their borders, as a store keeps them
    for (const TileCoord tile : world.access.coords()) world.store.syncGutters(tile);
    world.store.options().largeEditTexels = 32 * 32;
    const std::vector<uint8_t> before = fileOf(*world.access.tile(TileCoord(1, 1)));
    Op raise = circle(OpType::Raise, 64.0f, 64.0f, 50.0f);
    raise.delta = 1.0f;
    const TerrainOpResult result = applyOps(world.store, std::span<const Op>(&raise, 1));
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.largeEdits.size(), 1u) << "past largeEditTexels the op runs straight on the tiles";
    EXPECT_NE(fileOf(*world.access.tile(TileCoord(1, 1))), before);
    const std::vector<uint8_t> after = fileOf(*world.access.tile(TileCoord(1, 1)));
    result.largeEdits[0]->undo();
    EXPECT_EQ(fileOf(*world.access.tile(TileCoord(1, 1))), before);
    result.largeEdits[0]->redo();
    EXPECT_EQ(fileOf(*world.access.tile(TileCoord(1, 1))), after);
}

TEST(TiledMapStore, DryRunPredictsNoUndo) {
    World world;
    for (int x = 0; x < 3; x++) world.access.add(TileCoord(x, 0), fixtureTile(world.lattice, TileCoord(x, 0)));
    world.store.options().largeEditTexels = 16 * 16;
    const TexelRect rect = world.store.tileRect(HEIGHT, TileCoord(1, 0));
    LargeEditPrediction prediction = world.store.predictLarge(HEIGHT, rect);
    EXPECT_TRUE(prediction.large);
    EXPECT_TRUE(prediction.undoable);
    EXPECT_EQ(prediction.tiles, 3) << "the tile and the neighbours holding its borders";
    EXPECT_EQ(prediction.snapshotBytes, 3 * world.access.tile(TileCoord(0, 0))->cpuBytes());
    world.store.options().snapshotCapBytes = prediction.snapshotBytes - 1;
    prediction = world.store.predictLarge(HEIGHT, rect);
    EXPECT_FALSE(prediction.undoable) << "past the cap the call needs allow_no_undo";
    EXPECT_FALSE(world.store.predictLarge(HEIGHT, TexelRect{rect.x, rect.y, 8, 8}).large);
}

TEST(TiledMapStore, SeamCheckAndRepair) {
    World world;
    world.access.add(TileCoord(0, 0), fixtureTile(world.lattice, TileCoord(0, 0)));
    world.access.add(TileCoord(1, 0), fixtureTile(world.lattice, TileCoord(1, 0)));
    EXPECT_EQ(world.store.seamErrors(TileCoord(0, 0), false), 0);
    // corrupt one border sample of the higher tile
    auto bad = world.access.writable(TileCoord(1, 0));
    const uint16_t good = bad->sample(1, 20);
    bad->setSample(1, 20, static_cast<uint16_t>(good + 77));
    EXPECT_EQ(world.store.seamErrors(TileCoord(1, 0), false), 1);
    EXPECT_EQ(world.store.seamErrors(TileCoord(0, 0), false), 1);
    // the lower (x, z) wins
    EXPECT_EQ(world.store.seamErrors(TileCoord(1, 0), true), 1);
    EXPECT_EQ(world.store.seamErrors(TileCoord(1, 0), false), 0);
    EXPECT_EQ(world.access.tile(TileCoord(1, 0))->sample(1, 20), good);
    EXPECT_EQ(world.access.tile(TileCoord(0, 0))->sample(65, 20), good);
}

TEST(TiledMapStore, PaintAcrossBorderSharesBlendSamples) {
    World world;
    world.access.add(TileCoord(0, 0), fixtureTile(world.lattice, TileCoord(0, 0)));
    world.access.add(TileCoord(1, 0), fixtureTile(world.lattice, TileCoord(1, 0)));
    const MapId blend{MapKind::Blend, 0};
    ASSERT_TRUE(world.store.createMap(blend, MapFormat{}, 0.0f).ok);
    const int mb = world.lattice.blendOrigin();
    const int nb = world.lattice.blendQuadCount();
    const TexelRect rect{mb + nb - 2, mb + 3, 5, 2};
    std::vector<uint8_t> bytes(static_cast<size_t>(rect.area()) * 4, 0);
    for (size_t i = 0; i < bytes.size(); i += 4) bytes[i] = 200;
    ASSERT_TRUE(world.store.writeBytes(blend, rect, bytes).ok);
    const auto left = world.access.tile(TileCoord(0, 0)), right = world.access.tile(TileCoord(1, 0));
    ASSERT_EQ(left->blend.size(), 1u);
    ASSERT_EQ(right->blend.size(), 1u);
    for (int z = 3; z < 5; z++) {
        EXPECT_EQ(left->blend[0].texel(nb, z)[0], 200);
        EXPECT_EQ(right->blend[0].texel(0, z)[0], 200) << "the shared column is in both tiles";
    }
    // a tile with no blend map reads the world's fill
    std::vector<uint8_t> back(4);
    ASSERT_TRUE(world.store.readBytes(blend, TexelRect{mb + 4 * nb, mb, 1, 1}, back));
    EXPECT_EQ(back[0], 0);
    EXPECT_EQ(back[3], 255);
}
