// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// LineRibbonBuilder (lines and trails spec 6.2, 6.3): vertex and index counts for (points,
// corner, cap, loop) combinations against counts() and hand numbers, u for each texture mode,
// widths from the curve, colours from the gradient or per point, skipped duplicates and
// non-finite points, and the geometry the vertex shader makes of it (through vertexOffset,
// the C++ mirror of includes/lines_vs.glsl): the area of a straight line, a miter, a bevel
// past the limit, a round joint and round caps, U-turns and lines seen end-on without NaN.
// The width and colour shortcuts (startWidth, startColor). Pure.

#include <gtest/gtest.h>

#include "render/LineRibbonBuilder.h"
#include "math/AnimationCurve.h"
#include "math/Gradient.h"
#include "util/Color.h"

#include <cmath>
#include <vector>

using namespace jasperengine;

namespace {

    const float PI = 3.14159265358979f;

    struct Built {
        std::vector<LineRibbonVertex> vertices;
        std::vector<uint32_t> indices;
        LineRibbonResult result;
    };

    Built build(const std::vector<Vector3>& points, const LineRibbonSettings& settings, const float* pointT = nullptr,
                const Vector4* colors = nullptr){
        Built built;
        built.result = LineRibbonBuilder::build(points.data(), points.size(), settings, built.vertices, built.indices, pointT, colors);
        return built;
    }

    // the vertex where the shader puts it, the ribbon facing +Z (a camera looking down -Z)
    Vector3 placed(const LineRibbonVertex& vertex){
        return vertex.position + LineRibbonBuilder::vertexOffset(vertex, Vector3(0.0f, 0.0f, 1.0f)) * vertex.params.y;
    }

    // the summed area of the triangles in the XY plane (inner corners have none)
    float area(const Built& built){
        float total = 0.0f;
        for (size_t i = 0; i + 2 < built.indices.size(); i += 3){
            const Vector3 a = placed(built.vertices[built.indices[i]]);
            const Vector3 b = placed(built.vertices[built.indices[i + 1]]);
            const Vector3 c = placed(built.vertices[built.indices[i + 2]]);
            total += std::fabs((b - a).crossProduct(c - a).z) * 0.5f;
        }
        return total;
    }

    void expectCounts(const std::vector<Vector3>& points, bool loop, unsigned int corner, unsigned int cap, size_t vertices, size_t indices){
        LineRibbonSettings settings;
        settings.loop = loop;
        settings.numCornerVertices = corner;
        settings.numCapVertices = cap;
        Built built = build(points, settings);
        size_t countVertices = 0, countIndices = 0;
        LineRibbonBuilder::counts(points.size(), loop, corner, cap, countVertices, countIndices);
        EXPECT_EQ(built.vertices.size(), vertices) << points.size() << " points, loop " << loop << ", corner " << corner << ", cap " << cap;
        EXPECT_EQ(built.indices.size(), indices) << points.size() << " points, loop " << loop << ", corner " << corner << ", cap " << cap;
        EXPECT_EQ(countVertices, vertices);
        EXPECT_EQ(countIndices, indices);
        EXPECT_EQ(built.result.vertexCount, vertices);
        EXPECT_EQ(built.result.indexCount, indices);
        for (uint32_t index : built.indices){
            ASSERT_LT(index, built.vertices.size());
        }
    }

}

TEST(LineRibbonBuilderTest, VertexAndIndexCounts){
    const std::vector<Vector3> two = {Vector3(0, 0, 0), Vector3(1, 0, 0)};
    const std::vector<Vector3> three = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 1, 0)};
    const std::vector<Vector3> four = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 1, 0), Vector3(0, 1, 0)};

    // 2 points: two pairs, one quad
    expectCounts(two, false, 0, 0, 4, 6);
    // a joint: 2 sides x (in, out, the wedge's centre) and a wedge triangle per side
    expectCounts(three, false, 0, 0, 10, 18);
    // corner fans of 2 and caps of 3: + 2 x 2 corner vertices (2 x 3 triangles), caps 1 + 3 each (4 triangles)
    expectCounts(three, false, 2, 3, 22, 54);
    // a loop of 4: the start's out pair and 4 joints, 4 quads
    expectCounts(four, true, 0, 0, 26, 48);
    // caps are ignored on a loop
    expectCounts(four, true, 1, 5, 2 + 4 * 8, 4 * 6 + 4 * 2 * 2 * 3);
    // a loop of 2 draws the segment twice (Unity)
    expectCounts(two, true, 0, 0, 14, 24);
    // counts are capped at 90
    size_t vertices = 0, indices = 0;
    LineRibbonBuilder::counts(3, false, 500, 500, vertices, indices);
    size_t capped = 0, cappedIndices = 0;
    LineRibbonBuilder::counts(3, false, 90, 90, capped, cappedIndices);
    EXPECT_EQ(vertices, capped);
    EXPECT_EQ(indices, cappedIndices);
}

