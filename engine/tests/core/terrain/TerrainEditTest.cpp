// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The component side (T1 R3, R42, R53, R91; AC39, AC64, AC72)

#include "TerrainTestSupport.h"

#include "terrain/TerrainHoles.h"

#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "component/SplineComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/TerrainEdit.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/SplineSystem.h"
#include "support/AllocationCounter.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
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

namespace {

    // A node tree is only built with a camera to range it for
    void addCamera(Scene& scene) {
        const Entity camera = scene.createEntity();
        scene.addComponent<Transform>(camera, {});
        scene.addComponent<CameraComponent>(camera, {});
        scene.setCamera(camera);
    }

}

TEST(TerrainEdit, StoreReadsAndWritesTheComponentsMaps) {
    TerrainComponent terrain;
    makeTerrain(terrain, "t1-store");
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

    // T1 R23: a height edit keeps the node tree; the image follows, and the surface revision and
    // the foliage wait for the frame
    EXPECT_FALSE(terrain.needUpdateTerrain);
    EXPECT_TRUE(terrain.needUpdateTexture);
    EXPECT_FALSE(terrain.needUpdateFoliage);
    EXPECT_TRUE(terrain.heightMapLoaded);
    ASSERT_NE(terrain.editState, nullptr);
    EXPECT_TRUE(terrain.editState->surfaceMoved);

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
    makeTerrain(terrain, "t1-create");
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

    // Holes end to end, through the field T3 extends
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
    makeTerrain(terrain, "t1-events");
    terrain.blendMaps.resize(1);
    giveMap(terrain.blendMaps[0], "t1-events-blend", 4, 8, 0);
    terrain.foliageLayers.resize(1);
    giveMap(terrain.foliageLayers[0].densityMap, "t1-events-density", 1, 8, 0);
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
    makeTerrain(terrain, "t1-events-update");
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
    makeTerrain(terrain, "t1-events-scenes");
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

    // T1 R53: one null pointer and one revision integer on the component
    EXPECT_EQ(sizeof(TerrainComponent::editState), sizeof(std::shared_ptr<TerrainEditState>));
    EXPECT_EQ(sizeof(TerrainComponent::surfaceRevision), sizeof(uint32_t));

    Scene scene;
    const Entity entity = scene.createEntity();
    scene.addComponent<Transform>(entity, {});
    scene.addComponent<TerrainComponent>(entity, {});
    scene.addComponent<MeshComponent>(entity, {});
    TerrainComponent& terrain = scene.getComponent<TerrainComponent>(entity);
    makeTerrain(terrain, "t1-zero-cost");
    addCamera(scene);
    MeshSystem& system = *scene.getSystem<MeshSystem>();
    system.update(0.016);
    ASSERT_TRUE(terrain.heightMapLoaded);
    system.update(0.016);

    // No edit: a frame over a built terrain allocates what one without it does, plus the terrain loop's
    // own list of terrains to scatter (not this feature's), and a query allocates nothing
    Scene bare;
    addCamera(bare);
    MeshSystem& bareSystem = *bare.getSystem<MeshSystem>();
    bareSystem.update(0.016);
    uint64_t start = testsupport::threadAllocations();
    for (int frame = 0; frame < 1000; frame++) {
        bareSystem.update(0.016);
    }
    const uint64_t withoutTerrain = testsupport::threadAllocations() - start;
    start = testsupport::threadAllocations();
    for (int frame = 0; frame < 1000; frame++) {
        system.update(0.016);
    }
    EXPECT_LE(testsupport::threadAllocations() - start, withoutTerrain + 1000);

    start = testsupport::threadAllocations();
    for (int frame = 0; frame < 1000; frame++) {
        const SurfaceSample sample = querySurface(terrain, 1.0f, 1.0f);
        ASSERT_TRUE(sample.present);
    }
    EXPECT_EQ(testsupport::threadAllocations(), start);
    EXPECT_EQ(terrain.editState, nullptr);
    EXPECT_EQ(terrain.surfaceRevision, 0u);
}

