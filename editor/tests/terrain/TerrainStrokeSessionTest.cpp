// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A brush stroke on the engine (T1 R17, R18, R26, R27, R36; AC20): pure, over a RawMapStore, except
// for the routes that have to reach a Project (AC15, AC64).

#include "TerrainProjectFixture.h"
#include "object/TerrainOpsJson.h"
#include "json.hpp"
#include "component/TerrainComponent.h"
#include "component/Transform.h"

#include "command/type/TerrainMapDeltaCmd.h"
#include "component/SplineComponent.h"
#include "support/AllocationCounter.h"
#include "terrain/RawMapStore.h"
#include "terrain/TerrainStrokeSession.h"
#include "util/TerrainSplineBake.h"

#include <cmath>
#include <cstring>
#include <functional>

using namespace terrainprojecttest;

namespace {

    constexpr float FLOW = 1.5f;
    constexpr float CLICK_SECONDS = 1.0f / 60.0f;

    void addHeight(terrain::RawMapStore& store, int size, unsigned value) {
        std::vector<uint8_t> bytes(static_cast<size_t>(size) * size * 2);
        for (size_t i = 0; i < bytes.size(); i += 2) {
            bytes[i] = static_cast<uint8_t>(value & 0xFF);
            bytes[i + 1] = static_cast<uint8_t>(value >> 8);
        }
        store.addMap(HEIGHT_MAP, size, size, 1, 16, std::move(bytes));
    }

    unsigned heightAt(const terrain::RawMapStore& store, int size, int x, int y) {
        const uint8_t* bytes = store.mapBytes(HEIGHT_MAP);
        const size_t index = (static_cast<size_t>(y) * size + x) * 2;
        return bytes[index] | (static_cast<unsigned>(bytes[index + 1]) << 8);
    }

    TerrainStrokeBrush raiseBrush(float size, float strength) {
        TerrainStrokeBrush brush;
        brush.mode = TerrainBrushMode::Raise;
        brush.size = size;
        brush.strength = strength;
        return brush;
    }

}

// R34: a click is one stamp of strength * falloff * flow * 1/60 s
TEST(TerrainStrokeSession, AClickDepositsOneStamp) {
    constexpr int N = 65;
    terrain::RawMapStore store(static_cast<float>(N), 10.0f);
    addHeight(store, N, 0);
    TerrainStrokeSession session(store, HEIGHT_MAP, static_cast<float>(N), raiseBrush(4.0f, 0.5f));
    ASSERT_TRUE(session.moveTo(0.0f, 0.0f, CLICK_SECONDS));
    ASSERT_TRUE(session.flush());
    // the brush's centre, on texel 32's sample: full falloff
    const unsigned expected = static_cast<unsigned>(std::lround(0.5f * FLOW * CLICK_SECONDS * 65535.0f));
    EXPECT_NEAR(static_cast<double>(heightAt(store, N, 32, 32)), expected, 1.0);
    EXPECT_EQ(heightAt(store, N, 32 + 6, 32), 0u) << "past the radius";
}

// DC2: stamps too gentle for the stored bytes add up in the floats, through flushes
TEST(TerrainStrokeSession, GentleStampsAccumulateThroughFlushes) {
    constexpr int N = 65;
    terrain::RawMapStore store(static_cast<float>(N), 10.0f);
    addHeight(store, N, 0);
    TerrainStrokeSession session(store, HEIGHT_MAP, static_cast<float>(N), raiseBrush(4.0f, 0.01f));
    const float seconds = 0.2f / (65535.0f * 0.01f * FLOW);    // a fifth of a stored step, per stamp
    for (int i = 0; i < 50; i++) {
        ASSERT_TRUE(session.moveTo(0.0f, 0.0f, seconds));
        ASSERT_TRUE(session.flush());
    }
    EXPECT_NEAR(static_cast<double>(heightAt(store, N, 32, 32)), 10.0, 1.0) << "fifty fifths of a step, not fifty rounds to zero";
}

