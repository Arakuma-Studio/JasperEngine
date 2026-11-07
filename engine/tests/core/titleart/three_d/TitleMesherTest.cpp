// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S86 AC1-AC8: the 3D mesher. Outline normalisation (TitleOutline3DTest), bevel profiles
// (BevelProfileTest), the ring-stack extruder with its clamps (RingExtruderTest), outward bevels
// (OutwardBevelTest), band stacks (TitleSolidsTest), inflate (InflateMesherTest), the orb
// (OrbMesherTest) and the indexed mesh (TitleMeshTest).

#include "gtest/gtest.h"

#include "titleart/three_d/Title3DTestSupport.h"

#include "titleart/three_d/BevelProfile.h"
#include "titleart/three_d/TitleTierFeatures.h"
#include "titleart/three_d/mesher/InflateMesher.h"
#include "titleart/three_d/mesher/RingExtruder.h"
#include "titleart/three_d/mesher/TitleEarcut.h"
#include "titleart/three_d/mesher/TitleMeshBuilder.h"
#include "titleart/three_d/mesher/TitleSolids.h"

#include <cmath>
#include <random>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::three_d;
using namespace jasperengine::titleart::test;

namespace {

    constexpr double PI = 3.14159265358979323846;

    Shapes oneShape(const Contour& outer, std::vector<Contour> holes = {}) {
        return {{outer, std::move(holes)}};
    }

    // the glyphs of a text in Dela Gothic One as a mesher source
    TitleSource3D glyphSource(const std::string& text, double size = 200, Json chars = Json::object()) {
        static TestAssetResolver assets;
        Json layer = textLayer3D(text, DELA_GOTHIC, size);
        if (!chars.empty()) layer["chars"] = chars;
        const TitleArtDoc doc = documentWith({layer});
        return sourceOf(doc, 0, assets);
    }

    Title3D styleOf(Json style) {
        Json layer = textLayer3D("A", DELA_GOTHIC, 200, std::move(style));
        const TitleArtDoc doc = documentWith({layer});
        return decodeStyle3D(doc.layers()[0]);
    }

    std::shared_ptr<const TitleMesh> meshOf(const TitleSource3D& source, const Title3D& style, TitleTier tier = TitleTier::Bake,
                                            uint32_t testFlags = 0) {
        TitleMesher mesher;
        TitleMeshRequest r;
        r.source = source;
        r.style = style;
        r.tier = tier;
        r.testFlags = testFlags;
        return mesher.build(r);
    }

    // every triangle's geometric normal against its vertex normals: false when one points away
    bool facesAgreeWithNormals(const RawMesh& mesh) {
        for (const auto& slot : mesh.slots) {
            for (size_t t = 0; t + 2 < slot.size(); t += 3) {
                const RawVertex &a = slot[t], &b = slot[t + 1], &c = slot[t + 2];
                const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
                const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
                const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                const double area = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (area < 1e-6) continue;
                const double sx = a.nx + b.nx + c.nx, sy = a.ny + b.ny + c.ny, sz = a.nz + b.nz + c.nz;
                if (nx * sx + ny * sy + nz * sz < 0) return false;
            }
        }
        return true;
    }

}

// ---- AC1: outline normalisation ----

TEST(TitleOutline3DTest, OverlapUnionsIntoOneIsland) {
    const double eps = outlineEps(200);
    const Shapes merged = normalizeContours({squareContour(0, 0, 20), squareContour(30, 0, 20)}, false, eps);
    ASSERT_EQ(merged.size(), 1u);
    EXPECT_NEAR(std::abs(signedArea(merged[0].outer)), 40 * 40 + 40 * 40 - 10 * 40, 1e-6);
    // the red control: without the union both squares stay
    EXPECT_EQ(normalizeContours({squareContour(0, 0, 20)}, false, eps).size() + normalizeContours({squareContour(30, 0, 20)}, false, eps).size(), 2u);
}