TEST(LineRibbonBuilderTest, NothingBelowTwoPoints){
    LineRibbonSettings settings;
    EXPECT_EQ(build({}, settings).vertices.size(), 0u);
    EXPECT_EQ(build({Vector3(1, 2, 3)}, settings).vertices.size(), 0u);
    // every point the same
    Built same = build({Vector3(1, 2, 3), Vector3(1, 2, 3), Vector3(1, 2, 3)}, settings);
    EXPECT_EQ(same.vertices.size(), 0u);
    EXPECT_EQ(same.indices.size(), 0u);
}

TEST(LineRibbonBuilderTest, DuplicatesAndInvalidPointsAreSkipped){
    LineRibbonSettings settings;
    Built duplicates = build({Vector3(0, 0, 0), Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 0, 0)}, settings);
    EXPECT_EQ(duplicates.vertices.size(), 4u);
    EXPECT_EQ(duplicates.result.invalidPoints, 0u);

    Built nan = build({Vector3(0, 0, 0), Vector3(std::nanf(""), 0, 0), Vector3(1, 0, 0), Vector3(INFINITY, 1, 0)}, settings);
    EXPECT_EQ(nan.vertices.size(), 4u);
    EXPECT_EQ(nan.result.invalidPoints, 2u);
    for (const LineRibbonVertex& vertex : nan.vertices){
        EXPECT_TRUE(std::isfinite(vertex.position.x));
    }
    EXPECT_FLOAT_EQ(nan.result.length, 1.0f);
}

TEST(LineRibbonBuilderTest, TextureU){
    const std::vector<Vector3> points = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(3, 0, 0)};
    // the u of each station (the body vertices of a station share it)
    auto stationU = [&](LineTextureMode mode, Vector2 scale){
        LineRibbonSettings settings;
        settings.textureMode = mode;
        settings.textureScale = scale;
        Built built = build(points, settings);
        std::vector<float> u;
        for (const LineRibbonVertex& vertex : built.vertices){
            if (u.empty() || std::fabs(u.back() - vertex.uv.x) > 1e-6f) u.push_back(vertex.uv.x);
        }
        return u;
    };
    const std::vector<float> stretch = stationU(LineTextureMode::Stretch, Vector2(2.0f, 1.0f));
    ASSERT_EQ(stretch.size(), 3u);
    EXPECT_NEAR(stretch[0], 0.0f, 1e-6f);
    EXPECT_NEAR(stretch[1], 2.0f / 3.0f, 1e-6f);
    EXPECT_NEAR(stretch[2], 2.0f, 1e-6f);

    // Tile: once every 1 / scale.x world units
    const std::vector<float> tile = stationU(LineTextureMode::Tile, Vector2(0.5f, 1.0f));
    ASSERT_EQ(tile.size(), 3u);
    EXPECT_NEAR(tile[1], 0.5f, 1e-6f);
    EXPECT_NEAR(tile[2], 1.5f, 1e-6f);

    const std::vector<float> distribute = stationU(LineTextureMode::DistributePerSegment, Vector2(1.0f, 1.0f));
    ASSERT_EQ(distribute.size(), 3u);
    EXPECT_NEAR(distribute[1], 0.5f, 1e-6f);
    EXPECT_NEAR(distribute[2], 1.0f, 1e-6f);

    const std::vector<float> repeat = stationU(LineTextureMode::RepeatPerSegment, Vector2(3.0f, 1.0f));
    ASSERT_EQ(repeat.size(), 3u);
    EXPECT_NEAR(repeat[1], 3.0f, 1e-6f);
    EXPECT_NEAR(repeat[2], 6.0f, 1e-6f);

    // v: 0 on the -1 side, scale.y on the +1 side
    LineRibbonSettings settings;
    settings.textureScale = Vector2(1.0f, 4.0f);
    Built built = build({Vector3(0, 0, 0), Vector3(1, 0, 0)}, settings);
    for (const LineRibbonVertex& vertex : built.vertices){
        EXPECT_FLOAT_EQ(vertex.uv.y, vertex.params.x < 0.0f ? 0.0f : 4.0f);
    }
}