// R18: the stamps of a move fall as TEW:1607-1628 cut them, and share its time
TEST(TerrainStrokeSession, AMoveCutsItsTimeOverItsStamps) {
    constexpr int N = 129;
    terrain::RawMapStore cut(static_cast<float>(N), 10.0f);
    addHeight(cut, N, 0);
    terrain::RawMapStore handMade(static_cast<float>(N), 10.0f);
    addHeight(handMade, N, 0);

    TerrainStrokeSession moved(cut, HEIGHT_MAP, static_cast<float>(N), raiseBrush(8.0f, 0.5f));
    ASSERT_TRUE(moved.moveTo(-20.0f, 0.0f, 0.05f));
    ASSERT_TRUE(moved.moveTo(20.0f, 0.0f, 0.05f));
    ASSERT_TRUE(moved.flush());

    // spacing 8 * 0.25 = 2 over 40 units: 20 stamps sharing 0.05 s
    TerrainStrokeSession stamped(handMade, HEIGHT_MAP, static_cast<float>(N), raiseBrush(8.0f, 0.5f));
    ASSERT_TRUE(stamped.moveTo(-20.0f, 0.0f, 0.05f));
    stamped.liftAnchor();
    for (int i = 1; i <= 20; i++) {
        ASSERT_TRUE(stamped.moveTo(-20.0f + 40.0f * (static_cast<float>(i) / 20.0f), 0.0f, 0.05f / 20.0f));
        stamped.liftAnchor();
    }
    ASSERT_TRUE(stamped.flush());
    for (int x = 0; x < N; x++) {
        ASSERT_NEAR(static_cast<double>(heightAt(cut, N, x, 64)), static_cast<double>(heightAt(handMade, N, x, 64)), 1.0) << "column " << x;
    }
}

// R36: a person's ramp depends only on its two ends; sculpt_terrain's accumulates (R18)
TEST(TerrainStrokeSession, AMouseRampDependsOnlyOnItsEnds) {
    constexpr int N = 65;
    auto rampThrough = [&](const std::vector<Vector2>& path, bool accumulate) {
        auto store = std::make_unique<terrain::RawMapStore>(static_cast<float>(N), 10.0f);
        addHeight(*store, N, 20000);
        std::vector<uint8_t> raised(static_cast<size_t>(4) * 4 * 2);
        for (size_t i = 0; i < raised.size(); i += 2) {
            raised[i] = 0;
            raised[i + 1] = 0xC0;
        }
        store->writeBytes(HEIGHT_MAP, terrain::TexelRect{50, 30, 4, 4}, raised);
        TerrainStrokeBrush brush = raiseBrush(5.0f, 0.6f);
        brush.mode = TerrainBrushMode::Ramp;
        brush.accumulateRamp = accumulate;
        TerrainStrokeSession session(*store, HEIGHT_MAP, static_cast<float>(N), std::move(brush));
        session.setRampStart(path.front().x, path.front().y);
        for (const Vector2& point : path) {
            session.moveTo(point.x, point.y, 0.05f);
            session.flush();
        }
        return std::vector<uint8_t>(store->mapBytes(HEIGHT_MAP), store->mapBytes(HEIGHT_MAP) + store->mapByteCount(HEIGHT_MAP));
    };
    const Vector2 start(-20.0f, 0.0f);
    const Vector2 end(20.0f, 0.0f);
    const std::vector<uint8_t> direct = rampThrough({start, end}, false);
    const std::vector<uint8_t> wandering = rampThrough({start, Vector2(0.0f, 15.0f), Vector2(10.0f, -12.0f), end}, false);
    EXPECT_EQ(direct, wandering) << "the tiles it wandered over went back";
    EXPECT_NE(rampThrough({start, Vector2(0.0f, 15.0f), end}, true), rampThrough({start, end}, true));
}