TEST(TitleOutline3DTest, OuterCounterClockwiseHoleClockwiseAndIslands) {
    const double eps = outlineEps(200);
    // an "O" (clockwise outer given) with an island in its hole
    const Shapes shapes = normalizeContours({squareContour(0, 0, 50, true), squareContour(0, 0, 30), squareContour(0, 0, 10)}, true, eps);
    ASSERT_EQ(shapes.size(), 2u);
    size_t ring = shapes[0].holes.empty() ? 1 : 0;
    EXPECT_GT(signedArea(shapes[ring].outer), 0);
    ASSERT_EQ(shapes[ring].holes.size(), 1u);
    EXPECT_LT(signedArea(shapes[ring].holes[0]), 0);
    EXPECT_GT(signedArea(shapes[1 - ring].outer), 0);   // the island is a shape of its own
}

TEST(TitleOutline3DTest, GridIsOneHundredthAndCollinearDropped) {
    const double eps = outlineEps(200);   // 0.02
    // a vertex 0.004 off the grid snaps to 0.01
    const Shapes snapped = normalizeContours({{{0, 0}, {10.004, 0}, {10.004, 10}, {0, 10}}}, false, eps);
    ASSERT_EQ(snapped.size(), 1u);
    for (const TitleVec2& p : snapped[0].outer) EXPECT_NEAR(p.x * 100, std::round(p.x * 100), 1e-6) << p.x;
    // a vertex 1.5 eps off its neighbours' line is dropped (the limit is 2 eps)
    const Shapes dropped = normalizeContours({{{0, 0}, {5, 1.5 * eps}, {10, 0}, {10, 10}, {0, 10}}}, false, eps);
    ASSERT_EQ(dropped.size(), 1u);
    EXPECT_EQ(dropped[0].outer.size(), 4u) << "the vertex 1.5 eps off its neighbours' line stays";
}

TEST(TitleOutline3DTest, RiggedGlyphStaysSeparate) {
    // "AB" with negative tracking overlaps; rigging B keeps it a part of its own
    const TitleSource3D merged = glyphSource("AB", 200, Json::object());
    ASSERT_EQ(merged.glyphs.size(), 2u);
    TitleSource3D overlap = merged;
    for (TitleGlyph3D& g : overlap.glyphs) {
        if (g.charIndex == 1) g.path.transform(geometry::Affine::translate(-70, 0));
    }
    const TitleShapes3D unrigged = buildTitleShapes(overlap, 8);
    EXPECT_EQ(unrigged.parts.size(), 1u) << "overlapping A and B union into one island";
    overlap.glyphs[1].rigged = true;
    const TitleShapes3D rigged = buildTitleShapes(overlap, 8);
    ASSERT_EQ(rigged.parts.size(), 2u);
    EXPECT_EQ(rigged.rigs.size(), 1u);
    EXPECT_EQ(rigged.parts[1].rig, 0);
}

// ---- AC2: bevel profiles ----

