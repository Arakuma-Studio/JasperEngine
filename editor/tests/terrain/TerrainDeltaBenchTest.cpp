// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 R49, AC38: the delta undo's budgets. The delta step is the editor's (TerrainMapDeltaCmd), so its
// times are this suite's rather than je-bench-terrain's (docs/perf/terrain.md). The wall-clock budgets
// are enforced only with JASPER_TEST_PERF_SLACK (perfSlack), at the spec's sizes; a plain run measures
// smaller maps and prints. The stored size of a stroke is a count, checked on every run.

#include "TerrainProjectFixture.h"
#include "component/TerrainComponent.h"

#include "command/type/TerrainMapDeltaCmd.h"
#include "support/EditorTestSupport.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace terrainprojecttest;

namespace {

    double msSince(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    double medianOf(std::vector<double> values) {
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

    class TerrainDeltaBench : public TerrainProjectFixture {
    protected:
        struct Timing {
            double commitMs = 0.0;
            double undoMs = 0.0;
            size_t tiles = 0;
        };

        // A stroke's end on a mapSize² 16-bit map: rect raised as a brush leaves it (a smooth change,
        // not noise), shown, then cut into a step and undone
        Timing strokeOver(int mapSize, terraincore::TexelRect rect, unsigned salt) {
            giveHeight16(mapSize);
            terrain().terrainSize = static_cast<float>(mapSize);
            EditorTerrainStore store(project.get(), scene->id, scene->scene, terrainEntity, terrain());
            terraincore::OverlayMapStore overlay(store);
            std::vector<uint8_t> bytes(static_cast<size_t>(rect.area()) * 2);
            for (int y = 0; y < rect.h; y++) {
                for (int x = 0; x < rect.w; x++) {
                    const unsigned value = 0x4040u + static_cast<unsigned>((x + y) / 3) + salt;
                    const size_t index = (static_cast<size_t>(y) * rect.w + x) * 2;
                    bytes[index] = static_cast<uint8_t>(value & 0xFF);
                    bytes[index + 1] = static_cast<uint8_t>(value >> 8);
                }
            }
            EXPECT_TRUE(overlay.writeBytes(HEIGHT_MAP, rect, bytes).ok);
            EXPECT_TRUE(overlay.flushToBase().ok);

            Timing timing;
            auto start = std::chrono::steady_clock::now();
            auto* step = new TerrainMapDeltaCmd(project.get(), scene->id, terrainEntity, terrainEntity, overlay, true);
            overlay.discard();
            EXPECT_TRUE(history()->addCommandNoMerge(step));
            timing.commitMs = msSince(start);
            timing.tiles = step->tileCount();

            start = std::chrono::steady_clock::now();
            EXPECT_TRUE(history()->undo());
            timing.undoMs = msSince(start);
            return timing;
        }

        static void report(const char* what, double ms, double budget, double slack) {
            std::printf("[bench] %-34s %8.3f ms (budget %.1f ms)\n", what, ms, budget);
            if (slack > 0.0) {
                EXPECT_LE(ms, budget * slack) << what;
            }
        }
    };

}

// R49: commit and undo of 64 changed tiles (a stroke), and of a whole 4096² map
TEST_F(TerrainDeltaBench, CommitAndUndoWithinBudget) {
    const double slack = editor::test::perfSlack();
    std::vector<double> commits;
    std::vector<double> undos;
    for (unsigned run = 0; run < 5; run++) {
        // 8 x 8 tiles of 64, somewhere else each time
        const int origin = 64 * static_cast<int>(run);
        const Timing timing = strokeOver(1024, terraincore::TexelRect{origin, origin, 512, 512}, run);
        ASSERT_EQ(timing.tiles, 64u);
        commits.push_back(timing.commitMs);
        undos.push_back(timing.undoMs);
    }
    report("commit 64 tiles", medianOf(commits), 2.0, slack);
    report("undo 64 tiles", medianOf(undos), 1.0, slack);

    const int whole = slack > 0.0 ? 4096 : 1024;
    const Timing map = strokeOver(whole, terraincore::TexelRect{0, 0, whole, whole}, 7);
    ASSERT_EQ(map.tiles, static_cast<size_t>(whole / 64) * static_cast<size_t>(whole / 64));
    std::printf("[bench] whole map is %d^2\n", whole);
    report("commit a whole map", map.commitMs, 60.0, slack);
    report("undo a whole map", map.undoMs, 60.0, slack);
}

// R49: a radius-64 stroke along the diagonal of a 2048² map stores at most an eighth of what today's
// step stored, its rectangle of the map's bytes (TerrainTextureEditCmd kept the stroke's bounding box)
TEST_F(TerrainDeltaBench, ADiagonalStrokeStoresAnEighthOfTodaysPatch) {
    constexpr int N = 2048;
    constexpr float RADIUS = 64.0f;
    giveHeight16(N);
    terrain().terrainSize = static_cast<float>(N);
    EditorTerrainStore store(project.get(), scene->id, scene->scene, terrainEntity, terrain());
    terraincore::OverlayMapStore overlay(store);
    terraincore::Op op;
    op.type = terraincore::OpType::Raise;
    op.delta = 0.5f;
    op.shape.type = terraincore::ShapeType::Stroke;
    op.shape.radius = RADIUS;
    const float end = static_cast<float>(N) * 0.5f - RADIUS;
    op.shape.points = {Vector2(-end, -end), Vector2(end, end)};
    ASSERT_TRUE(runOn(overlay, op).ok);

    const TerrainMapDeltaCmd step(project.get(), scene->id, terrainEntity, terrainEntity, overlay, false);
    ASSERT_FALSE(step.empty());
    // today's rectangle: the stroke's bounding box, every texel of it
    const size_t side = static_cast<size_t>(2.0f * end + 2.0f * RADIUS);
    const size_t todaysPatch = side * side * 2;
    std::printf("[bench] diagonal stroke: %zu tiles, %zu bytes stored, today's patch %zu bytes\n", step.tileCount(),
                step.memoryBytes(), todaysPatch);
    EXPECT_LE(step.memoryBytes(), todaysPatch / 8);
}
