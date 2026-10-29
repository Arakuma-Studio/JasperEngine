// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Tests/SplineFillFieldTests.cpp, Copyright
// (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

// The FitToBanks field and flood on synthetic heightfields (S18 R7, AC6). Connectivity is the
// property under test, not "does it fill". The fill works in world XZ, so OpenEngine's scenes keep
// their coordinates; only the right of travel is this engine's (forward x up), which the signed
// lateral distance carries. Plus: the corridor block (a field never stores the whole terrain), the
// knot-naming diagnostics, and the R19 budget on a 1025^2 lattice.

#include "SplineGeometryTestSupport.h"
#include "spline/SplineFillField.h"
#include "AllocationCounter.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::spline;
using namespace jasperengine::spline::geometrytest;

namespace{

    // a ground lattice plus the storage behind it
    struct FieldTestGround{
        std::vector<float> heights;
        SplineGroundGrid grid;

        FieldTestGround(uint32_t countX, uint32_t countZ, float spacing, float originX, float originZ,
                        const std::function<float(float, float)>& height){
            grid.originX = originX;
            grid.originZ = originZ;
            grid.spacingX = spacing;
            grid.spacingZ = spacing;
            grid.countX = countX;
            grid.countZ = countZ;
            heights.resize(static_cast<size_t>(countX) * countZ);
            for (uint32_t cz = 0; cz < countZ; cz++){
                for (uint32_t cx = 0; cx < countX; cx++){
                    heights[grid.index(cx, cz)] = height(grid.worldX(cx), grid.worldZ(cz));
                }
            }
            grid.heights = heights;
        }
    };

    // The extrude system's station construction: central-difference forward, right = forward x up
    // in the level plane, up = right x forward, distance walked in true 3D length. Points are AT the
    // waterline.
    std::vector<SplineStation> fieldMakeStations(const std::vector<Vector3>& points){
        std::vector<SplineStation> stations(points.size());
        float distance = 0.0f;
        for (size_t i = 0; i < points.size(); i++){
            if (i > 0u){
                distance += lengthOf(points[i] - points[i - 1u]);
            }
            const Vector3 ahead = points[std::min(i + 1u, points.size() - 1u)];
            const Vector3 behind = points[i == 0u ? 0u : i - 1u];
            Vector3 forward = ahead - behind;
            const float length = lengthOf(forward);
            forward = length > 1.0e-6f ? forward * (1.0f / length) : Vector3(0.0f, 0.0f, 1.0f);
            Vector3 right = crossOf(Vector3(forward.x, 0.0f, forward.z), UP);
            const float rightLength = lengthOf(right);
            right = rightLength > 1.0e-6f ? right * (1.0f / rightLength) : Vector3(1.0f, 0.0f, 0.0f);

            stations[i].position = points[i];
            stations[i].forward = forward;
            stations[i].right = right;
            stations[i].up = crossOf(right, forward);
            stations[i].distance = distance;
        }
        return stations;
    }

    // a straight run down +Z at x = 0, every station at the same altitude
    std::vector<SplineStation> fieldStraightRun(float waterline, float fromZ, float toZ, float step){
        std::vector<Vector3> points;
        for (float z = fromZ; z <= toZ + 1.0e-4f; z += step){
            points.push_back(Vector3(0.0f, waterline, z));
        }
        return fieldMakeStations(points);
    }

    const SplineFillCorner& fieldAt(const SplineFillResult& field, uint32_t cx, uint32_t cz){
        return field.corner(cx, cz);
    }

    // 4-connected components among wet corners, over the whole grid
    uint32_t fieldCountWetComponents(const SplineFillResult& field, const SplineGroundGrid& grid){
        std::vector<uint8_t> seen(grid.cornerCount(), 0u);
        uint32_t components = 0;
        std::vector<uint32_t> stack;
        const auto wet = [&](uint32_t index){ return field.corner(index % grid.countX, index / grid.countX).wet != 0u; };
        for (uint32_t start = 0; start < grid.cornerCount(); start++){
            if (!wet(start) || seen[start]){
                continue;
            }
            components++;
            stack.push_back(start);
            seen[start] = 1u;
            while (!stack.empty()){
                const uint32_t index = stack.back();
                stack.pop_back();
                const uint32_t cx = index % grid.countX;
                const uint32_t cz = index / grid.countX;
                const auto visit = [&](uint32_t other){
                    if (wet(other) && !seen[other]){
                        seen[other] = 1u;
                        stack.push_back(other);
                    }
                };
                if (cx > 0u){
                    visit(index - 1u);
                }
                if (cx + 1u < grid.countX){
                    visit(index + 1u);
                }
                if (cz > 0u){
                    visit(index - grid.countX);
                }
                if (cz + 1u < grid.countZ){
                    visit(index + grid.countX);
                }
            }
        }
        return components;
    }

