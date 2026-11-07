// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The component side (S53 R3, R42, R53, R91; AC39, AC64, AC72)

#include "TerrainTestSupport.h"

#include "terrain/TerrainHoles.h"

#include "Scene.h"
#include "component/TerrainComponent.h"
#include "object/TerrainEdit.h"
#include "subsystem/MeshSystem.h"
#include "support/AllocationCounter.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace terraintest;

namespace {

    constexpr int SIZE = 16;

    // A map in memory under an id of its own
    void giveMap(Texture& texture, const std::string& id, int channels, int bits, uint8_t fill) {
        const int bytesPerChannel = bits / 8;
        const size_t bytes = static_cast<size_t>(SIZE) * SIZE * channels * bytesPerChannel;
        unsigned char* raw = static_cast<unsigned char*>(std::malloc(bytes));
        std::memset(raw, fill, bytes);
        const ColorFormat format = channels == 4 ? ColorFormat::RGBA : (bits == 16 ? ColorFormat::RED16 : ColorFormat::RED);
        TextureData data(SIZE, SIZE, static_cast<unsigned int>(bytes), format, channels, raw);
        data.setDataOwned(false);
        texture.setData(id, data);
        texture.getData().setDataOwned(true);
    }

    void makeTerrain(TerrainComponent& terrain, const std::string& name) {
        terrain.terrainSize = static_cast<float>(SIZE);
        terrain.maxHeight = 8.0f;
        giveMap(terrain.heightMap, name + "-height", 1, 16, 0x40);
    }

    struct Received {
        Entity terrain;
        MapId map;
        TexelRect rect;
    };

}

TEST(TerrainEdit, StoreReadsAndWritesTheComponentsMaps) {
    TerrainComponent terrain;
    makeTerrain(terrain, "s53-store");
    TerrainComponentStore store(nullptr, 1, terrain);

    const MapFormat format = store.format(HEIGHT);
    ASSERT_TRUE(format.exists);
    EXPECT_EQ(format.bounds.w, SIZE);
    EXPECT_EQ(format.channels, 1);
    EXPECT_EQ(format.bitsPerChannel, 16);
    EXPECT_FALSE(store.format(HOLE).exists);
    EXPECT_FALSE(store.format(blend(0)).exists);
    EXPECT_EQ(store.frame().maxHeight, 8.0f);

    terrain.needUpdateTerrain = false;
    terrain.needUpdateTexture = false;
    terrain.needUpdateFoliage = false;
    terrain.heightMapLoaded = true;

    Op op = circleOp(OpType::Raise, 0.0f, 0.0f, 4.0f);
    op.delta = 2.0f;
    const TerrainOpResult result = apply(store, op);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.changed.size(), 1u);

    EXPECT_TRUE(terrain.needUpdateTerrain) << "the map changed, so the terrain refreshes as a stroke does today";
    EXPECT_TRUE(terrain.needUpdateTexture);
    EXPECT_TRUE(terrain.needUpdateFoliage);
    EXPECT_FALSE(terrain.heightMapLoaded);

    // The query sees it at once, through the same pixels
    const SurfaceSample sample = querySurface(terrain, 0.0f, 0.0f);
    EXPECT_GT(sample.height, 0x4040 / 65535.0f * 8.0f + 0.5f);

    std::vector<std::pair<MapId, TexelRect>> dirty;
    store.takeDirty(dirty);
    ASSERT_EQ(dirty.size(), 1u);
    EXPECT_EQ(dirty[0].second, result.changed[0].rect);

    // A store's lattice is the query's: texel centres at (i + 0.5) / N
    const Vector2 first = store.worldOfTexel(HEIGHT, 0, 0);
    EXPECT_FLOAT_EQ(first.x, -7.5f);
    const Vector2 texel = store.texelOfWorld(HEIGHT, -7.5f, 7.5f);
    EXPECT_FLOAT_EQ(texel.x, 0.0f);
    EXPECT_FLOAT_EQ(texel.y, 15.0f);
    EXPECT_EQ(store.beginLarge(HEIGHT, format.bounds), nullptr);
    EXPECT_EQ(store.takeUndoHook(), nullptr);
    EXPECT_EQ(store.preferredBlock(HEIGHT).w, SIZE);
}