// AC30 (R33): one drag sampled at 20 and at 240 events a second leaves the same bytes, and the
// frame budget changes when stamps run, not what they write. Red with a stamp per mouse event.
TEST(TerrainStrokeSession, TheMouseRateDoesNotChangeTheDrag) {
    constexpr int N = 129;
    auto drag = [&](int eventsPerSecond, double budget) {
        auto store = std::make_unique<terrain::RawMapStore>(static_cast<float>(N), 10.0f);
        addHeight(*store, N, 20000);
        TerrainStrokeSession session(*store, HEIGHT_MAP, static_cast<float>(N), raiseBrush(6.0f, 0.3f));
        const int events = eventsPerSecond;   // one second from x = -40 to x = 40
        for (int i = 0; i <= events; i++) {
            const float t = static_cast<float>(i) / static_cast<float>(events);
            session.addSample(t, -40.0f + 80.0f * t, 0.0f, 1.0f);
            session.run(budget);
        }
        session.run(0.0);
        EXPECT_FALSE(session.hasQueued());
        session.flush();
        return std::vector<uint8_t>(store->mapBytes(HEIGHT_MAP), store->mapBytes(HEIGHT_MAP) + store->mapByteCount(HEIGHT_MAP));
    };
    const std::vector<uint8_t> slow = drag(20, 0.0);
    EXPECT_EQ(drag(240, 0.0), slow) << "the stamps sit at the brush's spacing, not at the events";
    EXPECT_EQ(drag(240, 1e-9), slow) << "a tight frame budget only delays them";
    terrain::RawMapStore flat(static_cast<float>(N), 10.0f);
    addHeight(flat, N, 20000);
    EXPECT_NE(slow, std::vector<uint8_t>(flat.mapBytes(HEIGHT_MAP), flat.mapBytes(HEIGHT_MAP) + flat.mapByteCount(HEIGHT_MAP))) << "the drag wrote";
}

// R34, R35: a pointer held still keeps stamping at 60 a second (one sample a frame), each a click's deposit
TEST(TerrainStrokeSession, AHeldBrushStampsAtSixtyASecond) {
    constexpr int N = 65;
    auto store = std::make_unique<terrain::RawMapStore>(static_cast<float>(N), 10.0f);
    addHeight(*store, N, 20000);
    TerrainStrokeSession session(*store, HEIGHT_MAP, static_cast<float>(N), raiseBrush(4.0f, 0.5f));
    session.addSample(0.0f, 0.0f, 0.0f, 1.0f);
    session.run(0.0);
    session.flush();
    const unsigned once = heightAt(*store, N, N / 2, N / 2) - 20000;
    ASSERT_GT(once, 0u);
    for (int frame = 1; frame <= 30; frame++) session.addSample(static_cast<float>(frame) / 60.0f, 0.0f, 0.0f, 1.0f);
    session.run(0.0);
    session.flush();
    const unsigned held = heightAt(*store, N, N / 2, N / 2) - 20000;
    EXPECT_NEAR(static_cast<double>(held) / static_cast<double>(once), 31.0, 1.5) << "half a second of frames is 30 more stamps";
}

// R27: Esc goes back through the before tiles
TEST(TerrainStrokeSession, CancelRestoresEveryByte) {
    constexpr int N = 200;
    terrain::RawMapStore store(static_cast<float>(N), 10.0f);
    addHeight(store, N, 12345);
    const std::vector<uint8_t> before(store.mapBytes(HEIGHT_MAP), store.mapBytes(HEIGHT_MAP) + store.mapByteCount(HEIGHT_MAP));
    TerrainStrokeSession session(store, HEIGHT_MAP, static_cast<float>(N), raiseBrush(10.0f, 1.0f));
    ASSERT_TRUE(session.moveTo(-60.0f, -60.0f, 0.05f));
    ASSERT_TRUE(session.moveTo(60.0f, 60.0f, 0.05f));
    ASSERT_TRUE(session.flush());
    EXPECT_NE(std::memcmp(store.mapBytes(HEIGHT_MAP), before.data(), before.size()), 0);
    session.cancel();
    EXPECT_EQ(std::memcmp(store.mapBytes(HEIGHT_MAP), before.data(), before.size()), 0);
}