namespace {

    // A built node tree over a map with one texel raised, so ranges have something to notice
    struct BuiltTerrain {
        Scene scene;
        Entity entity = NULL_ENTITY;

        explicit BuiltTerrain(const std::string& name) {
            entity = scene.createEntity();
            scene.addComponent<Transform>(entity, {});
            TerrainComponent component;
            component.rootGridSize = 2;
            component.levels = 3;
            scene.addComponent<TerrainComponent>(entity, component);
            scene.addComponent<MeshComponent>(entity, {});
            makeTerrain(terrain(), name);
            addCamera(scene);
            system().update(0.016);
        }

        TerrainComponent& terrain() { return scene.getComponent<TerrainComponent>(entity); }
        MeshSystem& system() { return *scene.getSystem<MeshSystem>(); }
    };

    std::vector<std::pair<float, float>> nodeRanges(const TerrainComponent& terrain) {
        std::vector<std::pair<float, float>> ranges;
        for (unsigned int i = 0; i < terrain.numNodes; i++) {
            ranges.emplace_back(terrain.nodes[i].minHeight, terrain.nodes[i].maxHeight);
        }
        return ranges;
    }

}

// AC18 (T1 R23): the tree stays, its ranges are what a full rebuild would give
TEST(TerrainEdit, HeightEditDoesNotRebuild) {
    BuiltTerrain built("t1-refit");
    TerrainComponent& terrain = built.terrain();
    ASSERT_TRUE(terrain.heightMapLoaded);
    ASSERT_GT(terrain.numNodes, 0u);
    const TerrainNode* nodes = terrain.nodes.data();
    const unsigned int numNodes = terrain.numNodes;
    const std::vector<std::pair<float, float>> before = nodeRanges(terrain);

    TerrainComponentStore store(&built.scene, built.entity, terrain);
    Op op;
    op.type = OpType::Raise;
    op.delta = 3.0f;
    op.shape.type = ShapeType::Circle;
    op.shape.center = Vector2(-5.0f, -5.0f);
    op.shape.radius = 2.0f;
    ASSERT_TRUE(apply(store, op).ok);

    EXPECT_TRUE(terrain.heightMapLoaded) << "drawing is not gated off during a stroke";
    EXPECT_FALSE(terrain.needUpdateTerrain);
    built.system().update(0.016);
    EXPECT_EQ(terrain.nodes.data(), nodes) << "the node memory was not made again";
    EXPECT_EQ(terrain.numNodes, numNodes);
    EXPECT_TRUE(terrain.heightMapLoaded);
    EXPECT_EQ(terrain.surfaceRevision, 1u);

    const std::vector<std::pair<float, float>> refitted = nodeRanges(terrain);
    EXPECT_NE(refitted, before) << "the edit reached the ranges";

    terrain.needUpdateTerrain = true;
    built.system().update(0.016);
    EXPECT_EQ(nodeRanges(terrain), refitted) << "as a full rebuild would give them";
}

// AC18 (T1 R23): at most four revisions a second during a stroke, and always one at its end
TEST(TerrainEdit, StrokeHoldsTheRevisionRate) {
    BuiltTerrain built("t1-revision-rate");
    TerrainComponent& terrain = built.terrain();
    terrain.needUpdateFoliage = false;
    TerrainComponentStore store(&built.scene, built.entity, terrain);
    Op op;
    op.type = OpType::Raise;
    op.delta = 0.5f;

    beginTerrainStroke(terrain);
    double now = 100.0;
    int revisionsMade = 0;
    for (int frame = 0; frame < 60; frame++) {
        const uint32_t was = terrain.surfaceRevision;
        op.type = frame % 2 == 0 ? OpType::Raise : OpType::Lower;   // it keeps moving, never clipped
        ASSERT_TRUE(apply(store, op).ok);
        built.system().flushTerrainEdit(terrain, now);
        if (terrain.surfaceRevision != was) revisionsMade++;
        now += 1.0 / 60.0;
    }
    EXPECT_LE(revisionsMade, 5) << "one second of frames, the first one included";
    EXPECT_GE(revisionsMade, 3);

    op.type = OpType::Raise;
    ASSERT_TRUE(apply(store, op).ok);
    endTerrainStroke(terrain);
    const uint32_t atEnd = terrain.surfaceRevision;
    built.system().flushTerrainEdit(terrain, now);
    EXPECT_EQ(terrain.surfaceRevision, atEnd + 1) << "the last edit always counts";
    EXPECT_TRUE(terrain.needUpdateFoliage) << "foliage follows the revision";
    built.system().flushTerrainEdit(terrain, now + 1.0);
    EXPECT_EQ(terrain.surfaceRevision, atEnd + 1) << "nothing moved since";
}