TEST(TerrainEdit, CreatesAndRemovesMaps) {
    TerrainComponent terrain;
    makeTerrain(terrain, "s53-create");
    terrain.foliageLayers.resize(1);
    TerrainComponentStore store(nullptr, 1, terrain);

    Op paint = circleOp(OpType::PaintLayer, 0.0f, 0.0f, 3.0f);
    paint.layer = 1;
    const TerrainOpResult painted = apply(store, paint);
    ASSERT_TRUE(painted.ok) << painted.error;
    ASSERT_EQ(terrain.blendMaps.size(), 1u);
    EXPECT_TRUE(store.format(blend(0)).exists);
    EXPECT_EQ(store.format(blend(0)).channels, 4);

    Op density0 = circleOp(OpType::PaintDensity, 0.0f, 0.0f, 3.0f);
    density0.layer = 0;
    ASSERT_TRUE(apply(store, density0).ok);
    EXPECT_FALSE(terrain.foliageLayers[0].densityMap.empty());

    // A layer it does not have is refused before anything runs, with the reason
    Op density1 = density0;
    density1.layer = 1;
    const TerrainOpResult refused = apply(store, density1);
    EXPECT_FALSE(refused.ok);
    EXPECT_NE(refused.error.find("foliage layer"), std::string::npos) << refused.error;

    // Holes end to end, through the field S55 extends
    EXPECT_TRUE(terrain.holeMap.empty());
    const TerrainOpResult cut = apply(store, circleOp(OpType::CutHoles, 0.0f, 0.0f, 3.0f));
    ASSERT_TRUE(cut.ok) << cut.error;
    EXPECT_FALSE(terrain.holeMap.empty());
    EXPECT_TRUE(querySurface(terrain, 0.5f, 0.5f).hole);
    EXPECT_FALSE(querySurface(terrain, 7.0f, 7.0f).hole);
    Op fill;
    fill.type = OpType::FillHoles;
    const TerrainOpResult filled = apply(store, fill);
    ASSERT_TRUE(filled.ok);
    EXPECT_TRUE(filled.ops[0].mapRemoved);
    EXPECT_TRUE(terrain.holeMap.empty());
    EXPECT_FALSE(querySurface(terrain, 0.5f, 0.5f).hole);
}