TEST(LineRibbonBuilderTest, WidthsAndColours){
    const AnimationCurve curve = AnimationCurve::linear(0.0f, 1.0f, 1.0f, 0.0f);
    const Gradient gradient = Gradient::twoColors(Vector4(1, 0, 0, 1), Vector4(0, 0, 1, 0));
    LineRibbonSettings settings;
    settings.widthMultiplier = 2.0f;
    settings.widthCurve = &curve;
    settings.colorGradient = &gradient;
    Built built = build({Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(3, 0, 0)}, settings);
    // stations at t 0, 1/3, 1: half widths 1, 2/3, 0
    EXPECT_NEAR(built.vertices.front().params.y, 1.0f, 1e-6f);
    EXPECT_NEAR(built.vertices.back().params.y, 0.0f, 1e-6f);
    EXPECT_NEAR(built.result.maxHalfWidth, 1.0f, 1e-6f);
    bool middle = false;
    for (const LineRibbonVertex& vertex : built.vertices){
        if (vertex.position == Vector3(1, 0, 0)){
            EXPECT_NEAR(vertex.params.y, 2.0f / 3.0f, 1e-6f);
            // colours mix in sRGB: a third of the way from red to blue is 2/3 sRGB red
            EXPECT_NEAR(vertex.color.x, Color::sRGBToLinear(Vector3(2.0f / 3.0f, 0, 0)).x, 1e-5f);
            EXPECT_NEAR(vertex.color.w, 2.0f / 3.0f, 1e-5f);
            middle = true;
        }
    }
    EXPECT_TRUE(middle);

    // a negative curve is width 0, never flipped
    const AnimationCurve negative = AnimationCurve::constant(-1.0f);
    settings.widthCurve = &negative;
    Built flat = build({Vector3(0, 0, 0), Vector3(1, 0, 0)}, settings);
    for (const LineRibbonVertex& vertex : flat.vertices){
        EXPECT_FLOAT_EQ(vertex.params.y, 0.0f);
    }

    // per-point colours (Segments mode) and parameters (trails: by age)
    const Vector4 colors[2] = {Vector4(0, 1, 0, 1), Vector4(1, 1, 0, 0.5f)};
    const float t[2] = {0.0f, 0.5f};
    settings.widthCurve = &curve;
    Built segment = build({Vector3(0, 0, 0), Vector3(1, 0, 0)}, settings, t, colors);
    EXPECT_EQ(segment.vertices.front().color, colors[0]);
    EXPECT_EQ(segment.vertices.back().color, colors[1]);
    // t 0.5 on a curve 1 -> 0, multiplier 2: width 1, half 0.5
    EXPECT_NEAR(segment.vertices.back().params.y, 0.5f, 1e-6f);
}

TEST(LineRibbonBuilderTest, StraightLineArea){
    LineRibbonSettings settings;
    settings.widthMultiplier = 1.0f;
    Built built = build({Vector3(0, 0, 0), Vector3(2, 0, 0)}, settings);
    EXPECT_NEAR(area(built), 2.0f, 1e-5f);
    // the +1 side is the right of the direction (seen from +Z), the -1 side the left
    for (const LineRibbonVertex& vertex : built.vertices){
        EXPECT_NEAR(placed(vertex).y, vertex.params.x > 0.0f ? -0.5f : 0.5f, 1e-6f);
    }
    // collinear points: the same area
    Built collinear = build({Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(2, 0, 0)}, settings);
    EXPECT_NEAR(area(collinear), 2.0f, 1e-5f);
}