// AC18 (T1 R23): spline conform follows an edit that no longer rebuilds the terrain. Red without the
// revision bump: needUpdateTerrain never rises, so the road would stay on the old surface.
TEST(TerrainEdit, SplineConformSeesRevision) {
    Scene scene;
    const Entity ground = scene.createEntity();
    scene.addComponent<Transform>(ground, {});
    scene.addComponent<TerrainComponent>(ground, {});
    TerrainComponent& terrain = scene.getComponent<TerrainComponent>(ground);
    makeTerrain(terrain, "t1-conform");
    terrain.needUpdateTerrain = false;

    const Entity road = scene.createEntity();
    scene.addComponent<Transform>(road, {});
    SplineComponent spline;
    spline::SplineKnot a;
    a.position = Vector3(-6.0f, 0.0f, 0.0f);
    a.radius = 1.0f;
    spline::SplineKnot b = a;
    b.position = Vector3(6.0f, 0.0f, 0.0f);
    spline.spline.setKnots({a, b});
    scene.addComponent<SplineComponent>(road, spline);
    MeshComponent mesh;
    mesh.numSubmeshes = 1;
    scene.addComponent<MeshComponent>(road, mesh);
    SplineExtrudeComponent extrude;
    extrude.shape = SplineExtrudeShape::Crown;
    extrude.conform = SplineConform::TerrainHeight;
    scene.addComponent<SplineExtrudeComponent>(road, extrude);

    SplineSystem& splines = *scene.getSystem<SplineSystem>();
    MeshSystem& meshes = *scene.getSystem<MeshSystem>();

    auto vertexBytes = [&]() {
        std::vector<unsigned char> bytes;
        for (Entity chunk : splines.getGeneratedEntities(road)) {
            const MeshComponent& part = scene.getComponent<MeshComponent>(chunk);
            const unsigned char* data = reinterpret_cast<const unsigned char*>(part.buffer.getData());
            bytes.insert(bytes.end(), data, data + part.buffer.getSize());
            const float y = scene.getComponent<Transform>(chunk).position.y;
            const unsigned char* origin = reinterpret_cast<const unsigned char*>(&y);
            bytes.insert(bytes.end(), origin, origin + sizeof(y));
        }
        return bytes;
    };
    auto frame = [&]() {
        meshes.update(0.016);
        splines.update(0.016);
    };

    frame();
    splines.rebuildNow(road);
    frame();
    const std::vector<unsigned char> onTheOldSurface = vertexBytes();
    ASSERT_FALSE(onTheOldSurface.empty());

    TerrainComponentStore store(&scene, ground, terrain);
    Op op;
    op.type = OpType::Raise;
    op.delta = 2.0f;
    ASSERT_TRUE(apply(store, op).ok);
    EXPECT_FALSE(terrain.needUpdateTerrain);
    frame();
    EXPECT_TRUE(splines.isRebuildPending(road) || vertexBytes() != onTheOldSurface) << "the spline noticed the surface moved";
    splines.rebuildNow(road);
    EXPECT_NE(vertexBytes(), onTheOldSurface) << "the road follows the raised ground";
}

TEST(TerrainEdit, NewTerrainDefaultsHaveOneHome) {
    // Nothing yet differs from the struct's defaults: T3 R53 and T5 F12 add theirs in this function
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