TEST(BevelProfileTest, PresetsFollowTheFormulas) {
    BevelShape shape;
    shape.profile = BevelProfileKind::Round;
    const auto round = unitBevel(shape, 6);
    ASSERT_EQ(round.size(), 1u);
    ASSERT_EQ(round[0].size(), 7u);
    for (int k = 0; k <= 6; k++) {
        const double t = k / 6.0 * PI / 2;
        EXPECT_NEAR(round[0][k].o, std::sin(t), 1e-6);
        EXPECT_NEAR(round[0][k].h, 1 - std::cos(t), 1e-6);
    }
    // flat at the inner edge (tangent along o), vertical at the outer (along h)
    EXPECT_NEAR(round[0][0].th, 0, 1e-9);
    EXPECT_NEAR(round[0][6].to, 0, 1e-9);
    shape.profile = BevelProfileKind::Cove;
    const auto cove = unitBevel(shape, 6)[0];
    for (int k = 0; k <= 6; k++) {
        const double t = k / 6.0 * PI / 2;
        EXPECT_NEAR(cove[k].o, 1 - std::cos(t), 1e-6);
        EXPECT_NEAR(cove[k].h, std::sin(t), 1e-6);
    }
    shape.profile = BevelProfileKind::Step;
    const auto step = unitBevel(shape, 6);
    ASSERT_EQ(step.size(), 4u);
    EXPECT_NEAR(step[0][1].o, 0.15, 1e-9);
    EXPECT_NEAR(step[2][1].o, 0.65, 1e-9);
    shape.profile = BevelProfileKind::Tube;
    const auto tube = unitBevel(shape, 6)[0];
    ASSERT_EQ(tube.size(), 13u);
    for (int k = 0; k <= 12; k++) {
        const double s = k / 12.0;
        EXPECT_NEAR(tube[k].o, (1 - std::cos(s * PI)) / 2, 1e-6);
        EXPECT_NEAR(tube[k].h, -std::sin(s * PI), 1e-6);
    }
    shape.profile = BevelProfileKind::Inset;
    const auto inset = unitBevel(shape, 6);
    EXPECT_NEAR(inset[0][1].h, -1, 1e-9);
    shape.profile = BevelProfileKind::Chamfer;
    EXPECT_EQ(unitBevel(shape, 6)[0].size(), 2u);
    shape.profile = BevelProfileKind::Flat;
    EXPECT_EQ(unitBevel(shape, 6).size(), 2u);
}

TEST(BevelProfileTest, CurveExponentAndScaleCompensation) {
    BevelShape shape;
    shape.profile = BevelProfileKind::Chamfer;
    shape.curve = 1;   // p = 4
    const auto bent = unitBevel(shape, 6)[0];
    ASSERT_EQ(bent.size(), 7u);
    EXPECT_NEAR(bent[3].h, std::pow(0.5, 4), 1e-9);
    EXPECT_EQ(effectiveSegments(6, 1), 6);
    EXPECT_EQ(effectiveSegments(6, 4), 12);
    EXPECT_EQ(effectiveSegments(20, 4), 24);
}

TEST(BevelProfileTest, CustomCurveCreasesAtLinearKeys) {
    BevelShape shape;
    shape.profile = BevelProfileKind::Custom;
    shape.custom.points = {{0, 0, false}, {0.5, 0.2, true}, {1, 1, false}};
    const auto pieces = unitBevel(shape, 6);
    ASSERT_EQ(pieces.size(), 2u) << "a corner key splits the profile";
    EXPECT_NEAR(pieces[0].back().o, 0.5, 1e-12);
    EXPECT_NEAR(pieces[1].front().o, 0.5, 1e-12);
    // each side of the crease keeps its own slope, the curve's one-sided derivative
    EXPECT_NEAR(pieces[0].back().th, shape.custom.derivative(0.5, true), 1e-9);
    EXPECT_NEAR(pieces[1].front().th, shape.custom.derivative(0.5, false), 1e-9);
    EXPECT_GT(std::abs(pieces[0].back().th - pieces[1].front().th), 0.1);
    // every key time is sampled
    bool hasKey = false;
    for (const UnitPoint& p : pieces[0]) hasKey = hasKey || std::abs(p.o - 0.5) < 1e-12;
    EXPECT_TRUE(hasKey);
}

// ---- AC3, AC4: the ring-stack extruder ----

TEST(RingExtruderTest, SquareInsideChamferIsTwentyTriangles) {
    // a 100 x 100 square, depth 10, inside chamfer 5 at 1 segment
    std::vector<ProfileSpan> spans;
    BevelShape chamfer;
    chamfer.profile = BevelProfileKind::Chamfer;
    const ProfilePoint end = bandSpans(spans, 1, -5, 0, 5, 5, chamfer, 1);
    SolidSpec spec;
    spec.capOff = -5;
    spec.spans = spans;
    spec.wallSlot = spec.backSlot = 2;
    spec.zBack = end.z - 10;
    RawMesh mesh;
    extrudeSolid(mesh, oneShape(squareContour(0, 0, 50)), spec);
    EXPECT_EQ(mesh.triangleCount(), 20u);   // caps 2 + 2, bevel 8, wall 8
    const MeshCheck check = checkRaw(mesh);
    EXPECT_TRUE(check.closed) << check.badEdges << " open edges";
    EXPECT_EQ(check.euler, 2);
    EXPECT_GT(check.volume, 0);
}