TEST(LineRibbonBuilderTest, MiterBevelAndRoundJoints){
    LineRibbonSettings settings;
    settings.widthMultiplier = 1.0f;
    const std::vector<Vector3> corner = {Vector3(0, 0, 0), Vector3(2, 0, 0), Vector3(2, 2, 0)};

    // a right angle, miter: the union of the two legs with a square outer corner
    Built miter = build(corner, settings);
    EXPECT_NEAR(area(miter), 4.0f, 1e-4f);
    bool outerCorner = false;
    for (const LineRibbonVertex& vertex : miter.vertices){
        const Vector3 p = placed(vertex);
        if (std::fabs(p.x - 2.5f) < 1e-4f && std::fabs(p.y + 0.5f) < 1e-4f) outerCorner = true;
    }
    EXPECT_TRUE(outerCorner);

    // a round joint: the legs and a quarter disc at the corner
    settings.numCornerVertices = 32;
    Built round = build(corner, settings);
    const float expected = 3.75f + PI * 0.25f / 4.0f;
    EXPECT_NEAR(area(round), expected, 2e-3f);
    // the outer side's round vertices are half a width from the corner, the inner side's all
    // on the inner miter point
    int outerFan = 0;
    for (const LineRibbonVertex& vertex : round.vertices){
        if (vertex.params.z == (float)LineRibbonRole_Corner){
            const float distance = (placed(vertex) - vertex.position).length();
            if (std::fabs(distance - 0.5f) < 1e-4f){
                outerFan++;
            }else{
                EXPECT_NEAR(distance, 0.5f * std::sqrt(2.0f), 1e-4f);
            }
        }
    }
    EXPECT_EQ(outerFan, 32);

    // a sharp turn past the miter limit (4 half widths) is bevelled: no spike
    settings.numCornerVertices = 0;
    Built sharp = build({Vector3(0, 0, 0), Vector3(2, 0, 0), Vector3(0, 0.2f, 0)}, settings);
    for (const LineRibbonVertex& vertex : sharp.vertices){
        EXPECT_LE((placed(vertex) - vertex.position).length(), 4.0f * 0.5f + 1e-4f);
    }
}

TEST(LineRibbonBuilderTest, RoundCaps){
    LineRibbonSettings settings;
    settings.widthMultiplier = 1.0f;
    settings.numCapVertices = 64;
    Built built = build({Vector3(0, 0, 0), Vector3(2, 0, 0)}, settings);
    // the body and two half discs of radius 0.5
    EXPECT_NEAR(area(built), 2.0f + PI * 0.25f, 2e-3f);
    // the caps reach past the ends, butt caps do not
    float minX = 0.0f, maxX = 0.0f;
    for (const LineRibbonVertex& vertex : built.vertices){
        minX = std::min(minX, placed(vertex).x);
        maxX = std::max(maxX, placed(vertex).x);
    }
    EXPECT_NEAR(minX, -0.5f, 1e-3f);
    EXPECT_NEAR(maxX, 2.5f, 1e-3f);

    settings.numCapVertices = 0;
    Built butt = build({Vector3(0, 0, 0), Vector3(2, 0, 0)}, settings);
    for (const LineRibbonVertex& vertex : butt.vertices){
        EXPECT_GE(placed(vertex).x, -1e-6f);
        EXPECT_LE(placed(vertex).x, 2.0f + 1e-6f);
    }
}

TEST(LineRibbonBuilderTest, DegenerateViewsHaveNoNaN){
    LineRibbonSettings settings;
    settings.widthMultiplier = 1.0f;
    settings.numCornerVertices = 4;
    settings.numCapVertices = 4;
    // a U-turn
    Built uturn = build({Vector3(0, 0, 0), Vector3(2, 0, 0), Vector3(0, 0, 0)}, settings);
    // the line along the view direction (+Z): the perpendicular falls back
    Built endOn = build({Vector3(0, 0, 0), Vector3(0, 0, 2), Vector3(0, 0, 4)}, settings);
    for (const Built* built : {&uturn, &endOn}){
        for (const LineRibbonVertex& vertex : built->vertices){
            const Vector3 p = placed(vertex);
            EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
        }
    }
}

TEST(LineRibbonBuilderTest, LoopClosesTheLine){
    LineRibbonSettings settings;
    settings.widthMultiplier = 0.2f;
    settings.loop = true;
    // a square of side 2: four legs of 2 x 0.2 sharing miter corners
    Built square = build({Vector3(0, 0, 0), Vector3(2, 0, 0), Vector3(2, 2, 0), Vector3(0, 2, 0)}, settings);
    EXPECT_NEAR(square.result.length, 8.0f, 1e-5f);
    // outer square 2.2^2 minus inner 1.8^2
    EXPECT_NEAR(area(square), 2.2f * 2.2f - 1.8f * 1.8f, 1e-3f);
    // a closing point equal to the first is the loop's own segment
    Built closed = build({Vector3(0, 0, 0), Vector3(2, 0, 0), Vector3(2, 2, 0), Vector3(0, 2, 0), Vector3(0, 0, 0)}, settings);
    EXPECT_EQ(closed.vertices.size(), square.vertices.size());
}