    // Dry corners the wet region encloses on every side whose own ground is below the water around
    // them: water cannot drain through water, so this must be zero. Ground ABOVE the waterline is an
    // islet and stays dry-topped.
    uint32_t fieldCountEnclosedDryBelowWaterline(const SplineFillResult& field, const SplineGroundGrid& grid){
        std::vector<uint8_t> reachable(grid.cornerCount(), 0u);
        std::vector<uint32_t> stack;
        const auto at = [&](uint32_t index) -> const SplineFillCorner&{ return field.corner(index % grid.countX, index / grid.countX); };
        const auto visit = [&](uint32_t index){
            if (reachable[index] || at(index).wet){
                return;
            }
            reachable[index] = 1u;
            stack.push_back(index);
        };
        for (uint32_t cx = 0; cx < grid.countX; cx++){
            visit(grid.index(cx, 0u));
            visit(grid.index(cx, grid.countZ - 1u));
        }
        for (uint32_t cz = 0; cz < grid.countZ; cz++){
            visit(grid.index(0u, cz));
            visit(grid.index(grid.countX - 1u, cz));
        }
        while (!stack.empty()){
            const uint32_t index = stack.back();
            stack.pop_back();
            const uint32_t cx = index % grid.countX;
            const uint32_t cz = index / grid.countX;
            if (cx > 0u){
                visit(index - 1u);
            }
            if (cx + 1u < grid.countX){
                visit(index + 1u);
            }
            if (cz > 0u){
                visit(index - grid.countX);
            }
            if (cz + 1u < grid.countZ){
                visit(index + grid.countX);
            }
        }

        uint32_t enclosed = 0;
        for (uint32_t index = 0; index < grid.cornerCount(); index++){
            const SplineFillCorner& corner = at(index);
            if (corner.wet || reachable[index] || corner.station == SPLINE_FILL_NO_STATION){
                continue;
            }
            if (corner.waterline - grid.heights[index] > 0.0f){
                enclosed++;
            }
        }
        return enclosed;
    }

    // A run that doubles back on itself over ground whose inner bank carries a low col.
    std::vector<SplineStation> fieldHairpinRun(float waterline){
        std::vector<Vector3> points;
        for (float z = -18.0f; z <= 0.0f; z += 0.5f){
            points.push_back(Vector3(-6.0f, waterline, z));
        }
        for (float a = 0.0f; a <= 3.14159f; a += 0.15f){
            points.push_back(Vector3(-6.0f + 6.0f * (1.0f - std::cos(a)), waterline, 6.0f * std::sin(a)));
        }
        for (float z = 0.0f; z >= -18.0f; z -= 0.5f){
            points.push_back(Vector3(6.0f, waterline, z));
        }
        return fieldMakeStations(points);
    }

    // a run descending from 2 m to -1 m down a channel that continues below sea level
    std::vector<SplineStation> fieldDescendingRun(){
        std::vector<Vector3> points;
        for (float z = -15.0f; z <= 15.0f; z += 0.5f){
            points.push_back(Vector3(0.0f, 2.0f - (z + 15.0f) * 0.1f, z));
        }
        return fieldMakeStations(points);
    }

    float fieldDescendingChannel(float x, float){
        return std::abs(x) <= 3.0f ? -5.0f : 10.0f;
    }

}

// ---- The basic shape: a trench fills to its walls and stops there ----

TEST(SplineFillFieldTests, AStraightTrenchFillsToItsWallsAndNoFurther){
    // flat bed out to |x| = 2, then walls climbing 10 m per metre
    FieldTestGround ground(81u, 81u, 0.5f, -20.0f, -20.0f, [](float x, float){
        const float wall = std::abs(x) - 2.0f;
        return wall <= 0.0f ? 0.0f : wall * 10.0f;
    });

    const std::vector<SplineStation> stations = fieldStraightRun(0.5f, -15.0f, 15.0f, 0.5f);
    SplineFillParams params;
    params.maxHalfWidth = 12.0f;
    params.edgeDrop = 0.1f;

    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);
    ASSERT_GT(field.diagnostics.wetCorners, 0u);
    EXPECT_EQ(field.diagnostics.drySeeds, 0u);

    // the contour is at phi = -edgeDrop: 0.6 = (|x| - 2) * 10 puts the wall crossing at |x| = 2.06
    for (uint32_t cz = 0; cz < ground.grid.countZ; cz++){
        for (uint32_t cx = 0; cx < ground.grid.countX; cx++){
            if (!fieldAt(field, cx, cz).wet){
                continue;
            }
            EXPECT_LE(std::abs(ground.grid.worldX(cx)), 2.06f + 1.0e-3f)
                << "water climbed the wall at (" << ground.grid.worldX(cx) << ", " << ground.grid.worldZ(cz) << ")";
        }
    }
    EXPECT_TRUE(fieldAt(field, 40u, 40u).wet) << "the trench centre must fill";
}

TEST(SplineFillFieldTests, TheRegionBoundaryIsTheMinusEdgeDropIso){
    // a ramp rising with x, closed downhill by a wall at x = -6 so the water has something to stand
    // behind
    FieldTestGround ground(81u, 21u, 0.5f, -20.0f, -5.0f, [](float x, float){ return x <= -6.0f ? 10.0f : x; });

    const std::vector<SplineStation> stations = fieldStraightRun(0.0f, -3.0f, 3.0f, 0.5f);
    SplineFillParams params;
    params.maxHalfWidth = 12.0f;
    params.edgeDrop = 0.25f;

    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

    // waterline 0, ground = x, so phi = -x: the iso phi = -edgeDrop sits at x = +0.25
    for (uint32_t cx = 0; cx < ground.grid.countX; cx++){
        const float x = ground.grid.worldX(cx);
        if (x > -6.0f && x < 0.2f){
            EXPECT_TRUE(fieldAt(field, cx, 10u).wet) << "x = " << x;
        }
        if (x > 0.3f){
            EXPECT_FALSE(fieldAt(field, cx, 10u).wet) << "x = " << x;
        }
    }
}