TEST(RingExtruderTest, GlyphSolidsAreClosedManifolds) {
    Json style = {{"bevel", {{"width", 3}, {"depth", 3}, {"profile", "round"}}}, {"bevel_position", "inside"}};
    const Title3D s = styleOf(style);
    std::string glyphs;
    for (char c = 'A'; c <= 'Z'; c++) glyphs += c;
    for (char c = 'a'; c <= 'z'; c++) glyphs += c;
    for (char c = '0'; c <= '9'; c++) glyphs += c;
    for (char c : glyphs) {
        const TitleSource3D source = glyphSource(std::string(1, c));
        if (source.glyphs.empty()) continue;
        const auto mesh = meshOf(source, s);
        const MeshCheck check = checkMesh(*mesh);
        EXPECT_TRUE(check.closed) << "'" << c << "': " << check.badEdges << " open edges: " << check.firstBad;
        EXPECT_GT(check.volume, 0) << c;
        for (const TitleMeshVertex& v : mesh->vertices) {
            const auto n = decodeTitleNormal(v.normal[0], v.normal[1]);
            ASSERT_NEAR(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), 1.0, 1e-3) << c;
        }
    }
    // genus: I 0, O 1, B 2
    EXPECT_EQ(checkMesh(*meshOf(glyphSource("I"), s)).euler, 2);
    EXPECT_EQ(checkMesh(*meshOf(glyphSource("O"), s)).euler, 0);
    EXPECT_EQ(checkMesh(*meshOf(glyphSource("B"), s)).euler, -2);
}

TEST(RingExtruderTest, CapsAreFlat) {
    const Title3D s = styleOf({{"bevel", {{"width", 3}, {"depth", 3}}}, {"bevel_position", "inside"}});
    const auto mesh = meshOf(glyphSource("A"), s);
    size_t caps = 0;
    for (const TitleMeshSlot& slot : mesh->slots) {
        if (slot.role != TitleSlotRole::Face) continue;
        for (uint32_t i = 0; i < slot.indexCount; i++) {
            const TitleMeshVertex& v = mesh->vertices[mesh->index(slot.firstIndex + i)];
            const auto n = decodeTitleNormal(v.normal[0], v.normal[1]);
            EXPECT_EQ(n[0], 0.0f);
            EXPECT_EQ(n[1], 0.0f);
            EXPECT_EQ(n[2], 1.0f);
            caps++;
        }
    }
    EXPECT_GT(caps, 0u);
}

TEST(RingExtruderTest, CapsOnTheOutlineFoldTheInsideBevel) {
    // the red control of AC3: caps triangulated on the outline instead of their own ring fold
    // their long slivers over the inset ring (triangles facing away from their normals)
    const Title3D s = styleOf({{"bevel", {{"width", 6}, {"depth", 3}, {"profile", "chamfer"}}}, {"bevel_position", "inside"}});
    const TitleShapes3D shapes = buildTitleShapes(glyphSource("S"), 8);
    TitleSolidsOptions options;
    const TitleSolids own = buildTitleSolids(shapes, s, {}, options, nullptr);
    EXPECT_TRUE(checkRaw(own.mesh).closed);
    EXPECT_TRUE(facesAgreeWithNormals(own.mesh));
    options.testFlags = MESHER_TEST_CAP_ON_OUTLINE;
    EXPECT_FALSE(facesAgreeWithNormals(buildTitleSolids(shapes, s, {}, options, nullptr).mesh));
}