// The heights a stroke reads for its ramp and its cursor are those it found, wherever it has been
TEST(TerrainStrokeSession, HeightBeforeIsTheMapItFound) {
    constexpr int N = 65;
    terrain::RawMapStore store(static_cast<float>(N), 10.0f);
    addHeight(store, N, 32768);
    TerrainStrokeSession session(store, HEIGHT_MAP, static_cast<float>(N), raiseBrush(8.0f, 1.0f));
    const float found = session.heightBefore(0.0f, 0.0f);
    EXPECT_NEAR(found, 32768.0f / 65535.0f, 1e-5f);
    ASSERT_TRUE(session.moveTo(0.0f, 0.0f, 0.05f));
    ASSERT_TRUE(session.flush());
    EXPECT_GT(heightAt(store, N, 32, 32), 32768u);
    EXPECT_EQ(session.heightBefore(0.0f, 0.0f), found);
}

// AC20 (R26): the memory of a small stroke does not depend on the map. Red with an eager before copy.
TEST(TerrainStrokeSession, SmallStrokeOnHugeMapNoWholeMapAlloc) {
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "no allocation counter on this platform";
    constexpr int N = 4096;
    terrain::RawMapStore store(static_cast<float>(N), 100.0f);
    addHeight(store, N, 20000);

    testsupport::resetLargestAllocation();
    const uint64_t bytesBefore = testsupport::totalAllocatedBytes();
    {
        // inside one 64x64 tile, away from its edges
        TerrainStrokeSession session(store, HEIGHT_MAP, static_cast<float>(N), raiseBrush(4.0f, 0.5f));
        const float x = 32.0f - static_cast<float>(N) * 0.5f;
        ASSERT_TRUE(session.moveTo(x, x, CLICK_SECONDS));
        ASSERT_TRUE(session.moveTo(x + 2.0f, x, CLICK_SECONDS));
        ASSERT_TRUE(session.flush());
        EXPECT_EQ(session.overlay().tileCount(), 1u);
        session.cancel();
    }
    const uint64_t allocated = testsupport::totalAllocatedBytes() - bytesBefore;
    EXPECT_LT(allocated, 1024u * 1024u) << "all of the stroke's allocations together";
    EXPECT_LT(testsupport::largestAllocation(), 256u * 1024u) << "its largest buffer";
}

namespace {

    class TerrainSplineLiveSessionTest : public TerrainProjectFixture {};

    Entity addRoad(Scene* s, Entity terrainEntity, float fromX, float toX) {
        const Entity road = s->createEntity();
        s->addComponent<Transform>(road, {});
        SplineComponent spline;
        spline::SplineKnot a;
        a.position = Vector3(fromX, 3.0f, 0.0f);
        a.radius = 3.0f;
        spline::SplineKnot b = a;
        b.position = Vector3(toX, 3.0f, 0.0f);
        spline.spline.setKnots({a, b});
        spline.terrain.terrain = terrainEntity;
        s->addComponent<SplineComponent>(road, spline);
        return road;
    }

}

// AC15 (R20): the session holds the tiles it has been over, not a copy of each map
TEST_F(TerrainSplineLiveSessionTest, LiveSessionAllocatesNoWholeMap) {
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "no allocation counter on this platform";
    constexpr int N = 2048;
    giveHeight16(N);
    settleFiles();
    const std::vector<unsigned char> before = bytesOf(HEIGHT_MAP);
    const Entity road = addRoad(scene->scene, terrainEntity, -20.0f, 20.0f);
    const size_t mapBytes = before.size();

    TerrainSplineLiveSession session;
    std::string error;
    testsupport::resetLargestAllocation();
    ASSERT_TRUE(session.begin(project.get(), scene->id, road, error)) << error;
    SplineComponent& spline = scene->scene->getComponent<SplineComponent>(road);
    spline::SplineKnot a = spline.spline.knot(0);
    spline::SplineKnot b = spline.spline.knot(1);
    a.position.z += 10.0f;
    b.position.z += 10.0f;
    spline.spline.setKnots({a, b});
    EXPECT_TRUE(session.update(error)) << error;
    EXPECT_LT(testsupport::largestAllocation(), mapBytes / 4) << "the map is " << mapBytes << " bytes";
    session.cancel();
    EXPECT_EQ(bytesOf(HEIGHT_MAP), before);
}

namespace {