// a flat waterline over ground that keeps falling has nothing holding it up: the region ends on
// the ground rather than at the corridor
TEST(SplineFillFieldTests, AWaterlineOverGroundThatKeepsFallingStopsShortOfTheCorridor){
    FieldTestGround ground(81u, 21u, 0.5f, -20.0f, -5.0f, [](float x, float){ return x * 1.0f; });

    const std::vector<SplineStation> stations = fieldStraightRun(0.0f, -3.0f, 3.0f, 0.5f);
    SplineFillParams params;
    params.maxHalfWidth = 12.0f;
    params.edgeDrop = 0.25f;

    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

    for (uint32_t cz = 0; cz < ground.grid.countZ; cz++){
        for (uint32_t cx = 0; cx < ground.grid.countX; cx++){
            const SplineFillCorner& corner = fieldAt(field, cx, cz);
            if (corner.wet){
                EXPECT_LT(corner.distance, params.maxHalfWidth - 1.0f)
                    << "water reached the corridor over open falling ground at x = " << ground.grid.worldX(cx);
            }
        }
    }
    EXPECT_GT(field.diagnostics.overBankCorners, 0u) << "the ramp gave the water nothing to stand behind, yet nothing was refused";
}

// ---- Defect class 2: the pocket inside a bend no perpendicular ray reaches ----

TEST(SplineFillFieldTests, AConcaveBendFillsThePocketNoPerpendicularRayCanReach){
    // an L-shaped trench down +Z to the origin then along +X, plus a lobe of bed off the bend where
    // the perpendicular march is blind (x < 0, z > 0)
    FieldTestGround ground(81u, 81u, 0.5f, -20.0f, -20.0f, [](float x, float z){
        const bool inLegZ = std::abs(x) <= 2.0f && z <= 0.0f;
        const bool inLegX = std::abs(z) <= 2.0f && x >= 0.0f;
        const bool lobe = x >= -4.0f && x <= 0.0f && z >= 0.0f && z <= 4.0f;
        return (inLegZ || inLegX || lobe) ? 0.0f : 10.0f;
    });

    std::vector<Vector3> points;
    for (float z = -15.0f; z <= 0.0f; z += 0.5f){
        points.push_back(Vector3(0.0f, 0.5f, z));
    }
    for (float x = 0.5f; x <= 15.0f; x += 0.5f){
        points.push_back(Vector3(x, 0.5f, 0.0f));
    }
    const std::vector<SplineStation> stations = fieldMakeStations(points);

    SplineFillParams params;
    params.maxHalfWidth = 12.0f;
    params.edgeDrop = 0.1f;
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

    const uint32_t pocketX = 34u; // x = -3
    const uint32_t pocketZ = 44u; // z = +2
    ASSERT_NEAR(ground.grid.worldX(pocketX), -3.0f, 1.0e-4f);
    ASSERT_NEAR(ground.grid.worldZ(pocketZ), 2.0f, 1.0e-4f);
    ASSERT_EQ(ground.heights[ground.grid.index(pocketX, pocketZ)], 0.0f) << "the pocket must be bed";

    // genuinely off every station's perpendicular
    float closestAlong = 1.0e9f;
    for (const SplineStation& s : stations){
        const float dx = ground.grid.worldX(pocketX) - s.position.x;
        const float dz = ground.grid.worldZ(pocketZ) - s.position.z;
        closestAlong = std::min(closestAlong, std::abs(dx * s.forward.x + dz * s.forward.z));
    }
    EXPECT_GT(closestAlong, 0.2f) << "the pocket lies on some station's perpendicular";
    EXPECT_LT(fieldAt(field, pocketX, pocketZ).distance, params.maxHalfWidth);
    EXPECT_TRUE(fieldAt(field, pocketX, pocketZ).wet) << "the fill must reach the pocket the perpendicular march shadows past";
}

// ---- Defect class 3: the detached tongue over a ridge ----

TEST(SplineFillFieldTests, ABasinBehindARidgeStaysDryEvenThoughItIsBelowTheWaterline){
    FieldTestGround ground(81u, 41u, 0.5f, -10.0f, -10.0f, [](float x, float){
        if (std::abs(x) <= 2.0f){
            return 0.0f;  // the channel
        }
        if (x > 4.0f && x < 9.0f){
            return -1.0f; // the detached basin, BELOW the waterline
        }
        return 5.0f;      // banks and the ridge between them
    });

    const std::vector<SplineStation> stations = fieldStraightRun(0.5f, -8.0f, 8.0f, 0.5f);
    SplineFillParams params;
    params.maxHalfWidth = 12.0f;
    params.edgeDrop = 0.1f;

    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

    const uint32_t basinX = 32u; // x = 6
    ASSERT_NEAR(ground.grid.worldX(basinX), 6.0f, 1.0e-4f);
    const SplineFillCorner& basin = fieldAt(field, basinX, 20u);
    // a threshold would take it: below the waterline and inside the reach
    EXPECT_GT(basin.inside, 0.0f) << "the basin must pass the per-corner test";
    // the flood does not, because the ridge between is dry
    EXPECT_FALSE(basin.wet);
    EXPECT_EQ(fieldCountWetComponents(field, ground.grid), 1u);
}

// ---- Defect class 3, second costume: the hairpin that became a lake ----