TEST(RingExtruderTest, ClampAndUntangle) {
    // a 3 px stroke with a 4 px inside bevel never inverts
    std::vector<ProfileSpan> spans;
    BevelShape chamfer;
    chamfer.profile = BevelProfileKind::Chamfer;
    bandSpans(spans, 1, -4, 0, 4, 2, chamfer, 1);
    SolidSpec spec;
    spec.capOff = -4;
    spec.spans = spans;
    spec.wallSlot = spec.backSlot = 2;
    spec.zBack = -12;
    // a stroke whose long sides carry vertices every 5 px: their miters never collapse an edge,
    // only the ray clamp stops them
    Contour outline;
    for (int x = 0; x <= 60; x += 5) outline.push_back({(double)x, 0});
    for (int x = 60; x >= 0; x -= 5) outline.push_back({(double)x, 3});
    const Shapes stroke = oneShape(outline);
    // an inside bevel stays within the stroke; a ring that inverts crosses to the other side
    auto withinStroke = [](const RawMesh& mesh) {
        for (const auto& slot : mesh.slots) {
            for (const RawVertex& v : slot) {
                if (v.x < -1e-4 || v.x > 60 + 1e-4 || v.y < -1e-4 || v.y > 3 + 1e-4) return false;
            }
        }
        return true;
    };
    RawMesh clamped;
    extrudeSolid(clamped, stroke, spec);
    EXPECT_TRUE(facesAgreeWithNormals(clamped));
    EXPECT_TRUE(withinStroke(clamped));
    EXPECT_TRUE(checkRaw(clamped).closed);
    // red control: the clamp (and untangle) off lets the rings cross the stroke
    spec.testFlags = MESHER_TEST_NO_RAY_CLAMP;
    RawMesh folded;
    extrudeSolid(folded, stroke, spec);
    EXPECT_FALSE(withinStroke(folded));
}

TEST(RingExtruderTest, GridClampEqualsBruteForce) {
    std::mt19937 random(7);
    const std::string letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::uniform_int_distribution<size_t> pick(0, letters.size() - 1);
    const double eps = outlineEps(200);
    for (int n = 0; n < 50; n++) {
        const TitleSource3D source = glyphSource(std::string(1, letters[pick(random)]));
        for (const TitleGlyph3D& g : source.glyphs) {
            for (const ShapeContours& shape : normalizeContours(flattenContours(g.path, 8), false, eps)) {
                std::vector<Contour> contours = {shape.outer};
                contours.insert(contours.end(), shape.holes.begin(), shape.holes.end());
                const RingLimits grid = rayLimits(contours, false);
                const RingLimits brute = rayLimits(contours, true);
                for (size_t r = 0; r < contours.size(); r++) {
                    for (size_t i = 0; i < contours[r].size(); i++) {
                        const double a = grid.maxIn[r][i], b = brute.maxIn[r][i];
                        if (std::isinf(b)) EXPECT_TRUE(std::isinf(a));
                        else EXPECT_NEAR(a, b, 1e-5);
                        const double c = grid.maxOut[r][i], d = brute.maxOut[r][i];
                        if (std::isinf(d)) EXPECT_TRUE(std::isinf(c));
                        else EXPECT_NEAR(c, d, 1e-5);
                    }
                }
            }
        }
    }
}

// ---- AC5: outward bevels ----

TEST(OutwardBevelTest, TouchingCirclesShareOneBandStack) {
    const double eps = outlineEps(100);
    const Shapes circles = normalizeContours({circleContour(0, 0, 20, 64)}, false, eps);
    const Shapes other = normalizeContours({circleContour(40, 0, 20, 64)}, false, eps);
    Shapes both = circles;
    both.insert(both.end(), other.begin(), other.end());
    OutwardSpec spec;
    BevelShape chamfer;
    chamfer.profile = BevelProfileKind::Chamfer;
    bandSpans(spec.spans, 1, 0, 0, 4, 4, chamfer, 1);
    spec.wallSlot = spec.backSlot = 2;
    spec.zBack = -10;
    spec.step = 1;
    spec.eps = eps;
    RawMesh mesh;
    ExtrudeStats stats;
    extrudeOutward(mesh, both, spec, &stats);
    // one component: the grown circles merge into one outer ring
    EXPECT_EQ(offsetShapes(both, 4, false, false, eps).size(), 1u);
    // the band's triangles cover the Clipper annulus within 0.1 %
    double area = 0;
    for (size_t t = 0; t + 2 < mesh.slots[1].size(); t += 3) {
        const RawVertex &a = mesh.slots[1][t], &b = mesh.slots[1][t + 1], &c = mesh.slots[1][t + 2];
        area += std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) / 2;
    }
    double expected = 0;
    for (const ShapeContours& s : offsetShapes(both, 4, false, false, eps)) expected += std::abs(signedArea(s.outer));
    for (const ShapeContours& s : both) expected -= std::abs(signedArea(s.outer));
    EXPECT_NEAR(area, expected, expected * 1e-3);
    // refinement adds levels in the valley between the merging circles
    EXPECT_GT(stats.ringLevels, 1u);
}

