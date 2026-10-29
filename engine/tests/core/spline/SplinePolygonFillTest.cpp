// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The ear clipper and the flat fill (S18 R7, D10, AC8): concave shapes, either orientation,
// refusals of self-intersecting and self-touching outlines, and fill meshes whose triangle
// normals agree with their vertex normals (front faces counter-clockwise, D6).

#include "SplineTestCommon.h"

#include "spline/SplinePolygonFill.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::spline;
using namespace jasperengine::test;

namespace{

    double fillPolygonArea(const std::vector<Vector2>& p){
        double a = 0.0;
        for (size_t i = 0; i < p.size(); i++){
            const Vector2& u = p[i];
            const Vector2& v = p[(i + 1) % p.size()];
            a += double(u.x) * double(v.y) - double(v.x) * double(u.y);
        }
        return 0.5 * a;
    }

    double fillTriangleArea(const std::vector<Vector2>& p, uint32_t a, uint32_t b, uint32_t c){
        return 0.5 * ((double(p[b].x) - p[a].x) * (double(p[c].y) - p[a].y) - (double(p[b].y) - p[a].y) * (double(p[c].x) - p[a].x));
    }

    // every triangle counter-clockwise; returns their total area
    double fillCheckTriangles(const std::vector<Vector2>& p, const std::vector<uint32_t>& triangles){
        EXPECT_EQ(triangles.size() % 3, 0u);
        double total = 0.0;
        for (size_t t = 0; t + 2 < triangles.size(); t += 3){
            EXPECT_LT(triangles[t], p.size());
            EXPECT_LT(triangles[t + 1], p.size());
            EXPECT_LT(triangles[t + 2], p.size());
            const double area = fillTriangleArea(p, triangles[t], triangles[t + 1], triangles[t + 2]);
            EXPECT_GT(area, 0.0) << "triangle " << t / 3;
            total += area;
        }
        return total;
    }

    std::vector<Vector2> fillStar(uint32_t points, float outer, float inner){
        std::vector<Vector2> out;
        for (uint32_t i = 0; i < 2 * points; i++){
            const float a = SPLINE_TEST_PI * float(i) / float(points);
            const float r = i % 2 == 0 ? outer : inner;
            out.push_back(Vector2(r * std::cos(a), r * std::sin(a)));
        }
        return out;
    }

    // every triangle's geometric normal (b - a) x (c - a) agrees with its vertices' normals
    void fillCheckMesh(const std::vector<SplineVertex>& vertices, const std::vector<uint32_t>& indices){
        ASSERT_EQ(indices.size() % 3, 0u);
        ASSERT_FALSE(indices.empty());
        for (size_t t = 0; t < indices.size(); t += 3){
            const SplineVertex& a = vertices[indices[t]];
            const SplineVertex& b = vertices[indices[t + 1]];
            const SplineVertex& c = vertices[indices[t + 2]];
            const Vector3 n = splineTestCross(b.position - a.position, c.position - a.position);
            EXPECT_GT(splineTestDot(n, a.normal), 0.0f) << "triangle " << t / 3;
            EXPECT_GT(splineTestDot(n, b.normal), 0.0f);
            EXPECT_GT(splineTestDot(n, c.normal), 0.0f);
        }
    }

    float fillMeshArea(const std::vector<SplineVertex>& vertices, const std::vector<uint32_t>& indices){
        double total = 0.0;
        for (size_t t = 0; t < indices.size(); t += 3){
            const Vector3 n = splineTestCross(vertices[indices[t + 1]].position - vertices[indices[t]].position,
                                              vertices[indices[t + 2]].position - vertices[indices[t]].position);
            total += 0.5 * double(splineTestLength(n));
        }
        return float(total);
    }

}

TEST(SplinePolygonFillTests, ConcaveTwelveGonGivesTenTrianglesOfItsArea){
    const std::vector<std::vector<Vector2>> shapes = {
        fillStar(6, 10.0f, 4.0f),
        // a comb: three teeth
        {Vector2(0, 0), Vector2(10, 0), Vector2(10, 6), Vector2(8, 6), Vector2(8, 2), Vector2(6, 2),
         Vector2(6, 6), Vector2(4, 6), Vector2(4, 2), Vector2(2, 2), Vector2(2, 6), Vector2(0, 6)},
    };
    for (std::vector<Vector2> shape : shapes){
        ASSERT_EQ(shape.size(), 12u);
        for (bool reversed : {false, true}){
            if (reversed){
                std::reverse(shape.begin(), shape.end());
            }
            std::vector<uint32_t> triangles;
            SplineFillIssue issue;
            ASSERT_TRUE(triangulatePolygon(shape, triangles, &issue));
            EXPECT_EQ(issue.error, SplineFillError::None);
            EXPECT_EQ(triangles.size(), 3u * 10u);
            EXPECT_NEAR(fillCheckTriangles(shape, triangles), std::fabs(fillPolygonArea(shape)), 1e-4);
        }
    }
}