TEST(SplineFillFieldTests, AHairpinOverOneBasinYieldsOneRegionWithAnHonestStep){
    FieldTestGround ground(81u, 81u, 0.5f, -20.0f, -20.0f, [](float x, float z){
        return (std::abs(x) <= 8.0f && std::abs(z) <= 8.0f) ? -1.0f : 10.0f;
    });

    std::vector<Vector3> points;
    for (float z = -7.0f; z <= 6.0f; z += 0.5f){
        points.push_back(Vector3(-3.0f, 0.5f, z));
    }
    for (float x = -2.5f; x <= 3.0f; x += 0.5f){
        points.push_back(Vector3(x, 0.5f, 6.5f));
    }
    for (float z = 6.0f; z >= -7.0f; z -= 0.5f){
        points.push_back(Vector3(3.0f, 0.5f, z));
    }
    // drop the whole run 1 m from source to mouth
    for (size_t i = 0; i < points.size(); i++){
        points[i].y -= 1.0f * static_cast<float>(i) / static_cast<float>(points.size() - 1u);
    }
    const std::vector<SplineStation> stations = fieldMakeStations(points);

    SplineFillParams params;
    params.maxHalfWidth = 12.0f;
    params.edgeDrop = 0.1f;
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

    ASSERT_GT(field.diagnostics.wetCorners, 0u);
    // ONE region, visited once: no second surface to z-fight
    EXPECT_EQ(fieldCountWetComponents(field, ground.grid), 1u);
    // the step down the middle is REPORTED rather than flattened away
    EXPECT_GT(field.diagnostics.medialStepMetres, 0.1f);
    EXPECT_LT(field.diagnostics.medialStepMetres, 1.01f) << "no step can exceed the run's own total drop";
}

// ---- 4-connectivity, seeds, spill, the sea floor and the budget ----

TEST(SplineFillFieldTests, WaterDoesNotLeakThroughAOneCellDiagonalGap){
    // two basins touching only at a diagonal; the upper one CLOSED, so connectivity alone keeps it dry
    FieldTestGround ground(21u, 21u, 1.0f, -10.0f, -10.0f, [](float x, float z){
        const bool lower = x <= 0.0f && z <= 0.0f;
        const bool upper = x >= 1.0f && x <= 6.0f && z >= 1.0f && z <= 6.0f;
        return (lower || upper) ? 0.0f : 10.0f;
    });

    std::vector<Vector3> points;
    for (float z = -6.0f; z <= 0.0f; z += 1.0f){
        points.push_back(Vector3(-3.0f, 0.5f, z));
    }
    const std::vector<SplineStation> stations = fieldMakeStations(points);

    SplineFillParams params;
    params.maxHalfWidth = 12.0f;
    params.edgeDrop = 0.1f;
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

    EXPECT_GT(fieldAt(field, 15u, 15u).inside, 0.0f);
    EXPECT_FALSE(fieldAt(field, 15u, 15u).wet) << "an 8-connected flood leaks here";
    EXPECT_EQ(fieldCountWetComponents(field, ground.grid), 1u);
}

TEST(SplineFillFieldTests, ASeedAboveItsOwnBedIsReportedRatherThanFilled){
    FieldTestGround ground(41u, 41u, 0.5f, -10.0f, -10.0f, [](float, float){ return 5.0f; });

    const std::vector<SplineStation> stations = fieldStraightRun(0.5f, -5.0f, 5.0f, 0.5f);
    SplineFillParams params;
    params.maxHalfWidth = 12.0f;
    params.edgeDrop = 0.1f;
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

    EXPECT_EQ(field.diagnostics.wetCorners, 0u);
    EXPECT_GT(field.diagnostics.drySeeds, 0u);
    EXPECT_EQ(field.diagnostics.drySeeds, field.diagnostics.seeds) << "every seed is dry on ground this high";
    EXPECT_NEAR(field.diagnostics.firstDrySeed.y, 0.5f, 1.0e-4f);
    // the first dry seed names its station, whose splineT names the knot
    EXPECT_EQ(field.diagnostics.firstDrySeedStation, 0u);
}

TEST(SplineFillFieldTests, WaterWithNoBankInsideTheReachIsRefusedAndNamed){
    // flat ground far wider than the reach: nothing holds this water
    FieldTestGround ground(81u, 81u, 0.5f, -20.0f, -20.0f, [](float, float){ return -1.0f; });

    const std::vector<SplineStation> stations = fieldStraightRun(0.5f, -10.0f, 10.0f, 0.5f);
    SplineFillParams params;
    params.maxHalfWidth = 4.0f;
    params.edgeDrop = 0.1f;
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

    EXPECT_EQ(field.diagnostics.wetCorners, 0u);
    EXPECT_EQ(field.diagnostics.drySeeds, 0u) << "the bed IS below the waterline; that is not why";
    EXPECT_GT(field.diagnostics.overBankCorners, 0u);
    // every refusal is for want of a bank inside the reach
    EXPECT_EQ(field.diagnostics.unseenBankCorners, field.diagnostics.overBankCorners);
}

TEST(SplineFillFieldTests, TheSameRunWithBanksInsideTheReachFillsAndNamesNothing){
    FieldTestGround ground(81u, 81u, 0.5f, -20.0f, -20.0f, [](float x, float z){
        return std::abs(x) <= 2.0f && std::abs(z) <= 14.0f ? -1.0f : 10.0f;
    });

    const std::vector<SplineStation> stations = fieldStraightRun(0.5f, -10.0f, 10.0f, 0.5f);
    SplineFillParams params;
    params.maxHalfWidth = 12.0f;
    params.edgeDrop = 0.1f;
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

    ASSERT_GT(field.diagnostics.wetCorners, 0u);
    EXPECT_EQ(field.diagnostics.overBankCorners, 0u);
    EXPECT_EQ(field.diagnostics.unseenBankCorners, 0u);
}

