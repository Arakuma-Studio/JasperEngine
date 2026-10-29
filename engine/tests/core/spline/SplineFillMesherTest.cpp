// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Tests/SplineFillMesherTests.cpp, Copyright
// (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

// Mesh invariants of the region fill (S18 R7, AC6), checked against real output: a closed boundary,
// no T-junctions, no degenerate triangles, upward winding (cross(b - a, c - a) along the normal),
// a shoreline that moves continuously, chunk seams that agree bit for bit, uv0 = (arc, depth) and
// the flow colour (D9). Plus a synthetic bowl lake against its analytic disc and the buried rim.

#include "SplineGeometryTestSupport.h"
#include "spline/SplineFillMesher.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::spline;
using namespace jasperengine::spline::geometrytest;

namespace{

    struct MesherTestGround{
        std::vector<float> heights;
        SplineGroundGrid grid;

        MesherTestGround(uint32_t countX, uint32_t countZ, float spacing, float originX, float originZ,
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

    std::vector<SplineStation> mesherMakeStations(const std::vector<Vector3>& points){
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

    std::vector<SplineStation> mesherStraightRun(float waterline, float fromZ, float toZ, float step){
        std::vector<Vector3> points;
        for (float z = fromZ; z <= toZ + 1.0e-4f; z += step){
            points.push_back(Vector3(0.0f, waterline, z));
        }
        return mesherMakeStations(points);
    }

    SplineFillChunkRange mesherWholeGrid(const SplineGroundGrid& grid){
        return SplineFillChunkRange{0u, 0u, grid.countX, grid.countZ};
    }

    // a trench scene with an ordinary, irregular shoreline
    struct TrenchScene{
        MesherTestGround ground;
        std::vector<SplineStation> stations;
        SplineFillParams params;
        SplineFillResult field;

        explicit TrenchScene(float waterline = 0.5f)
            : ground(61u, 61u, 0.5f, -15.0f, -15.0f, [](float x, float z){
                  // a wobbling channel, so the shoreline crosses cells at every angle
                  const float bed = std::sin(z * 0.4f) * 2.0f;
                  const float across = std::abs(x - bed);
                  return across <= 2.0f ? 0.0f : (across - 2.0f) * 6.0f;
              }){
            std::vector<Vector3> points;
            for (float z = -12.0f; z <= 12.0f; z += 0.5f){
                points.push_back(Vector3(std::sin(z * 0.4f) * 2.0f, waterline, z));
            }
            stations = mesherMakeStations(points);
            params.maxHalfWidth = 10.0f;
            params.edgeDrop = 0.1f;
            field = buildSplineFillField(stations, ground.grid, params);
        }
    };

    // A synthetic bowl, h = 0.05 r^2 - 2, holding a lake at y = 0.5 drawn as a small closed ring of
    // stations about its centre. The contour runs where the ground stands edgeDrop above the water,
    // the circle r^2 = (0.5 + edgeDrop + 2) / 0.05.
    struct BowlScene{
        MesherTestGround ground;
        std::vector<SplineStation> stations;
        SplineFillParams params;
        SplineFillResult field;
        float shoreRadius = 0.0f;

        BowlScene()
            : ground(129u, 129u, 0.25f, -16.0f, -16.0f, [](float x, float z){ return 0.05f * (x * x + z * z) - 2.0f; }){
            std::vector<Vector3> points;
            for (int i = 0; i <= 32; i++){
                const float angle = 2.0f * PI * static_cast<float>(i) / 32.0f;
                points.push_back(Vector3(2.0f * std::cos(angle), 0.5f, 2.0f * std::sin(angle)));
            }
            stations = mesherMakeStations(points);
            params.maxHalfWidth = 20.0f;
            params.edgeDrop = 0.1f;
            field = buildSplineFillField(stations, ground.grid, params);
            shoreRadius = std::sqrt((0.5f + params.edgeDrop + 2.0f) / 0.05f);
        }
    };

    using MesherEdgeKey = std::pair<uint32_t, uint32_t>;
    MesherEdgeKey mesherEdge(uint32_t a, uint32_t b){
        return a < b ? MesherEdgeKey{a, b} : MesherEdgeKey{b, a};
    }

    // Every edge is used once (the shoreline) or twice (interior); and no vertex sits ON a boundary
    // edge without being one of its endpoints, which is what a T-junction IS.
    void mesherExpectClosedWithoutTJunctions(const SplineFillMesh& mesh){
        std::map<MesherEdgeKey, uint32_t> edgeUses;
        for (size_t i = 0; i + 2u < mesh.indices.size(); i += 3u){
            edgeUses[mesherEdge(mesh.indices[i], mesh.indices[i + 1u])]++;
            edgeUses[mesherEdge(mesh.indices[i + 1u], mesh.indices[i + 2u])]++;
            edgeUses[mesherEdge(mesh.indices[i + 2u], mesh.indices[i])]++;
        }
        std::vector<uint32_t> boundary;
        for (const auto& [edge, uses] : edgeUses){
            EXPECT_LE(uses, 2u) << "edge (" << edge.first << ", " << edge.second << ") used " << uses << " times";
            if (uses == 1u){
                boundary.push_back(edge.first);
                boundary.push_back(edge.second);
            }
        }
        ASSERT_FALSE(boundary.empty()) << "a filled region must have a shoreline";

        // every boundary vertex closes: it ends exactly two boundary edges
        std::map<uint32_t, uint32_t> boundaryDegree;
        for (const uint32_t v : boundary){
            boundaryDegree[v]++;
        }
        for (const auto& [vertex, degree] : boundaryDegree){
            EXPECT_EQ(degree % 2u, 0u) << "boundary vertex " << vertex << " leaves the shoreline open";
        }

        for (size_t e = 0; e + 1u < boundary.size(); e += 2u){
            const Vector3 a = mesh.vertices[boundary[e]].position;
            const Vector3 b = mesh.vertices[boundary[e + 1u]].position;
            const Vector3 ab(b.x - a.x, 0.0f, b.z - a.z);
            const float lengthSq = ab.x * ab.x + ab.z * ab.z;
            if (lengthSq < 1.0e-12f){
                continue;
            }
            for (uint32_t v = 0; v < mesh.vertices.size(); v++){
                if (v == boundary[e] || v == boundary[e + 1u]){
                    continue;
                }
                const Vector3 p = mesh.vertices[v].position;
                const float t = ((p.x - a.x) * ab.x + (p.z - a.z) * ab.z) / lengthSq;
                if (t <= 0.01f || t >= 0.99f){
                    continue;
                }
                const float dx = p.x - (a.x + ab.x * t);
                const float dz = p.z - (a.z + ab.z * t);
                EXPECT_GT(dx * dx + dz * dz, 1.0e-8f) << "vertex " << v << " lies on a boundary edge it is not an endpoint of";
            }
        }
    }

    // plan-view area, positive for triangles wound up
    double mesherPlanArea(const SplineFillMesh& mesh){
        double area = 0.0;
        for (size_t i = 0; i + 2u < mesh.indices.size(); i += 3u){
            const Vector3& p0 = mesh.vertices[mesh.indices[i]].position;
            const Vector3& p1 = mesh.vertices[mesh.indices[i + 1u]].position;
            const Vector3& p2 = mesh.vertices[mesh.indices[i + 2u]].position;
            area += 0.5 * (static_cast<double>(p1.z - p0.z) * (p2.x - p0.x) - static_cast<double>(p1.x - p0.x) * (p2.z - p0.z));
        }
        return area;
    }

    bool mesherOnLattice(const SplineGroundGrid& grid, const Vector3& p){
        const float gx = (p.x - grid.originX) / grid.spacingX;
        const float gz = (p.z - grid.originZ) / grid.spacingZ;
        return std::abs(gx - std::round(gx)) < 1.0e-4f && std::abs(gz - std::round(gz)) < 1.0e-4f;
    }

}

TEST(SplineFillMesherTests, AFullyWetBlockIsExactlyTwoTrianglesPerCell){
    MesherTestGround ground(11u, 11u, 1.0f, -5.0f, -5.0f, [](float, float){ return -1.0f; });
    const std::vector<SplineStation> stations = mesherStraightRun(0.5f, -4.0f, 4.0f, 0.5f);

    SplineFillParams params;
    params.maxHalfWidth = 100.0f; // no corridor clipping inside this grid
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);
    ASSERT_EQ(field.diagnostics.wetCorners, ground.grid.cornerCount());

    const SplineFillMesh mesh = buildSplineFillMesh(field, ground.grid, stations, mesherWholeGrid(ground.grid), SplineFillMeshParams{});

    const uint32_t cells = (ground.grid.countX - 1u) * (ground.grid.countZ - 1u);
    EXPECT_EQ(mesh.indices.size(), static_cast<size_t>(cells) * 6u);
    // no shoreline anywhere, so exactly the corner lattice
    EXPECT_EQ(mesh.vertices.size(), static_cast<size_t>(ground.grid.cornerCount()));
    expectWellFormed(mesh.vertices, mesh.indices, "fully wet block", false);
}

TEST(SplineFillMesherTests, TheSurfaceIsClosedAndHasNoTJunctions){
    const TrenchScene scene;
    ASSERT_GT(scene.field.diagnostics.wetCorners, 0u);

    const SplineFillMesh mesh = buildSplineFillMesh(scene.field, scene.ground.grid, scene.stations,
                                                    mesherWholeGrid(scene.ground.grid), SplineFillMeshParams{});
    ASSERT_TRUE(mesh.isValid());
    ASSERT_EQ(mesh.indices.size() % 3u, 0u);
    mesherExpectClosedWithoutTJunctions(mesh);
    expectWellFormed(mesh.vertices, mesh.indices, "trench", false);
}

TEST(SplineFillMesherTests, NoTriangleIsDegenerateAndEveryOneFacesUp){
    const TrenchScene scene;
    const SplineFillMesh mesh = buildSplineFillMesh(scene.field, scene.ground.grid, scene.stations,
                                                    mesherWholeGrid(scene.ground.grid), SplineFillMeshParams{});
    ASSERT_TRUE(mesh.isValid());

    for (size_t i = 0; i + 2u < mesh.indices.size(); i += 3u){
        const uint32_t i0 = mesh.indices[i];
        const uint32_t i1 = mesh.indices[i + 1u];
        const uint32_t i2 = mesh.indices[i + 2u];
        ASSERT_TRUE(i0 != i1 && i1 != i2 && i0 != i2) << "repeated index at triangle " << i / 3u;

        const Vector3 p0 = mesh.vertices[i0].position;
        const Vector3 p1 = mesh.vertices[i1].position;
        const Vector3 p2 = mesh.vertices[i2].position;
        // plan-view signed area: positive when cross(p1 - p0, p2 - p0) points up
        const float twiceArea = (p1.z - p0.z) * (p2.x - p0.x) - (p1.x - p0.x) * (p2.z - p0.z);
        EXPECT_GT(twiceArea, 1.0e-9f) << "triangle " << i / 3u << " is degenerate or wound face-down in plan view";
        EXPECT_GT(crossOf(p1 - p0, p2 - p0).y, 0.0f) << "triangle " << i / 3u;
    }
}

TEST(SplineFillMesherTests, TheShorelineMovesContinuouslyAsTheWaterlineIsSwept){
    // a steady ramp closed downhill at x = -6: 1 cm of waterline must move the shoreline about 2 cm,
    // not jump it a whole cell
    const float spacing = 0.5f;
    MesherTestGround ground(41u, 21u, spacing, -10.0f, -5.0f, [](float x, float){ return x <= -6.0f ? 10.0f : x * 0.5f; });

    float previousExtent = 0.0f;
    float largestJump = 0.0f;
    for (int step = 0; step <= 20; step++){
        const float waterline = static_cast<float>(step) * 0.01f;
        const std::vector<SplineStation> stations = mesherStraightRun(waterline, -3.0f, 3.0f, 0.5f);
        SplineFillParams params;
        params.maxHalfWidth = 8.0f;
        params.edgeDrop = 0.1f;
        const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

        const SplineFillMesh mesh = buildSplineFillMesh(field, ground.grid, stations, mesherWholeGrid(ground.grid), SplineFillMeshParams{});
        ASSERT_TRUE(mesh.isValid()) << "waterline " << waterline;

        float extent = -1.0e9f;
        for (const SplineVertex& vertex : mesh.vertices){
            extent = std::max(extent, vertex.position.x);
        }
        if (step > 0){
            largestJump = std::max(largestJump, std::abs(extent - previousExtent));
        }
        previousExtent = extent;
    }

    EXPECT_LT(largestJump, spacing * 0.5f) << "the shoreline jumped by " << largestJump << " m: a staircase, not a contour";
    EXPECT_GT(largestJump, 0.0f) << "the shoreline did not move at all";
}

TEST(SplineFillMesherTests, TheRimSitsEdgeDropBelowTheWaterlineAndInteriorVerticesSitOnIt){
    const TrenchScene scene;
    SplineFillMeshParams meshParams;
    meshParams.edgeDrop = scene.params.edgeDrop;
    const SplineFillMesh mesh = buildSplineFillMesh(scene.field, scene.ground.grid, scene.stations,
                                                    mesherWholeGrid(scene.ground.grid), meshParams);
    ASSERT_TRUE(mesh.isValid());

    uint32_t interior = 0;
    uint32_t rim = 0;
    for (const SplineVertex& vertex : mesh.vertices){
        if (mesherOnLattice(scene.ground.grid, vertex.position)){
            interior++;
            EXPECT_NEAR(vertex.position.y, scene.stations.front().position.y, 1.0e-3f);
        }else{
            rim++;
            EXPECT_NEAR(vertex.position.y, scene.stations.front().position.y - meshParams.edgeDrop, 1.0e-3f);
        }
    }
    EXPECT_GT(interior, 0u);
    EXPECT_GT(rim, 0u);
}

// Bit-identity at a chunk seam: uv0 is a measurement the water shader reads, so a seam that agreed
// about position and disagreed about arc or depth would draw a foam line down a lattice column.
TEST(SplineFillMesherTests, ChunkBoundariesProduceBitIdenticalVerticesFromBothSides){
    const TrenchScene scene;
    const SplineFillChunkRange left{0u, 0u, 31u, scene.ground.grid.countZ};
    const SplineFillChunkRange right{30u, 0u, scene.ground.grid.countX, scene.ground.grid.countZ};

    const SplineFillMesh a = buildSplineFillMesh(scene.field, scene.ground.grid, scene.stations, left, SplineFillMeshParams{});
    const SplineFillMesh b = buildSplineFillMesh(scene.field, scene.ground.grid, scene.stations, right, SplineFillMeshParams{});
    ASSERT_TRUE(a.isValid());
    ASSERT_TRUE(b.isValid());

    const float seamX = scene.ground.grid.worldX(30u);
    const auto onSeam = [&](const SplineFillMesh& mesh){
        std::map<float, SplineVertex> byZ;
        for (const SplineVertex& vertex : mesh.vertices){
            if (vertex.position.x == seamX){
                byZ[vertex.position.z] = vertex;
            }
        }
        return byZ;
    };
    const auto seamA = onSeam(a);
    const auto seamB = onSeam(b);
    ASSERT_FALSE(seamA.empty()) << "the seam column must carry vertices";

    uint32_t compared = 0;
    for (const auto& [z, vertex] : seamA){
        const auto found = seamB.find(z);
        if (found == seamB.end()){
            continue;
        }
        compared++;
        EXPECT_TRUE(vertexBitsEqual(vertex, found->second)) << "the chunk seam disagrees at z = " << z;
    }
    EXPECT_GT(compared, 5u) << "too few shared seam vertices to call this a seam test";
}

// uv0 carries a MEASUREMENT, arc along the flow and still-water depth in metres, over a basin with
// a known waterline and bed so both channels have an exact expected value at every vertex
TEST(SplineFillMesherTests, Uv0IsArcAlongFlowAndStillWaterDepthInMetres){
    // 2 m under the centreline, 0.5 m at the lip, wall beyond it
    const auto bedHeight = [](float x){ return -2.0f + std::abs(x) * 0.5f; };
    MesherTestGround ground(41u, 41u, 0.5f, -10.0f, -10.0f, [&](float x, float){ return std::abs(x) <= 3.0f ? bedHeight(x) : 10.0f; });
    // flat waterline at y = 0 along +Z from z = -8: the arc at any vertex is z + 8
    const std::vector<SplineStation> stations = mesherStraightRun(0.0f, -8.0f, 8.0f, 0.5f);

    SplineFillParams params;
    params.maxHalfWidth = 8.0f;
    params.edgeDrop = 0.1f;
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

    SplineFillMeshParams meshParams;
    meshParams.edgeDrop = params.edgeDrop;
    meshParams.flowSpeedScale = 0.4f;
    const SplineFillMesh mesh = buildSplineFillMesh(field, ground.grid, stations, mesherWholeGrid(ground.grid), meshParams);
    ASSERT_TRUE(mesh.isValid());

    uint32_t surfaceVertices = 0;
    uint32_t shorelineVertices = 0;
    for (const SplineVertex& vertex : mesh.vertices){
        EXPECT_NEAR(vertex.uv.x, vertex.position.z + 8.0f, 1.0e-3f) << "uv0.x must be arc length along the flow";
        if (vertex.position.y > -0.5f * params.edgeDrop){
            surfaceVertices++;
            EXPECT_NEAR(vertex.uv.y, -bedHeight(vertex.position.x), 1.0e-3f) << "uv0.y must be waterline minus bed at x = " << vertex.position.x;
        }else{
            shorelineVertices++;
            // inside is depth + edgeDrop wherever the ground decides the crossing
            EXPECT_NEAR(vertex.uv.y, -params.edgeDrop, 1.0e-3f) << "a ground-decided shoreline vertex reads exactly -edgeDrop";
        }

        // U runs along the flow (+Z here) with the sweep's handedness; the colour is the flow (0, 1)
        // in XZ, encoded 0.5 + 0.5 d, and the speed scale
        EXPECT_NEAR(vertex.normal.y, 1.0f, 1.0e-3f);
        EXPECT_NEAR(vertex.tangent.z, 1.0f, 1.0e-3f);
        EXPECT_FLOAT_EQ(vertex.tangent.w, -1.0f);
        EXPECT_FLOAT_EQ(vertex.color.x, 0.5f);
        EXPECT_FLOAT_EQ(vertex.color.y, 1.0f);
        EXPECT_FLOAT_EQ(vertex.color.z, 0.4f);
        EXPECT_FLOAT_EQ(vertex.color.w, 1.0f);
    }
    EXPECT_GT(surfaceVertices, 100u);
    EXPECT_GT(shorelineVertices, 20u);
}

// the claim uv0.x makes is ARC ALONG THE CHANNEL: a meander's arc outruns its axial extent
TEST(SplineFillMesherTests, Uv0XFollowsTheChannelRatherThanAWorldAxis){
    const TrenchScene scene;
    ASSERT_TRUE(scene.field.hasRegion());

    SplineFillMeshParams meshParams;
    meshParams.edgeDrop = scene.params.edgeDrop;
    const SplineFillMesh mesh = buildSplineFillMesh(scene.field, scene.ground.grid, scene.stations,
                                                    mesherWholeGrid(scene.ground.grid), meshParams);
    ASSERT_TRUE(mesh.isValid());

    float minArc = mesh.vertices.front().uv.x;
    float maxArc = minArc;
    float minZ = mesh.vertices.front().position.z;
    float maxZ = minZ;
    for (const SplineVertex& vertex : mesh.vertices){
        minArc = std::min(minArc, vertex.uv.x);
        maxArc = std::max(maxArc, vertex.uv.x);
        minZ = std::min(minZ, vertex.position.z);
        maxZ = std::max(maxZ, vertex.position.z);
        // inside <= depth + edgeDrop at both ends of every crossing: never deeper shore than the ground says
        EXPECT_GE(vertex.uv.y, -scene.params.edgeDrop - 1.0e-4f);
    }
    // the centreline x = 2 sin(0.4 z) is about 14 % longer than its z extent
    EXPECT_GT(maxArc - minArc, (maxZ - minZ) * 1.05f) << "uv0.x tracks a world axis, not the channel";

    const float upstreamEdge = minZ + (maxZ - minZ) * 0.25f;
    const float downstreamEdge = minZ + (maxZ - minZ) * 0.75f;
    float worstUpstream = -std::numeric_limits<float>::infinity();
    float bestDownstream = std::numeric_limits<float>::infinity();
    uint32_t upstream = 0;
    uint32_t downstream = 0;
    for (const SplineVertex& vertex : mesh.vertices){
        if (vertex.position.z <= upstreamEdge){
            worstUpstream = std::max(worstUpstream, vertex.uv.x);
            upstream++;
        }else if (vertex.position.z >= downstreamEdge){
            bestDownstream = std::min(bestDownstream, vertex.uv.x);
            downstream++;
        }
    }
    ASSERT_GT(upstream, 20u);
    ASSERT_GT(downstream, 20u);
    EXPECT_LT(worstUpstream, bestDownstream) << "uv0.x must increase downstream";
}

// depth is a property of the WATER: it must not move when the lattice does
TEST(SplineFillMesherTests, DepthIsIndependentOfWhereTheLatticeStarts){
    const auto build = [](float originShift){
        MesherTestGround ground(41u, 41u, 0.5f, -10.0f + originShift, -10.0f, [originShift](float x, float){
            return std::abs(x - originShift) <= 3.0f ? -1.5f : 10.0f;
        });
        std::vector<Vector3> points;
        for (float z = -8.0f; z <= 8.0f + 1.0e-4f; z += 0.5f){
            points.push_back(Vector3(originShift, 0.0f, z));
        }
        const std::vector<SplineStation> stations = mesherMakeStations(points);

        SplineFillParams params;
        params.maxHalfWidth = 8.0f;
        params.edgeDrop = 0.1f;
        const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);

        SplineFillMeshParams meshParams;
        meshParams.edgeDrop = params.edgeDrop;
        return buildSplineFillMesh(field, ground.grid, stations, mesherWholeGrid(ground.grid), meshParams);
    };