TEST(TerrainEdit, EventsFireOncePerFrameForEveryRoute) {
    Scene scene;
    TerrainComponent terrain;
    makeTerrain(terrain, "s53-events");
    terrain.blendMaps.resize(1);
    giveMap(terrain.blendMaps[0], "s53-events-blend", 4, 8, 0);
    terrain.foliageLayers.resize(1);
    giveMap(terrain.foliageLayers[0].densityMap, "s53-events-density", 1, 8, 0);
    const Entity entity = 42;
    TerrainComponentStore store(&scene, entity, terrain);

    std::vector<Received> received;
    Subscription subscription = TerrainEditEvents::subscribe(&scene, [&](Entity from, MapId map, TexelRect rect) {
        received.push_back(Received{from, map, rect});
    });

    auto frame = [&]() {
        received.clear();
        TerrainEditEvents::flush(&scene);
    };

    // applyOps: two writes in a frame come as one, the union
    {
        Op a = circleOp(OpType::Raise, -4.0f, -4.0f, 1.0f);
        a.delta = 1.0f;
        Op b = circleOp(OpType::Raise, 4.0f, 4.0f, 1.0f);
        b.delta = 1.0f;
        ASSERT_TRUE(apply(store, a).ok);
        ASSERT_TRUE(apply(store, b).ok);
        EXPECT_TRUE(TerrainEditEvents::hasPending());
        frame();
        ASSERT_EQ(received.size(), 1u);
        EXPECT_EQ(received[0].terrain, entity);
        EXPECT_EQ(received[0].map, HEIGHT);
        EXPECT_LE(received[0].rect.x, 4);
        EXPECT_GE(received[0].rect.right(), 12);
        EXPECT_FALSE(TerrainEditEvents::hasPending());
        frame();
        EXPECT_TRUE(received.empty()) << "nothing new, nothing delivered";
    }

    // setHeights
    {
        const std::vector<float> heights(4, 3.0f);
        ASSERT_TRUE(setHeights(store, TexelRect{0, 0, 2, 2}, heights).ok);
        frame();
        ASSERT_EQ(received.size(), 1u);
        EXPECT_EQ(received[0].map, HEIGHT);
        EXPECT_EQ(received[0].rect, (TexelRect{0, 0, 2, 2}));
    }

    // applyField
    {
        const TerrainOpResult field = applyField(store, blend(0), TexelRect{1, 1, 3, 3},
            [](void*, float, float, std::span<const float>) {
                FieldSample sample;
                sample.weight = 1.0f;
                sample.target[0] = 1.0f;
                return sample;
            }, nullptr);
        ASSERT_TRUE(field.ok);
        frame();
        ASSERT_EQ(received.size(), 1u);
        EXPECT_EQ(received[0].map, blend(0));
        EXPECT_EQ(received[0].rect, (TexelRect{1, 1, 3, 3}));
    }

    // Batch: a commit over several maps
    {
        Batch batch(store);
        const std::vector<float> heights(4, 0.9f);
        ASSERT_TRUE(batch.setHeights(TexelRect{4, 4, 2, 2}, heights));
        ASSERT_TRUE(batch.setDensity(TexelRect{4, 4, 2, 2}, 0, heights));
        EXPECT_FALSE(TerrainEditEvents::hasPending()) << "nothing before the commit";
        ASSERT_TRUE(batch.commit().ok);
        frame();
        ASSERT_EQ(received.size(), 2u);
        EXPECT_EQ(received[0].map, HEIGHT);
        EXPECT_EQ(received[1].map, density(0));
    }

    // A hole: created and written, one event
    {
        ASSERT_TRUE(apply(store, circleOp(OpType::CutHoles, 0.0f, 0.0f, 2.0f)).ok);
        frame();
        ASSERT_EQ(received.size(), 1u);
        EXPECT_EQ(received[0].map, HOLE);
    }

    // A dry run writes nothing, so it says nothing
    {
        Op dry = circleOp(OpType::Lower, 0.0f, 0.0f, 3.0f);
        dry.delta = 1.0f;
        OpOptions options;
        options.dryRun = true;
        ASSERT_TRUE(apply(store, dry, options).ok);
        EXPECT_FALSE(TerrainEditEvents::hasPending());
    }

    // RAII: gone with the subscription
    subscription.reset();
    Op after = circleOp(OpType::Raise, 0.0f, 0.0f, 1.0f);
    after.delta = 1.0f;
    ASSERT_TRUE(apply(store, after).ok);
    EXPECT_FALSE(TerrainEditEvents::hasPending()) << "no subscriber, nothing kept";
}

TEST(TerrainEdit, EventsDeliverFromMeshSystemUpdate) {
    Scene scene;
    TerrainComponent terrain;
    makeTerrain(terrain, "s53-events-update");
    TerrainComponentStore store(&scene, 7, terrain);
    int deliveries = 0;
    Subscription subscription = TerrainEditEvents::subscribe(&scene, [&](Entity, MapId, TexelRect) { deliveries++; });

    Op op;
    op.type = OpType::Raise;
    op.delta = 1.0f;
    ASSERT_TRUE(apply(store, op).ok);
    EXPECT_EQ(deliveries, 0) << "at the frame, not at the call";
    scene.getSystem<MeshSystem>()->update(0.016);
    EXPECT_EQ(deliveries, 1);
    scene.getSystem<MeshSystem>()->update(0.016);
    EXPECT_EQ(deliveries, 1);
}