TEST(SplinePolygonFillTests, SelfIntersectingInputIsRefused){
    const std::vector<Vector2> bowtie = {Vector2(0, 0), Vector2(4, 4), Vector2(4, 0), Vector2(0, 5)};
    std::vector<uint32_t> triangles = {1, 2, 3};
    SplineFillIssue issue;
    EXPECT_FALSE(triangulatePolygon(bowtie, triangles, &issue));
    EXPECT_EQ(issue.error, SplineFillError::SelfIntersecting);
    EXPECT_EQ(issue.edgeA, 0u);
    EXPECT_EQ(issue.edgeB, 2u);
    EXPECT_TRUE(triangles.empty());
    EXPECT_STRNE(fillErrorReason(issue.error), "");
}

TEST(SplinePolygonFillTests, SelfTouchingInputIsRefused){
    const std::vector<std::vector<Vector2>> shapes = {
        // a vertex on another edge
        {Vector2(0, 0), Vector2(4, 0), Vector2(4, 4), Vector2(2, 0.0f), Vector2(0, 4)},
        // two squares meeting at a corner (the corner visited twice)
        {Vector2(0, 0), Vector2(2, 0), Vector2(2, 2), Vector2(4, 2), Vector2(4, 4), Vector2(2, 4), Vector2(2, 2), Vector2(0, 2)},
        // an edge folding back over the one before it
        {Vector2(0, 0), Vector2(4, 0), Vector2(4, 4), Vector2(4, 2), Vector2(0, 4)},
    };
    for (const std::vector<Vector2>& shape : shapes){
        std::vector<uint32_t> triangles;
        SplineFillIssue issue;
        EXPECT_FALSE(triangulatePolygon(shape, triangles, &issue));
        EXPECT_EQ(issue.error, SplineFillError::SelfIntersecting);
    }
}

TEST(SplinePolygonFillTests, DegenerateInputIsRefused){
    std::vector<uint32_t> triangles;
    SplineFillIssue issue;
    const std::vector<Vector2> two = {Vector2(0, 0), Vector2(1, 0), Vector2(1, 0), Vector2(0, 0)};
    EXPECT_FALSE(triangulatePolygon(two, triangles, &issue));
    EXPECT_EQ(issue.error, SplineFillError::TooFewPoints);
    const std::vector<Vector2> line = {Vector2(0, 0), Vector2(1, 1), Vector2(3, 3), Vector2(2, 2)};
    EXPECT_FALSE(triangulatePolygon(line, triangles, &issue));
    EXPECT_EQ(issue.error, SplineFillError::ZeroArea);
}

TEST(SplinePolygonFillTests, StraightRunsAddNoSlivers){
    // a square with points along its sides and a closing repeat
    const std::vector<Vector2> square = {Vector2(0, 0), Vector2(2, 0), Vector2(4, 0), Vector2(4, 1), Vector2(4, 4),
                                         Vector2(2, 4), Vector2(0, 4), Vector2(0, 2), Vector2(0, 0)};
    std::vector<uint32_t> triangles;
    ASSERT_TRUE(triangulatePolygon(square, triangles));
    EXPECT_EQ(triangles.size(), 6u);
    EXPECT_NEAR(fillCheckTriangles(square, triangles), 16.0, 1e-9);
}

TEST(SplinePolygonFillTests, ManyPointsTriangulateDeterministically){
    std::vector<Vector2> circle;
    for (int i = 0; i < 1000; i++){
        const float a = 2.0f * SPLINE_TEST_PI * float(i) / 1000.0f;
        const float r = 10.0f + 3.0f * std::sin(5.0f * a);
        circle.push_back(Vector2(r * std::cos(a), r * std::sin(a)));
    }
    std::vector<uint32_t> first;
    std::vector<uint32_t> second;
    ASSERT_TRUE(triangulatePolygon(circle, first));
    ASSERT_TRUE(triangulatePolygon(circle, second));
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.size(), 3u * 998u);
    EXPECT_NEAR(fillCheckTriangles(circle, first), fillPolygonArea(circle), 1e-3);
}