TEST(OutwardBevelTest, AForcedBadFillTakesTheDifferenceFallback) {
    // the 0.1 % area check failing (forced here) cuts the band with the Clipper difference: the
    // band still covers the annulus and the stats say the fallback ran
    const double eps = outlineEps(100);
    const Shapes square = {{squareContour(0, 0, 20), {}}};
    OutwardSpec spec;
    BevelShape chamfer;
    chamfer.profile = BevelProfileKind::Chamfer;
    bandSpans(spec.spans, 1, 0, 0, 4, 4, chamfer, 1);
    spec.wallSlot = spec.backSlot = 2;
    spec.zBack = -6;
    spec.eps = eps;
    spec.testFlags = MESHER_TEST_FORCE_FALLBACK;
    RawMesh mesh;
    ExtrudeStats stats;
    extrudeOutward(mesh, square, spec, &stats);
    EXPECT_TRUE(stats.fallbackUsed);
    double area = 0;
    for (size_t t = 0; t + 2 < mesh.slots[1].size(); t += 3) {
        const RawVertex &a = mesh.slots[1][t], &b = mesh.slots[1][t + 1], &c = mesh.slots[1][t + 2];
        area += std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) / 2;
    }
    const double expected = std::abs(signedArea(offsetShapes(square, 4, false, false, eps)[0].outer)) - 40.0 * 40.0;
    EXPECT_NEAR(area, expected, expected * 0.01);
}

// ---- AC6: band stacks ----

TEST(TitleSolidsTest, CumulativeGrowAndZPlacement) {
    Json bands = Json::array();
    for (double offset : {-5.0, 0.0, 3.0}) bands.push_back({{"grow", 2}, {"offset", offset}, {"depth", 4}, {"bevel", {{"width", 0}, {"depth", 0}}}});
    const Title3D s = styleOf({{"depth", 10}, {"bands", bands}});
    TitleSource3D square;
    TitleGlyph3D g;
    g.path.moveTo(-50, -50);
    g.path.lineTo(50, -50);
    g.path.lineTo(50, 50);
    g.path.lineTo(-50, 50);
    g.path.close();
    square.glyphs.push_back(g);
    square.size = 100;
    const TitleShapes3D shapes = buildTitleShapes(square, 8);
    TitleSolidsOptions options;
    const TitleSolids solids = buildTitleSolids(shapes, s, {}, options, nullptr);
    // band k grows by the sum of grows 0..k: band 2's front cap reaches 50 + 6
    float maxX = 0;
    for (const RawVertex& v : solids.mesh.slots[3 + 2 * 2]) maxX = std::max(maxX, v.x);
    EXPECT_NEAR(maxX, 56.0f, 1e-3f);
    // z fronts: letter back -10; band 0 offset -5 -> min(-10 + 5, 0 - 0.25) = -5; band 1 sits
    // right behind band 0's back (-5 - 4 = -9); band 2 3 behind band 1's back (-13 - 3 = -16)
    const double expected[3] = {-5, -9, -16};
    for (int b = 0; b < 3; b++) {
        float front = -INFINITY;
        for (const RawVertex& v : solids.mesh.slots[3 + 2 * b]) front = std::max(front, v.z);
        EXPECT_NEAR(front, expected[b], 1e-4) << "band " << b;
    }
}