    class TerrainEditorRoutesTest : public TerrainProjectFixture {
    protected:
        int deliveriesAfterAFrame(const std::function<void()>& edit) {
            int deliveries = 0;
            Subscription subscription = TerrainEditEvents::subscribe(scene->scene, [&](Entity entity, terrain::MapId, terrain::TexelRect) {
                if (entity == terrainEntity) deliveries++;
            });
            edit();
            TerrainEditEvents::flush(scene->scene);
            return deliveries;
        }
    };

}

// AC64 (R42): the routes that need the editor publish as the others do: brush, bake and undo
TEST_F(TerrainEditorRoutesTest, BrushBakeAndUndoPublish) {
    giveHeight16(128);
    EXPECT_EQ(deliveriesAfterAFrame([&]() {
        EditorTerrainStore store(project.get(), scene->id, scene->scene, terrainEntity, terrain());
        TerrainStrokeSession session(store, HEIGHT_MAP, terrain().terrainSize, raiseBrush(8.0f, 1.0f));
        ASSERT_TRUE(session.moveTo(0.0f, 0.0f, 0.05f));
        ASSERT_TRUE(session.flush());
        auto* step = new TerrainMapDeltaCmd(project.get(), scene->id, terrainEntity, terrainEntity, session.overlay(), true);
        ASSERT_TRUE(history()->addCommandNoMerge(step));
    }), 1) << "the brush";

    EXPECT_EQ(deliveriesAfterAFrame([&]() { ASSERT_TRUE(history()->undo()); }), 1) << "undo";
    EXPECT_EQ(deliveriesAfterAFrame([&]() { ASSERT_TRUE(history()->redo()); }), 1) << "redo";

    const Entity road = addRoad(scene->scene, terrainEntity, -20.0f, 20.0f);
    EXPECT_EQ(deliveriesAfterAFrame([&]() {
        TerrainBakeChanges changes;
        std::string error;
        ASSERT_TRUE(TerrainSplineBake::apply(project.get(), scene->id, road, changes, error)) << error;
    }), 1) << "the spline bake";
}

// R41: a second terrain on the same file follows without a pixel copied, and refits too
TEST_F(TerrainEditorRoutesTest, ASharingTerrainFollows) {
    giveHeight16(128);
    const Entity other = addTerrain();
    TerrainComponent& second = scene->scene->getComponent<TerrainComponent>(other);
    second.heightMap = terrain().heightMap;
    second.heightMap.setReleaseDataAfterLoad(false);
    const void* pixels = terrain().heightMap.getData().getData();
    ASSERT_EQ(second.heightMap.getData().getData(), pixels);

    {
        EditorTerrainStore store(project.get(), scene->id, scene->scene, terrainEntity, terrain());
        terrain::OverlayMapStore overlay(store);
        ASSERT_TRUE(runOn(overlay, raiseOp(4.0f)).ok);
        ASSERT_TRUE(overlay.flushToBase().ok);
    }
    TerrainComponent& followed = scene->scene->getComponent<TerrainComponent>(other);
    EXPECT_EQ(followed.heightMap.getData().getData(), pixels) << "the same pool entry";
    ASSERT_NE(followed.editState, nullptr);
    EXPECT_TRUE(followed.editState->surfaceMoved);
    EXPECT_FALSE(followed.needUpdateTerrain) << "no rebuild for the follower either";
}