// A refusal's WHERE: two leaks, each confined to one lattice row. At z = -4 the ground past the
// bank drops to 0.8 m out beyond the corridor (refused by 0.2 m, UNSEEN); at z = +4 the bank crests
// at 0.6 m one corner inside the corridor (refused by 0.4 m, SEEN). The unseen row comes first in
// lattice order with the smaller overshoot, so "first found" would name the wrong row.
TEST(SplineFillFieldTests, TheWorstRefusalOfEachKindNamesItsOwnCorner){
    constexpr float waterline = 1.0f;
    constexpr float bedHalfWidth = 3.0f;
    constexpr float bedHalfLength = 13.0f;
    constexpr float bankTop = 5.0f;
    constexpr float spacing = 0.5f;
    constexpr float originZ = -15.0f;
    constexpr float unseenRowZ = -4.0f;
    constexpr float seenRowZ = 4.0f;
    constexpr float runStartZ = -12.0f;
    constexpr float unseenLeak = 0.8f;
    constexpr float crestX = 3.5f;
    constexpr float seenCrest = 0.6f;
    constexpr float pastCrest = 0.2f;
    constexpr float reach = 4.0f;

    const auto sameRow = [](float a, float b){ return std::abs(a - b) < spacing * 0.5f; };
    FieldTestGround ground(41u, 61u, spacing, -10.0f, originZ, [&](float x, float z){
        if (x > bedHalfWidth){
            if (sameRow(z, unseenRowZ)){
                return unseenLeak;
            }
            if (sameRow(z, seenRowZ)){
                return sameRow(x, crestX) ? seenCrest : pastCrest;
            }
        }
        return std::abs(x) <= bedHalfWidth && std::abs(z) <= bedHalfLength ? 0.0f : bankTop;
    });

    const std::vector<SplineStation> stations = fieldStraightRun(waterline, runStartZ, -runStartZ, spacing);
    SplineFillParams params;
    params.maxHalfWidth = reach;
    params.edgeDrop = 0.1f;
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);
    const SplineFillDiagnostics& d = field.diagnostics;

    ASSERT_GT(d.wetCorners, 0u);
    // eight corners per row (x = 0 .. 3.5)
    EXPECT_EQ(d.overBankCorners, 16u);
    EXPECT_EQ(d.unseenBankCorners, 8u);
    EXPECT_NEAR(d.maxOverBankMetres, waterline - seenCrest, 1.0e-5f);

    EXPECT_FLOAT_EQ(d.worstOverBank.x, 0.0f);
    EXPECT_FLOAT_EQ(d.worstOverBank.z, seenRowZ);
    EXPECT_FLOAT_EQ(d.worstOverBank.arcDistance, seenRowZ - runStartZ);

    EXPECT_FLOAT_EQ(d.worstUnseenBank.x, 0.0f);
    EXPECT_FLOAT_EQ(d.worstUnseenBank.z, unseenRowZ);
    EXPECT_FLOAT_EQ(d.worstUnseenBank.arcDistance, unseenRowZ - runStartZ);

    // read the field back AT the reported coordinates
    const auto cornerAt = [&](const SplineFillWorstSample& sample) -> const SplineFillCorner&{
        const uint32_t cx = static_cast<uint32_t>(std::lround((sample.x - ground.grid.originX) / ground.grid.spacingX));
        const uint32_t cz = static_cast<uint32_t>(std::lround((sample.z - ground.grid.originZ) / ground.grid.spacingZ));
        return fieldAt(field, cx, cz);
    };

    const SplineFillCorner& overBank = cornerAt(d.worstOverBank);
    EXPECT_EQ(overBank.wet, 0u);
    EXPECT_NE(overBank.station, SPLINE_FILL_NO_STATION);
    EXPECT_FLOAT_EQ(overBank.arcDistance, d.worstOverBank.arcDistance);
    EXPECT_NEAR(overBank.waterline - overBank.escape, d.maxOverBankMetres, 1.0e-5f);
    EXPECT_EQ(overBank.escapeUnseen, 0u) << "the deeper refusal is the one the run CAN see";

    const SplineFillCorner& unseen = cornerAt(d.worstUnseenBank);
    EXPECT_EQ(unseen.wet, 0u);
    EXPECT_NE(unseen.station, SPLINE_FILL_NO_STATION);
    EXPECT_FLOAT_EQ(unseen.arcDistance, d.worstUnseenBank.arcDistance);
    EXPECT_NE(unseen.escapeUnseen, 0u) << "the unseen-bank warning is pointing at a bank the run could see";
    EXPECT_NEAR(unseen.waterline - unseen.escape, waterline - unseenLeak, 1.0e-5f);
}

TEST(SplineFillFieldTests, TheSeaLevelFloorLiftsTheWaterlineAndNeverLowersIt){
    FieldTestGround ground(41u, 81u, 0.5f, -10.0f, -20.0f, fieldDescendingChannel);
    const std::vector<SplineStation> stations = fieldDescendingRun();

    SplineFillParams unclamped;
    unclamped.maxHalfWidth = 12.0f;
    unclamped.edgeDrop = 0.1f;
    const SplineFillResult without = buildSplineFillField(stations, ground.grid, unclamped);

    SplineFillParams clamped = unclamped;
    clamped.seaLevelFloor = 0.5f;
    const SplineFillResult with = buildSplineFillField(stations, ground.grid, clamped);

    ASSERT_EQ(with.corners.size(), without.corners.size());
    bool anyLifted = false;
    for (size_t i = 0; i < with.corners.size(); i++){
        if (with.corners[i].station == SPLINE_FILL_NO_STATION){
            continue;
        }
        EXPECT_GE(with.corners[i].waterline, 0.5f - 1.0e-4f) << "the floor must hold";
        EXPECT_GE(with.corners[i].waterline, without.corners[i].waterline - 1.0e-4f) << "a floor may only raise the waterline";
        anyLifted = anyLifted || with.corners[i].waterline > without.corners[i].waterline + 1.0e-3f;
    }
    EXPECT_TRUE(anyLifted) << "this run drops below the floor, so something must have been lifted";
}