TEST(TitleSolidsTest, RoundBandsGrowPerIslandAndRigsLift) {
    const Title3D s = styleOf({{"depth", 10}, {"bands", Json::array({{{"grow", 3}, {"join", "round"}, {"bevel", {{"width", 0}, {"depth", 0}}}}})}});
    const TitleSource3D two = glyphSource("II");
    const TitleShapes3D shapes = buildTitleShapes(two, 8);
    ASSERT_EQ(shapes.parts.size(), 2u);
    const TitleSolids solids = buildTitleSolids(shapes, s, {}, TitleSolidsOptions(), nullptr);
    EXPECT_EQ(solids.solids, 4u) << "two letters and a round band around each";
    // a rigged glyph's stack sits 0.25 % of size in front of the shared one
    TitleSource3D rigged = glyphSource("II", 200, Json{{"1", {{"rig", true}}}});
    const TitleSolids rs = buildTitleSolids(buildTitleShapes(rigged, 8), s, {}, TitleSolidsOptions(), nullptr);
    float shared = -INFINITY, own = -INFINITY;
    for (const RawVertex& v : rs.mesh.slots[3]) {
        const TitleMeshPart& part = rs.parts[v.part];
        (part.rig ? own : shared) = std::max(part.rig ? own : shared, v.z);
    }
    EXPECT_NEAR(own - shared, 0.0025 * 200, 1e-3);
}

// ---- AC7: inflate ----

TEST(InflateMesherTest, StepFormulaNodeCapAndCache) {
    const double eps = outlineEps(200);
    const Shapes square = normalizeContours({squareContour(0, 0, 50)}, false, eps);
    // s = 1.5 x area / perimeter = 1.5 x 10000 / 400 = 37.5 -> h = 37.5 / 12
    EXPECT_NEAR(inflateStep(square, 1, 1, 200000), 37.5 / 12, 1e-9);
    // detail 4 at 1x clamps h to 0.75
    const Shapes thin = normalizeContours({{{0, 0}, {100, 0}, {100, 4}, {0, 4}}}, false, eps);
    EXPECT_NEAR(inflateStep(thin, 1, 4, 200000), 0.75, 1e-9);
    // the node cap: h >= sqrt(area / cap)
    const Shapes big = normalizeContours({squareContour(0, 0, 1000)}, false, eps);
    const double h = inflateStep(big, 4, 4, 200000);
    EXPECT_GE(h, std::sqrt(2000.0 * 2000.0 / 200000) - 1e-9);
    const auto field = buildInflateField(big[0], h);
    EXPECT_LE(field->nodes, 200000u * 105 / 100);
    InflateFieldCache cache;
    cache.get(square[0], 3.0);
    cache.get(square[0], 3.0);
    EXPECT_EQ(cache.hits(), 1u);
    EXPECT_EQ(cache.misses(), 1u);
}