TEST(LineRibbonBuilderTest, WidthAndColourShortcuts){
    float multiplier = 0.0f;
    AnimationCurve curve = AnimationCurve::constant(1.0f);
    LineRibbonBuilder::setEndWidths(multiplier, curve, 0.35f, 0.1f);
    EXPECT_FLOAT_EQ(multiplier, 1.0f);
    EXPECT_FLOAT_EQ(LineRibbonBuilder::widthAt(multiplier, curve, 0.0f), 0.35f);
    EXPECT_FLOAT_EQ(LineRibbonBuilder::widthAt(multiplier, curve, 1.0f), 0.1f);
    LineRibbonBuilder::setEndWidths(multiplier, curve, -1.0f, 0.1f);
    EXPECT_FLOAT_EQ(LineRibbonBuilder::widthAt(multiplier, curve, 0.0f), 0.0f);

    Gradient gradient;
    LineRibbonBuilder::setEndColor(gradient, 0.0f, Vector4(1.0f, 0.5f, 0.0f, 1.0f));
    LineRibbonBuilder::setEndColor(gradient, 1.0f, Vector4(0.0f, 0.0f, 1.0f, 0.0f));
    const Vector4 start = LineRibbonBuilder::colorAt(gradient, 0.0f);
    EXPECT_NEAR(start.y, 0.5f, 1e-5f);
    EXPECT_NEAR(start.w, 1.0f, 1e-6f);
    const Vector4 end = LineRibbonBuilder::colorAt(gradient, 1.0f);
    EXPECT_NEAR(end.z, 1.0f, 1e-5f);
    EXPECT_NEAR(end.w, 0.0f, 1e-6f);
    // linear in memory
    EXPECT_NEAR(gradient.evaluate(0.0f).y, Color::sRGBToLinear(Vector3(0.0f, 0.5f, 0.0f)).y, 1e-5f);
    EXPECT_EQ(gradient.colorKeys.size(), 2u);
}

TEST(LineRibbonBuilderTest, SegmentVectorsAndJointCentres){
    // the vertices carry the segments' vectors (the pixel-width shader clips a segment at the
    // near plane with the point at its other end): zero on an end's open side
    LineRibbonSettings settings;
    Built built = build({Vector3(0, 0, 0), Vector3(2, 0, 0), Vector3(2, 3, 0)}, settings);
    int centres = 0;
    for (const LineRibbonVertex& vertex : built.vertices){
        if (vertex.position == Vector3(0, 0, 0)){
            EXPECT_EQ(vertex.tangentIn, Vector3(0, 0, 0));
            EXPECT_EQ(vertex.tangentOut, Vector3(2, 0, 0));
        }else if (vertex.position == Vector3(2, 3, 0)){
            EXPECT_EQ(vertex.tangentIn, Vector3(0, 3, 0));
            EXPECT_EQ(vertex.tangentOut, Vector3(0, 0, 0));
        }else{
            EXPECT_EQ(vertex.tangentIn, Vector3(2, 0, 0));
            EXPECT_EQ(vertex.tangentOut, Vector3(0, 3, 0));
        }
        // a joint's wedge centres sit where the other side's in vertex does (the inner miter point)
        if (vertex.params.z == (float)LineRibbonRole_JointCentre){
            centres++;
            bool matched = false;
            for (const LineRibbonVertex& other : built.vertices){
                if (other.params.z == (float)LineRibbonRole_In && other.position == vertex.position && other.params.x == vertex.params.x){
                    EXPECT_NEAR((placed(other) - placed(vertex)).length(), 0.0f, 1e-6f);
                    matched = true;
                }
            }
            EXPECT_TRUE(matched);
        }
    }
    EXPECT_EQ(centres, 2);
    // the end offsets use the one segment there (no NaN from the zero vector)
    for (const LineRibbonVertex& vertex : built.vertices){
        const Vector3 offset = LineRibbonBuilder::vertexOffset(vertex, Vector3(0.0f, 0.0f, 1.0f));
        EXPECT_TRUE(std::isfinite(offset.x) && std::isfinite(offset.y) && std::isfinite(offset.z));
    }
    // culling reaches the miter limit without corner vertices, one half width with them
    EXPECT_FLOAT_EQ(LineRibbonBuilder::boundsGrowth(0), 4.0f);
    EXPECT_FLOAT_EQ(LineRibbonBuilder::boundsGrowth(3), 1.0f);
}