TEST(SplineFillFieldTests, TheDefaultSeaLevelFloorChangesNothing){
    FieldTestGround ground(41u, 81u, 0.5f, -10.0f, -20.0f, fieldDescendingChannel);
    const std::vector<SplineStation> stations = fieldDescendingRun();

    SplineFillParams params;
    params.maxHalfWidth = 12.0f;
    const SplineFillResult defaulted = buildSplineFillField(stations, ground.grid, params);

    SplineFillParams explicitly = params;
    explicitly.seaLevelFloor = SPLINE_NO_SEA_LEVEL_FLOOR;
    const SplineFillResult same = buildSplineFillField(stations, ground.grid, explicitly);

    ASSERT_EQ(defaulted.corners.size(), same.corners.size());
    for (size_t i = 0; i < same.corners.size(); i++){
        EXPECT_EQ(defaulted.corners[i].waterline, same.corners[i].waterline);
    }
    EXPECT_EQ(defaulted.diagnostics.wetCorners, same.diagnostics.wetCorners);
}

TEST(SplineFillFieldTests, ARegionOverTheBudgetBuildsNothingAndSaysSo){
    // a closed basin the run sits in, so the budget is what stops it
    FieldTestGround ground(81u, 81u, 0.5f, -20.0f, -20.0f, [](float x, float z){
        return std::abs(x) <= 8.0f && std::abs(z) <= 14.0f ? -1.0f : 10.0f;
    });

    const std::vector<SplineStation> stations = fieldStraightRun(0.5f, -10.0f, 10.0f, 0.5f);
    SplineFillParams params;
    params.maxHalfWidth = 12.0f;
    params.maxWetCorners = 64u;

    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);
    EXPECT_TRUE(field.diagnostics.exceededBudget);
    EXPECT_EQ(field.diagnostics.wetCorners, 0u);
    EXPECT_FALSE(field.hasRegion()) << "over budget must build NOTHING, not a coarser surface";
    for (const SplineFillCorner& corner : field.corners){
        EXPECT_EQ(corner.wet, 0u);
    }
}

TEST(SplineFillFieldTests, AWaterlineThatRisesDownstreamIsReportedRatherThanClamped){
    FieldTestGround ground(41u, 81u, 0.5f, -10.0f, -20.0f, [](float, float){ return -1.0f; });

    std::vector<Vector3> points;
    for (float z = -10.0f; z <= 10.0f; z += 0.5f){
        points.push_back(Vector3(0.0f, z > 0.0f ? 1.0f : 0.0f, z)); // a 1 m step UP mid-run
    }
    const std::vector<SplineStation> stations = fieldMakeStations(points);

    SplineFillParams params;
    params.maxHalfWidth = 6.0f;
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

    EXPECT_GT(field.diagnostics.waterlineRiseMetres, SPLINE_WATERLINE_RISE_TOLERANCE_METRES);
    // not clamped: the water the author placed is still where they placed it
    EXPECT_NEAR(field.diagnostics.waterlineRiseMetres, 1.0f, 1.0e-4f);
    // and named: the first station past the step, at its distance along the run
    ASSERT_NE(field.diagnostics.waterlineRiseStation, SPLINE_FILL_NO_STATION);
    EXPECT_GT(stations[field.diagnostics.waterlineRiseStation].position.z, 0.0f);
    EXPECT_LE(stations[field.diagnostics.waterlineRiseStation - 1u].position.z, 0.0f);
    EXPECT_FLOAT_EQ(field.diagnostics.waterlineRiseAtArc, stations[field.diagnostics.waterlineRiseStation].distance);
}

// ---- The invariant the mesher depends on ----

TEST(SplineFillFieldTests, EveryBoundaryEdgeBracketsTheIso){
    // two 4-adjacent corners both inside are in the SAME component, so a wet corner's dry
    // 4-neighbour is never positive and the mesher's crossing always has a bracket
    FieldTestGround ground(81u, 81u, 0.5f, -20.0f, -20.0f, [](float x, float z){
        const float bed = std::sin(z * 0.35f) * 3.0f;
        const float across = std::abs(x - bed);
        if (across <= 2.5f){
            return -0.5f;
        }
        if (across > 6.0f && across < 8.0f){
            return -0.8f; // detached pockets, below the waterline
        }
        return 4.0f;
    });

    std::vector<Vector3> points;
    for (float z = -15.0f; z <= 15.0f; z += 0.5f){
        points.push_back(Vector3(std::sin(z * 0.35f) * 3.0f, 0.2f, z));
    }
    const std::vector<SplineStation> stations = fieldMakeStations(points);

    SplineFillParams params;
    params.maxHalfWidth = 9.0f;
    params.edgeDrop = 0.15f;
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);
    ASSERT_GT(field.diagnostics.wetCorners, 0u);

    uint32_t boundaryEdges = 0;
    const auto check = [&](const SplineFillCorner& a, const SplineFillCorner& b){
        if ((a.wet != 0u) == (b.wet != 0u)){
            return;
        }
        boundaryEdges++;
        const SplineFillCorner& in = a.wet ? a : b;
        const SplineFillCorner& out = a.wet ? b : a;
        EXPECT_GT(in.inside, 0.0f);
        EXPECT_LE(out.inside, 0.0f) << "a dry 4-neighbour of a wet corner must not be inside";
    };
    for (uint32_t cz = 0; cz < ground.grid.countZ; cz++){
        for (uint32_t cx = 0; cx < ground.grid.countX; cx++){
            if (cx + 1u < ground.grid.countX){
                check(fieldAt(field, cx, cz), fieldAt(field, cx + 1u, cz));
            }
            if (cz + 1u < ground.grid.countZ){
                check(fieldAt(field, cx, cz), fieldAt(field, cx, cz + 1u));
            }
        }
    }
    EXPECT_GT(boundaryEdges, 50u) << "this scene must actually have a boundary to check";
}