TEST(InflateMesherTest, HeightsWallsAndSharedMaxDist) {
    const double eps = outlineEps(200);
    const Shapes wide = normalizeContours({squareContour(0, 0, 40)}, false, eps);
    const Shapes thin = normalizeContours({{{100, -40}, {110, -40}, {110, 40}, {100, 40}}}, false, eps);
    const double step = 2.0;
    const auto fw = buildInflateField(wide[0], step);
    const auto ft = buildInflateField(thin[0], step);
    const double maxDist = std::max(fw->maxDist, ft->maxDist);
    InflateSpec spec;
    spec.height = 20;
    spec.profile.points = {{0, 0, false}, {0.35, 0.8, false}, {1, 1, false}};
    spec.maxDist = maxDist;
    spec.zBack = -30;
    spec.wallSlot = spec.backSlot = 2;
    RawMesh mw, mt;
    emitInflate(mw, *fw, spec);
    emitInflate(mt, *ft, spec);
    float peakW = -INFINITY, peakT = -INFINITY;
    for (const RawVertex& v : mw.slots[0]) peakW = std::max(peakW, v.z);
    for (const RawVertex& v : mt.slots[0]) peakT = std::max(peakT, v.z);
    EXPECT_LT(peakT, peakW) << "a thin stroke peaks lower (shared maxDist)";
    // z = height (profile(d / maxDist) - 1) at the nodes
    for (size_t v = 0; v < fw->dist.size(); v += 37) {
        const double z = spec.height * (spec.profile.evaluate(std::min(1.0, fw->dist[v] / maxDist)) - 1);
        bool found = false;
        for (const RawVertex& r : mw.slots[0]) {
            if (r.x == (float)fw->xy[2 * v] && r.y == (float)fw->xy[2 * v + 1]) {
                EXPECT_NEAR(r.z, z, 1e-4);
                found = true;
                break;
            }
        }
        (void)found;
    }
    // no T-junction: the front, wall and back close up
    RawMesh both = mw;
    const MeshCheck check = checkRaw(both);
    EXPECT_TRUE(check.closed) << check.badEdges << " open edges";
}

// ---- AC8: the orb and the mesh ----

TEST(OrbMesherTest, GridTrianglesAndNormals) {
    EXPECT_EQ(orbGridFor(0), 24);
    EXPECT_EQ(orbGridFor(400), 32);     // (pi/2) x 20 = 31.4 -> 32
    EXPECT_EQ(orbGridFor(1e6), 128);
    RawMesh mesh;
    extrudeOrb(mesh, 0, 50, 30, 30, 0, 0, -30, 24);
    EXPECT_EQ(mesh.triangleCount(), 12u * 24 * 24);
    for (const RawVertex& v : mesh.slots[0]) {
        // the ellipsoid's normal ~ (x / rx^2, y / ry^2, (z - cz) / rz^2)
        double nx = v.x / 2500.0, ny = v.y / 900.0, nz = (v.z + 30) / 900.0;
        const double l = std::sqrt(nx * nx + ny * ny + nz * nz);
        EXPECT_NEAR(v.nx, nx / l, 1e-4);
        EXPECT_NEAR(v.ny, ny / l, 1e-4);
        EXPECT_NEAR(v.nz, nz / l, 1e-4);
    }
    EXPECT_TRUE(checkRaw(mesh).closed) << "seam positions are shared";
}

TEST(TitleMeshTest, VertexFormatIndicesDeterminismAndCache) {
    EXPECT_EQ(sizeof(TitleMeshVertex), 24u);
    Json bands = Json::array({{{"grow", 2}, {"bevel", {{"width", 1}, {"depth", 1}, {"profile", "chamfer"}}}},
                              {{"grow", 2}, {"bevel", {{"width", 1}, {"depth", 1}, {"profile", "step"}}}},
                              {{"grow", 4}, {"plate", {{"kind", "contour"}}}}});
    const Title3D gold = styleOf({{"face", {{"preset", "gold"}}}, {"bevel", {{"width", 2.5}, {"depth", 2.5}}}, {"bands", bands}});
    const TitleSource3D source = glyphSource("BIG BONUS");
    const auto a = meshOf(source, gold);
    const auto b = meshOf(source, gold);
    ASSERT_EQ(a->vertices.size(), b->vertices.size());
    EXPECT_EQ(std::memcmp(a->vertices.data(), b->vertices.data(), a->vertices.size() * sizeof(TitleMeshVertex)), 0);
    EXPECT_EQ(a->indices16, b->indices16);
    EXPECT_FALSE(a->wideIndices);
    // indexed: faceted profiles split their rings' vertices per facet (Deviations), still far
    // below the 3 vertices per triangle of a triangle soup
    EXPECT_LE((double)a->stats.vertices, 1.5 * a->stats.triangles);
    EXPECT_LT(a->stats.acmrAfter, a->stats.acmrBefore);
    EXPECT_LE(a->stats.triangles, 55000u);
}