// T3 R41, R29: the hole brush cuts whole texels whatever its strength, and while a stroke runs the
// map stays, so filling the last hole shows at once; only the stroke's end may take the map away
TEST_F(TerrainEditorRoutesTest, HoleBrushKeepsTheMapThroughAStroke) {
    constexpr terrain::MapId HOLE_MAP{terrain::MapKind::Hole, 0};
    giveHeight16(128);
    const TerrainMapRef holeRef(TerrainMapTarget::HoleMap);
    ASSERT_TRUE(TerrainMapUtils::ensureEditableMap(project.get(), scene, terrainEntity, holeRef, 128, false));
    madeIds.push_back(terrain().holeMap.getId());
    EXPECT_FALSE(querySurface(terrain(), 0.0f, 0.0f).hole) << "a new hole map is solid";

    EditorTerrainStore store(project.get(), scene->id, scene->scene, terrainEntity, terrain());
    store.beginStroke();
    store.setKeepsMaps(true);
    TerrainStrokeBrush cut;
    cut.mode = TerrainBrushMode::CutHole;
    cut.size = 4.0f;
    cut.strength = 0.05f;
    cut.falloff = TerrainBrushFalloff::Linear;
    {
        TerrainStrokeSession session(store, HOLE_MAP, terrain().terrainSize, cut);
        ASSERT_TRUE(session.moveTo(0.0f, 0.0f, CLICK_SECONDS));
        ASSERT_TRUE(session.flush());
    }
    EXPECT_TRUE(querySurface(terrain(), 0.0f, 0.0f).hole) << "a whole texel, though the strength is low";
    EXPECT_FALSE(querySurface(terrain(), 10.0f, 0.0f).hole) << "past the radius";

    TerrainStrokeBrush fill = cut;
    fill.mode = TerrainBrushMode::FillHole;
    {
        TerrainStrokeSession session(store, HOLE_MAP, terrain().terrainSize, fill);
        ASSERT_TRUE(session.moveTo(0.0f, 0.0f, CLICK_SECONDS));
        ASSERT_TRUE(session.flush());
    }
    EXPECT_FALSE(terrain().holeMap.empty()) << "the stroke holds the map";
    EXPECT_FALSE(querySurface(terrain(), 0.0f, 0.0f).hole) << "the last fill shows";
    EXPECT_FALSE(store.canRemoveMap(HOLE_MAP));
    store.setKeepsMaps(false);
    store.endStroke();
    EXPECT_TRUE(store.canRemoveMap(HOLE_MAP)) << "the stroke's end may take it away";
}

// R56: Raise's alternate lifts floors to the picked height and leaves what is above; Lower's (Ctrl)
// flattens peaks to it and leaves what is below
TEST(TerrainStrokeSession, AlternatesLiftFloorsAndFlattenPeaks) {
    constexpr int N = 65;
    constexpr unsigned LOW = 10000, HIGH = 50000, TARGET = 30000;
    for (const TerrainBrushMode mode : {TerrainBrushMode::Raise, TerrainBrushMode::Lower}) {
        terrain::RawMapStore store(static_cast<float>(N), 10.0f);
        std::vector<uint8_t> bytes(static_cast<size_t>(N) * N * 2);
        for (int y = 0; y < N; y++) {
            for (int x = 0; x < N; x++) {
                const unsigned value = x < N / 2 ? LOW : HIGH;
                const size_t at = (static_cast<size_t>(y) * N + x) * 2;
                bytes[at] = static_cast<uint8_t>(value & 0xFF);
                bytes[at + 1] = static_cast<uint8_t>(value >> 8);
            }
        }
        store.addMap(HEIGHT_MAP, N, N, 1, 16, std::move(bytes));
        TerrainStrokeBrush brush = raiseBrush(10.0f, 1.0f);
        brush.mode = mode;
        brush.falloff = TerrainBrushFalloff::Constant;
        brush.alternate = true;
        brush.flattenTarget = static_cast<float>(TARGET) / 65535.0f;
        TerrainStrokeSession session(store, HEIGHT_MAP, static_cast<float>(N), brush);
        for (int i = 0; i < 400; i++) ASSERT_TRUE(session.moveTo(0.0f, 0.0f, CLICK_SECONDS));
        ASSERT_TRUE(session.flush());
        const unsigned left = heightAt(store, N, N / 2 - 3, N / 2);
        const unsigned right = heightAt(store, N, N / 2 + 3, N / 2);
        if (mode == TerrainBrushMode::Raise) {
            EXPECT_NEAR(static_cast<double>(left), TARGET, 300.0) << "the floor rose to the picked height";
            EXPECT_EQ(right, HIGH) << "what is above it stays";
        } else {
            EXPECT_EQ(left, LOW) << "what is below it stays";
            EXPECT_NEAR(static_cast<double>(right), TARGET, 300.0) << "the peak came down to the picked height";
        }
    }
}