// no dry corner below the water standing around it, ringed by that water on every side
TEST(SplineFillFieldTests, WaterThatEnclosesDryGroundBelowItsOwnSurfaceLeavesNoPocket){
    FieldTestGround ground(81u, 101u, 0.5f, -20.0f, -20.0f, [](float x, float z){
        const float bed = -4.0f;
        if (x <= -3.0f && x >= -9.0f && z <= 1.0f){
            return bed; // left leg
        }
        if (x >= 3.0f && x <= 9.0f && z <= 1.0f){
            return bed; // right leg
        }
        const float r = std::sqrt(x * x + z * z);
        if (z >= 0.0f && r >= 3.0f && r <= 9.0f){
            return bed; // the bend
        }
        if (x > -3.0f && x < 3.0f && z > -14.0f && z < 0.0f){
            return 1.2f; // the inner bank between the legs, with a low crest
        }
        return 12.0f;
    });

    // waterline ABOVE the inner bank's crest
    const std::vector<SplineStation> stations = fieldHairpinRun(2.5f);

    SplineFillParams params;
    params.maxHalfWidth = 14.0f;
    params.edgeDrop = 0.1f;
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

    ASSERT_GT(field.diagnostics.wetCorners, 0u) << "the run must actually hold water to test";
    EXPECT_EQ(fieldCountEnclosedDryBelowWaterline(field, ground.grid), 0u) << "a dry corner the water surrounds has nowhere to drain";
}

// ground standing ABOVE the water around it is an island, and sealing must not drown it
TEST(SplineFillFieldTests, AnIsletStandingAboveTheWaterlineKeepsItsDryTop){
    FieldTestGround ground(41u, 81u, 0.5f, -10.0f, -20.0f, [](float x, float z){
        if (std::abs(x) > 4.0f){
            return 12.0f;
        }
        return std::sqrt(x * x + z * z) <= 2.0f ? 4.0f : -4.0f; // rock top at 4 m, bed at -4 m
    });
    const std::vector<SplineStation> stations = fieldStraightRun(0.0f, -15.0f, 15.0f, 0.5f);

    SplineFillParams params;
    params.maxHalfWidth = 8.0f;
    params.edgeDrop = 0.1f;
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

    EXPECT_EQ(fieldAt(field, 20u, 40u).wet, 0u) << "ground above the waterline is an island, not a pocket";
    EXPECT_EQ(field.diagnostics.sealedPocketCorners, 0u);
    EXPECT_EQ(fieldCountEnclosedDryBelowWaterline(field, ground.grid), 0u);
}

// with the floor set to the sea's own altitude the mouth would be EXACTLY coplanar with the sea
TEST(SplineFillFieldTests, TheSeaLevelFloorStandsClearOfTheLevelItNames){
    FieldTestGround ground(41u, 81u, 0.5f, -10.0f, -20.0f, fieldDescendingChannel);
    const std::vector<SplineStation> stations = fieldDescendingRun();

    SplineFillParams params;
    params.maxHalfWidth = 12.0f;
    params.seaLevelFloor = 3.0f;
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

    bool anyOnTheFloor = false;
    for (const SplineFillCorner& corner : field.corners){
        if (corner.station == SPLINE_FILL_NO_STATION){
            continue;
        }
        EXPECT_GT(corner.waterline, 3.0f) << "water sitting EXACTLY on the sea plane z-fights with it";
        anyOnTheFloor = anyOnTheFloor || corner.waterline < 3.0f + SPLINE_SEA_LEVEL_FLOOR_LIFT_METRES * 2.0f;
    }
    EXPECT_TRUE(anyOnTheFloor) << "this run drops below the floor, so the floor must bind";
    EXPECT_LT(SPLINE_SEA_LEVEL_FLOOR_LIFT_METRES, 0.05f) << "a lift big enough to see is too big";
}

// the disabled floor survives the lift with no branch guarding it
TEST(SplineFillFieldTests, TheLiftCannotDisturbTheDisabledFloor){
    EXPECT_EQ(SPLINE_NO_SEA_LEVEL_FLOOR + SPLINE_SEA_LEVEL_FLOOR_LIFT_METRES, SPLINE_NO_SEA_LEVEL_FLOOR);
}

// ---- This port: right of travel, the corridor block, the budget ----

// the signed lateral distance is positive on the right of travel, forward x up: right of +Z is -X
TEST(SplineFillFieldTests, TheSignedLateralDistanceIsPositiveOnTheRightOfTravel){
    FieldTestGround ground(41u, 41u, 0.5f, -10.0f, -10.0f, [](float x, float){ return std::abs(x) <= 3.0f ? -1.0f : 5.0f; });
    const std::vector<SplineStation> stations = fieldStraightRun(0.0f, -6.0f, 6.0f, 0.5f);
    SplineFillParams params;
    params.maxHalfWidth = 6.0f;
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);
    const SplineFillCorner& right = fieldAt(field, 18u, 20u); // x = -1
    const SplineFillCorner& left = fieldAt(field, 22u, 20u);  // x = +1
    ASSERT_NE(right.station, SPLINE_FILL_NO_STATION);
    EXPECT_NEAR(right.lateralSigned, 1.0f, 1.0e-5f);
    EXPECT_NEAR(left.lateralSigned, -1.0f, 1.0e-5f);
    // arc runs along the flow: z + 6 at the centre row
    EXPECT_NEAR(right.arcDistance, 6.0f, 1.0e-4f);
}