TEST(SplinePolygonFillTests, FillMeshFacesUpInXZ){
    for (bool reversed : {false, true}){
        std::vector<Vector3> knots = circleKnots(10, 6.0f, 3.0f);
        if (reversed){
            std::reverse(knots.begin(), knots.end());
        }
        const SplineData s = makeSpline(SplineType::CatmullRom, knots, true);
        std::vector<Vector3> outline;
        sampleFillOutline(s, 0.5f, outline);
        ASSERT_GE(outline.size(), 70u);
        EXPECT_GT(splineTestLength(outline.back() - outline.front()), 0.1f) << "no closing repeat";
        EXPECT_NEAR(meanKnotHeight(s), 3.0f, 1e-6f);

        SplineFillOptions options;
        options.height = meanKnotHeight(s) + 0.5f;
        options.tilingU = 0.5f;
        options.tilingV = 2.0f;
        std::vector<SplineVertex> vertices;
        std::vector<uint32_t> indices;
        SplineFillIssue issue;
        ASSERT_TRUE(buildPolygonFill(outline, options, vertices, indices, &issue));
        ASSERT_EQ(vertices.size(), outline.size());
        fillCheckMesh(vertices, indices);
        for (size_t i = 0; i < vertices.size(); i++){
            EXPECT_EQ(vertices[i].position.y, 3.5f);
            EXPECT_EQ(vertices[i].position.x, outline[i].x);
            EXPECT_EQ(vertices[i].position.z, outline[i].z);
            expectNearVector(vertices[i].normal, Vector3(0, 1, 0), 0.0f);
            // world units x tiling, V along -Z
            EXPECT_EQ(vertices[i].uv.x, outline[i].x * 0.5f);
            EXPECT_EQ(vertices[i].uv.y, -outline[i].z * 2.0f);
            EXPECT_EQ(vertices[i].tangent.x, 1.0f);
            EXPECT_EQ(vertices[i].tangent.w, 1.0f);
        }
        EXPECT_NEAR(fillMeshArea(vertices, indices), SPLINE_TEST_PI * 36.0f, 0.01f * SPLINE_TEST_PI * 36.0f);
    }
}

TEST(SplinePolygonFillTests, FillMeshFacesTheCameraInXY){
    SplineData s = makeSpline(SplineType::CatmullRom, {Vector3(0, 0, 0), Vector3(6, 1, 0), Vector3(8, 6, 0), Vector3(4, 3, 0), Vector3(0, 6, 0)}, true);
    s.setUpAxis(Vector3(0, 0, 1));
    std::vector<Vector3> outline;
    sampleFillOutline(s, 0.25f, outline);
    SplineFillOptions options;
    options.plane = SplineFillPlane::XY;
    std::vector<SplineVertex> vertices;
    std::vector<uint32_t> indices;
    ASSERT_TRUE(buildPolygonFill(outline, options, vertices, indices));
    fillCheckMesh(vertices, indices);
    for (size_t i = 0; i < vertices.size(); i++){
        EXPECT_EQ(vertices[i].position.z, 0.0f);
        expectNearVector(vertices[i].normal, Vector3(0, 0, 1), 0.0f);
        EXPECT_EQ(vertices[i].uv.x, outline[i].x);
        EXPECT_EQ(vertices[i].uv.y, outline[i].y);
    }
}

TEST(SplinePolygonFillTests, ASelfCrossingSplineIsRefused){
    // a figure eight
    const SplineData s = makeSpline(SplineType::CatmullRom,
        {Vector3(0, 0, 0), Vector3(5, 0, 5), Vector3(10, 0, 0), Vector3(5, 0, -5), Vector3(0, 0, 0.5f), Vector3(-5, 0, 5), Vector3(-10, 0, 0), Vector3(-5, 0, -5)}, true);
    std::vector<Vector3> outline;
    sampleFillOutline(s, 0.5f, outline);
    std::vector<SplineVertex> vertices = {SplineVertex()};
    std::vector<uint32_t> indices = {0, 0, 0};
    SplineFillIssue issue;
    EXPECT_FALSE(buildPolygonFill(outline, SplineFillOptions(), vertices, indices, &issue));
    EXPECT_EQ(issue.error, SplineFillError::SelfIntersecting);
    EXPECT_TRUE(vertices.empty());
    EXPECT_TRUE(indices.empty());
}

TEST(SplinePolygonFillTests, OutlinesOfInvalidSplinesAreEmpty){
    std::vector<Vector3> outline = {Vector3(1, 1, 1)};
    sampleFillOutline(SplineData(), 1.0f, outline);
    EXPECT_TRUE(outline.empty());
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 0, 1)}, true);
    sampleFillOutline(s, 0.0f, outline);
    EXPECT_TRUE(outline.empty());
    // a tiny loop still has three points
    sampleFillOutline(s, 100.0f, outline);
    EXPECT_EQ(outline.size(), 3u);
}