// R39: a mouse stroke written as its edit_terrain call replays to the very same bytes
TEST(TerrainStrokeSession, TheStrokeLogReplaysToTheSameBytes) {
    constexpr int N = 65;
    for (const TerrainBrushMode mode : {TerrainBrushMode::Raise, TerrainBrushMode::Flatten}) {
        terrain::RawMapStore dragged(static_cast<float>(N), 10.0f);
        terrain::RawMapStore replayed(static_cast<float>(N), 10.0f);
        addHeight(dragged, N, 9000);
        addHeight(replayed, N, 9000);
        TerrainStrokeBrush brush = raiseBrush(5.0f, 0.6f);
        brush.mode = mode;
        brush.flattenTarget = 0.4f;
        TerrainStrokeSession session(dragged, HEIGHT_MAP, static_cast<float>(N), brush);
        std::vector<terrain::StrokeSample> samples;
        for (int i = 0; i < 24; i++) {
            const float t = static_cast<float>(i) / 23.0f;
            samples.push_back(terrain::StrokeSample{t * 0.4f, -20.0f + 40.0f * t, 12.0f * std::sin(t * 3.0f), 1.0f});
            session.addSample(samples.back().time, samples.back().x, samples.back().z, 1.0f);
        }
        ASSERT_TRUE(session.run(0.0));
        ASSERT_TRUE(session.flush());

        const std::string line = session.replayCall(samples);
        ASSERT_FALSE(line.empty()) << "a round brush with no mask is logged";
        const nlohmann::json call = nlohmann::json::parse(line);
        EXPECT_EQ(call["space"], "local");
        terrain::OpSpace space;
        space.world = false;
        terrain::OpJsonParser parser(space, terrain::OpJsonHooks{});
        std::vector<terrain::Op> ops;
        ASSERT_TRUE(parser.parseList(call["ops"], ops)) << parser.error;
        ASSERT_TRUE(terrain::applyOps(replayed, ops, terrain::OpOptions()).ok);
        const size_t bytes = static_cast<size_t>(N) * N * 2;
        EXPECT_EQ(std::memcmp(dragged.mapBytes(HEIGHT_MAP), replayed.mapBytes(HEIGHT_MAP), bytes), 0) << "byte for byte, mode " << static_cast<int>(mode);
    }
}

// R56: Paint Texture's alternate moves the weights toward their true 3 x 3 average, so a hard edge softens
TEST(TerrainStrokeSession, PaintTextureAlternateAveragesTheBlend) {
    constexpr int N = 33;
    const terrain::MapId blend{terrain::MapKind::Blend, 0};
    terrain::RawMapStore store(static_cast<float>(N), 10.0f);
    addHeight(store, N, 9000);
    std::vector<uint8_t> bytes(static_cast<size_t>(N) * N * 4, 0);
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            const size_t at = (static_cast<size_t>(y) * N + x) * 4;
            bytes[at] = x < N / 2 ? 255 : 0;   // layer 0 on the left half
            bytes[at + 3] = 255;
        }
    }
    store.addMap(blend, N, N, 4, 8, std::move(bytes));
    TerrainStrokeBrush brush = raiseBrush(6.0f, 1.0f);
    brush.mode = TerrainBrushMode::PaintLayer;
    brush.textureLayer = 0;
    brush.alternate = true;
    TerrainStrokeSession session(store, blend, static_cast<float>(N), brush);
    for (int i = 0; i < 30; i++) ASSERT_TRUE(session.moveTo(0.0f, 0.0f, CLICK_SECONDS));
    ASSERT_TRUE(session.flush());
    const uint8_t* after = store.mapBytes(blend);
    const uint8_t left = after[(static_cast<size_t>(N / 2) * N + N / 2 - 1) * 4];
    const uint8_t right = after[(static_cast<size_t>(N / 2) * N + N / 2) * 4];
    EXPECT_GT(left, 20);
    EXPECT_LT(left, 255) << "the full side gave some weight to the edge";
    EXPECT_GT(right, 0) << "and the empty side took some";
    EXPECT_LT(right, 235);
}