    const SplineFillMesh here = build(0.0f);
    const SplineFillMesh shifted = build(2.5f); // a whole number of cells along
    ASSERT_TRUE(here.isValid());
    ASSERT_EQ(here.vertices.size(), shifted.vertices.size());
    for (size_t i = 0; i < here.vertices.size(); i++){
        EXPECT_FLOAT_EQ(here.vertices[i].uv.y, shifted.vertices[i].uv.y) << "depth moved with the lattice origin at vertex " << i;
        EXPECT_FLOAT_EQ(here.vertices[i].uv.x, shifted.vertices[i].uv.x) << "arc moved with the lattice origin at vertex " << i;
    }
}

TEST(SplineFillMesherTests, ABlockWithNoWetCellProducesNothing){
    const TrenchScene scene;
    // a block well clear of the channel
    const SplineFillChunkRange empty{0u, 0u, 4u, 4u};
    const SplineFillMesh mesh = buildSplineFillMesh(scene.field, scene.ground.grid, scene.stations, empty, SplineFillMeshParams{});
    EXPECT_FALSE(mesh.isValid());
    EXPECT_TRUE(mesh.vertices.empty());
    EXPECT_TRUE(mesh.indices.empty());
}

// the carve covers every cell of the field's block exactly once, in chunks aligned to whole
// multiples of their size on the grid
TEST(SplineFillMesherTests, CarvingCoversEveryCellExactlyOnce){
    MesherTestGround ground(37u, 29u, 0.5f, 0.0f, 0.0f, [](float, float){ return 0.0f; });
    const std::vector<SplineStation> stations = mesherStraightRun(0.5f, 7.0f, 8.0f, 0.5f);
    SplineFillParams params;
    params.maxHalfWidth = 3.0f;
    const SplineFillResult field = buildSplineFillField(stations, ground.grid, params);
    ASSERT_GE(field.countX, 2u);
    ASSERT_LT(field.countX, ground.grid.countX) << "the field's block must not be the whole grid here";

    const std::vector<SplineFillChunkRange> chunks = carveFillChunks(field, 8u);
    ASSERT_FALSE(chunks.empty());

    std::set<std::pair<uint32_t, uint32_t>> covered;
    for (const SplineFillChunkRange& chunk : chunks){
        EXPECT_TRUE(chunk.minCornerX % 8u == 0u || chunk.minCornerX == field.minCornerX);
        EXPECT_TRUE(chunk.minCornerZ % 8u == 0u || chunk.minCornerZ == field.minCornerZ);
        EXPECT_LE(chunk.maxCornerX - chunk.minCornerX, 9u);
        for (uint32_t cz = chunk.minCornerZ; cz + 1u < chunk.maxCornerZ; cz++){
            for (uint32_t cx = chunk.minCornerX; cx + 1u < chunk.maxCornerX; cx++){
                EXPECT_TRUE(covered.insert(std::make_pair(cx, cz)).second) << "cell (" << cx << ", " << cz << ") is carved twice";
            }
        }
    }
    EXPECT_EQ(covered.size(), static_cast<size_t>(field.countX - 1u) * (field.countZ - 1u)) << "the carve left cells uncovered";
}

// ---- AC6 ----

// A synthetic bowl lake: its filled plan area is the analytic disc's within 2 %, the surface is
// closed without T-junctions, every triangle faces up, and the rim sits edgeDrop below the water
// with uv0.y = -edgeDrop.
TEST(SplineFillMesherTests, ABowlLakeFillsTheAnalyticDiscWithABuriedRim){
    const BowlScene scene;
    ASSERT_TRUE(scene.field.hasRegion());
    EXPECT_EQ(scene.field.diagnostics.drySeeds, 0u);
    EXPECT_EQ(scene.field.diagnostics.overBankCorners, 0u);

    SplineFillMeshParams meshParams;
    meshParams.edgeDrop = scene.params.edgeDrop;
    const SplineFillMesh mesh = buildSplineFillMesh(scene.field, scene.ground.grid, scene.stations,
                                                    mesherWholeGrid(scene.ground.grid), meshParams);
    ASSERT_TRUE(mesh.isValid());

    const double disc = static_cast<double>(PI) * scene.shoreRadius * scene.shoreRadius;
    const double area = mesherPlanArea(mesh);
    EXPECT_NEAR(area / disc, 1.0, 0.02) << "filled " << area << " m2 against the disc's " << disc << " m2";

    mesherExpectClosedWithoutTJunctions(mesh);
    expectWellFormed(mesh.vertices, mesh.indices, "bowl lake", false);

    // a shoreline vertex can land on a lattice point where the contour passes through one exactly
    // ((4, 6) and its mirrors), so the two kinds are told apart by their height
    uint32_t rim = 0;
    for (const SplineVertex& vertex : mesh.vertices){
        if (vertex.position.y > 0.5f - 0.5f * scene.params.edgeDrop){
            EXPECT_FLOAT_EQ(vertex.position.y, 0.5f);
            EXPECT_TRUE(mesherOnLattice(scene.ground.grid, vertex.position)) << "a surface vertex off the lattice";
            continue;
        }
        rim++;
        EXPECT_NEAR(vertex.position.y, 0.5f - scene.params.edgeDrop, 1.0e-5f) << "the rim is buried edgeDrop below the water";
        EXPECT_NEAR(vertex.uv.y, -scene.params.edgeDrop, 1.0e-3f) << "the rim reads -edgeDrop deep";
        // and it lies on the contour, the circle where the ground stands edgeDrop above the water
        EXPECT_NEAR(std::hypot(vertex.position.x, vertex.position.z), scene.shoreRadius, 0.02f);
    }
    EXPECT_GT(rim, 100u);
}

// The lake cut into lattice chunks: every chunk fits a uint16 index buffer, the chunks together are
// the whole surface (same triangles, same area), and every vertex two chunks share is bit-identical.
TEST(SplineFillMesherTests, ALakeInLatticeChunksIsTheWholeSurfaceWithBitIdenticalSeams){
    const BowlScene scene;
    SplineFillMeshParams meshParams;
    meshParams.edgeDrop = scene.params.edgeDrop;
    const SplineFillMesh whole = buildSplineFillMesh(scene.field, scene.ground.grid, scene.stations,
                                                     mesherWholeGrid(scene.ground.grid), meshParams);
    ASSERT_TRUE(whole.isValid());

    const std::vector<SplineFillChunkRange> chunks = carveFillChunks(scene.field, 16u);
    ASSERT_GE(chunks.size(), 4u);
    std::map<std::pair<float, float>, SplineVertex> seen;
    size_t triangles = 0;
    double area = 0.0;
    uint32_t shared = 0;
    for (const SplineFillChunkRange& chunk : chunks){
        const SplineFillMesh mesh = buildSplineFillMesh(scene.field, scene.ground.grid, scene.stations, chunk, meshParams);
        if (!mesh.isValid()){
            continue;
        }
        EXPECT_LE(mesh.vertices.size(), 65535u);
        expectWellFormed(mesh.vertices, mesh.indices, "lake chunk", false);
        triangles += mesh.indices.size() / 3u;
        area += mesherPlanArea(mesh);
        for (const SplineVertex& vertex : mesh.vertices){
            const auto key = std::make_pair(vertex.position.x, vertex.position.z);
            const auto found = seen.find(key);
            if (found == seen.end()){
                seen.emplace(key, vertex);
            }else{
                shared++;
                EXPECT_TRUE(vertexBitsEqual(found->second, vertex)) << "seam vertex at " << key.first << ", " << key.second;
            }
        }
    }
    EXPECT_GT(shared, 20u);
    EXPECT_EQ(triangles, whole.indices.size() / 3u);
    EXPECT_NEAR(area, mesherPlanArea(whole), 1.0e-6 * mesherPlanArea(whole));
}