TEST(TerrainEdit, EventsKeepScenesApart) {
    Scene sceneA;
    Scene sceneB;
    TerrainComponent terrain;
    makeTerrain(terrain, "s53-events-scenes");
    TerrainComponentStore storeA(&sceneA, 5, terrain);
    TerrainComponentStore storeB(&sceneB, 5, terrain);   // the same entity number in another scene

    int inA = 0;
    int inB = 0;
    int anywhere = 0;
    Subscription a = TerrainEditEvents::subscribe(&sceneA, [&](Entity, MapId, TexelRect) { inA++; });
    Subscription b = TerrainEditEvents::subscribe(&sceneB, [&](Entity, MapId, TexelRect) { inB++; });
    Subscription any = TerrainEditEvents::subscribe([&](Entity, MapId, TexelRect) { anywhere++; });

    Op op;
    op.type = OpType::Raise;
    op.delta = 1.0f;
    ASSERT_TRUE(apply(storeA, op).ok);
    TerrainEditEvents::flush(&sceneB);
    EXPECT_EQ(inA + inB + anywhere, 0) << "B's frame does not deliver A's edit";
    TerrainEditEvents::flush(&sceneA);
    EXPECT_EQ(inA, 1);
    EXPECT_EQ(inB, 0);
    EXPECT_EQ(anywhere, 1);

    // A scene going away takes its undelivered edits with it
    Op other;
    other.type = OpType::Lower;
    other.delta = 1.0f;
    ASSERT_TRUE(apply(storeB, other).ok);
    EXPECT_TRUE(TerrainEditEvents::hasPending());
    TerrainEditEvents::forgetScene(&sceneB);
    EXPECT_FALSE(TerrainEditEvents::hasPending());
}

TEST(TerrainEdit, ZeroCostWhenUnused) {
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "no allocation counter on this platform";

    TerrainComponent terrain;
    makeTerrain(terrain, "s53-zero-cost");

    // No edit: the frame's guard and a query allocate nothing, and the store never comes into it.
    // S53 R53's whole-frame count over an unedited terrain comes with the node refit it guards.
    const uint64_t before = testsupport::threadAllocations();
    for (int frame = 0; frame < 1000; frame++) {
        if (TerrainEditEvents::hasPending()) TerrainEditEvents::flush(nullptr);
        const SurfaceSample sample = querySurface(terrain, 1.0f, 1.0f);
        ASSERT_TRUE(sample.present);
    }
    EXPECT_EQ(testsupport::threadAllocations(), before);
}

TEST(TerrainEdit, NewTerrainDefaultsHaveOneHome) {
    // Nothing yet differs from the struct's defaults: S55 R53 and S57 F12 add theirs in this function
    TerrainComponent plain;
    TerrainComponent made;
    applyNewTerrainDefaults(made);
    EXPECT_EQ(made.terrainSize, plain.terrainSize);
    EXPECT_EQ(made.maxHeight, plain.maxHeight);
    EXPECT_EQ(made.levels, plain.levels);
    EXPECT_TRUE(made.holeMap.empty());
    EXPECT_TRUE(made.limitLayersPerPixel) << "T3 R53";
    EXPECT_FALSE(plain.limitLayersPerPixel) << "an old terrain is untouched";

    // Every route that makes a terrain for a person or an agent comes through it
    const std::filesystem::path repository = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path();
    std::ifstream command(repository / "editor" / "command" / "type" / "CreateEntityCmd.cpp");
    ASSERT_TRUE(command.good());
    std::stringstream text;
    text << command.rdbuf();
    EXPECT_NE(text.str().find("applyNewTerrainDefaults("), std::string::npos);
}

TEST(TerrainEdit, ApplyLayerOrderMovesTheLayers) {
    TerrainComponent terrain;
    terrain.surfaceLayers.resize(3);
    terrain.surfaceLayers[0].normalStrength = 0.1f;
    terrain.surfaceLayers[1].normalStrength = 0.2f;
    terrain.surfaceLayers[2].normalStrength = 0.3f;
    const int order[] = {2, 0, 1};
    terrain.needUpdateTexture = false;
    applyLayerOrder(terrain, order);
    EXPECT_FLOAT_EQ(terrain.surfaceLayers[2].normalStrength, 0.1f);
    EXPECT_FLOAT_EQ(terrain.surfaceLayers[0].normalStrength, 0.2f);
    EXPECT_FLOAT_EQ(terrain.surfaceLayers[1].normalStrength, 0.3f);
    EXPECT_TRUE(terrain.needUpdateTexture);
}