// The field covers only the block the corridor can reach, and that block gives every corner what
// the whole grid would: the same scene inside a terrain four times wider (more ground around it,
// none of it reachable) settles on the same field and the same diagnostics. Corners inside the
// corridor are compared to the bit; past it only their dryness counts (a corner on the stamp
// disc's rim may or may not be visited as the lattice coordinates round).
TEST(SplineFillFieldTests, TheFieldCoversOnlyTheCorridorAndMatchesALargerTerrain){
    const auto valley = [](float x, float z){
        const float bed = std::sin(z * 0.35f) * 3.0f;
        const float across = std::abs(x - bed);
        if (across <= 2.5f){
            return -0.5f;
        }
        if (across > 6.0f && across < 8.0f){
            return -0.8f;
        }
        return 0.1f + 0.05f * across;
    };
    std::vector<Vector3> points;
    for (float z = -15.0f; z <= 15.0f; z += 0.5f){
        points.push_back(Vector3(std::sin(z * 0.35f) * 3.0f, 0.2f, z));
    }
    const std::vector<SplineStation> stations = fieldMakeStations(points);
    SplineFillParams params;
    params.maxHalfWidth = 9.0f;
    params.edgeDrop = 0.15f;

    // corner (cx + 360, cz + 360) of the large grid stands where (cx, cz) of the small one does
    FieldTestGround small(241u, 241u, 0.25f, -30.0f, -30.0f, valley);
    FieldTestGround large(961u, 961u, 0.25f, -120.0f, -120.0f, valley);
    const SplineFillResult a = buildSplineFillField(stations, small.grid, params);
    const SplineFillResult b = buildSplineFillField(stations, large.grid, params);
    ASSERT_GT(a.diagnostics.wetCorners, 0u);

    // the block is the corridor's bounds, not the terrain
    EXPECT_LT(static_cast<size_t>(b.countX) * b.countZ, static_cast<size_t>(large.grid.cornerCount()) / 20u);
    EXPECT_GT(a.minCornerX, 0u);
    EXPECT_GT(a.minCornerZ, 0u);
    EXPECT_LT(a.minCornerX + a.countX, small.grid.countX);
    EXPECT_LT(a.minCornerZ + a.countZ, small.grid.countZ);
    uint32_t compared = 0;
    for (uint32_t cz = 0; cz < small.grid.countZ; cz++){
        for (uint32_t cx = 0; cx < small.grid.countX; cx++){
            const SplineFillCorner& p = a.corner(cx, cz);
            const SplineFillCorner& q = b.corner(cx + 360u, cz + 360u);
            EXPECT_EQ(p.wet, q.wet) << cx << ", " << cz;
            if (!(p.distance <= params.maxHalfWidth)){
                continue;
            }
            compared++;
            EXPECT_EQ(p.station, q.station) << cx << ", " << cz;
            EXPECT_TRUE(bitsEqual(p.distance, q.distance)) << cx << ", " << cz;
            EXPECT_TRUE(bitsEqual(p.inside, q.inside)) << cx << ", " << cz;
            EXPECT_TRUE(bitsEqual(p.waterline, q.waterline)) << cx << ", " << cz;
            EXPECT_TRUE(bitsEqual(p.escape, q.escape)) << cx << ", " << cz;
            EXPECT_EQ(p.escapeUnseen, q.escapeUnseen) << cx << ", " << cz;
        }
    }
    EXPECT_GT(compared, 1000u);
    EXPECT_EQ(a.diagnostics.wetCorners, b.diagnostics.wetCorners);
    EXPECT_EQ(a.diagnostics.overBankCorners, b.diagnostics.overBankCorners);
    EXPECT_EQ(a.diagnostics.unseenBankCorners, b.diagnostics.unseenBankCorners);
    EXPECT_EQ(a.diagnostics.sealedPocketCorners, b.diagnostics.sealedPocketCorners);
    EXPECT_EQ(a.diagnostics.maxDepthMetres, b.diagnostics.maxDepthMetres);
    EXPECT_EQ(a.diagnostics.medialStepMetres, b.diagnostics.medialStepMetres);

    // off the corridor the field is the unstamped dry corner
    EXPECT_FALSE(b.contains(0u, 0u));
    const SplineFillCorner& far = b.corner(0u, 0u);
    EXPECT_EQ(far.station, SPLINE_FILL_NO_STATION);
    EXPECT_EQ(far.wet, 0u);
}

// R19: a 300 m river on a 1025^2 terrain fills within 25 ms (enforced on the benchmark server)
TEST(SplineFillFieldTests, AThreeHundredMetreRiverOnA1025GridFillsWithinItsBudget){
    const auto riverBed = [](float x, float z){
        const float bed = 20.0f * std::sin(z * 0.02f);
        const float across = std::abs(x - bed);
        return across <= 6.0f ? -1.0f - 0.01f * z : 0.5f * (across - 6.0f) - 0.01f * z;
    };
    FieldTestGround ground(1025u, 1025u, 0.5f, -256.0f, -256.0f, riverBed);
    std::vector<Vector3> points;
    for (float z = -150.0f; z <= 150.0f; z += 1.0f){
        points.push_back(Vector3(20.0f * std::sin(z * 0.02f), 0.5f - 0.01f * z, z));
    }
    const std::vector<SplineStation> stations = fieldMakeStations(points);
    SplineFillParams params;
    params.maxHalfWidth = 12.0f;

    (void)buildSplineFillField(stations, ground.grid, params);
    const auto start = std::chrono::steady_clock::now();
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("[bench] FitToBanks 300 m river on 1025^2 %.3f ms (budget 25 ms)\n", ms);
    EXPECT_GT(field.diagnostics.wetCorners, 10000u);
    EXPECT_EQ(field.diagnostics.drySeeds, 0u);
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(ms, 25.0 * testsupport::perfSlack());
    }
}
