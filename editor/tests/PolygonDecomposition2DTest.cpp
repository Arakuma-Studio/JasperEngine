// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Concave polygon decomposition for 2D colliders (physics-2d T10): convex counter-clockwise
// pieces of at most maxVertices that cover the polygon without overlaps, welding, winding,
// self-intersection, errors, determinism and the 500-point timing.

#include <gtest/gtest.h>

#include "physics/PolygonDecomposition2D.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "object/ui/Polygon.h"

using namespace jasperengine;

namespace {

    using Polygon = std::vector<Vector2>;

    double signedArea(const Polygon& polygon) {
        double sum = 0.0;
        for (size_t i = 1; i + 1 < polygon.size(); i++) {
            const double ax = (double)polygon[i].x - polygon[0].x;
            const double ay = (double)polygon[i].y - polygon[0].y;
            const double bx = (double)polygon[i + 1].x - polygon[0].x;
            const double by = (double)polygon[i + 1].y - polygon[0].y;
            sum += ax * by - ay * bx;
        }
        return sum * 0.5;
    }

    double turn(const Vector2& a, const Vector2& b, const Vector2& c) {
        return ((double)b.x - a.x) * ((double)c.y - b.y) - ((double)b.y - a.y) * ((double)c.x - b.x);
    }

    double extent(const Polygon& polygon) {
        float minX = polygon[0].x, maxX = polygon[0].x, minY = polygon[0].y, maxY = polygon[0].y;
        for (const Vector2& p : polygon) {
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
        return std::max((double)maxX - minX, (double)maxY - minY);
    }

    // Every corner turns left (collinear allowed within a tolerance), 3..maxVertices points.
    void expectConvexPieces(const Decomposition2D& result, int maxVertices, double scale) {
        const double tolerance = 1e-9 * scale * scale;
        for (size_t i = 0; i < result.pieces.size(); i++) {
            const Polygon& piece = result.pieces[i];
            ASSERT_GE(piece.size(), 3u) << "piece " << i;
            EXPECT_LE((int)piece.size(), maxVertices) << "piece " << i;
            EXPECT_GT(signedArea(piece), 0.0) << "piece " << i << " is not counter-clockwise";
            const size_t n = piece.size();
            for (size_t k = 0; k < n; k++) {
                EXPECT_GE(turn(piece[(k + n - 1) % n], piece[k], piece[(k + 1) % n]), -tolerance)
                    << "piece " << i << " corner " << k << " turns right";
            }
        }
    }

    // Projections of two convex pieces on the edge normals of both: separated (touching along
    // an edge or at a corner counts as separated) when one axis shows a gap above -tolerance.
    bool separated(const Polygon& a, const Polygon& b, double tolerance) {
        for (int pass = 0; pass < 2; pass++) {
            const Polygon& edges = pass == 0 ? a : b;
            const size_t n = edges.size();
            for (size_t i = 0; i < n; i++) {
                const double nx = (double)edges[(i + 1) % n].y - edges[i].y;
                const double ny = -((double)edges[(i + 1) % n].x - edges[i].x);
                const double length = std::sqrt(nx * nx + ny * ny);
                if (length == 0.0) {
                    continue;
                }
                double minA = 1e300, maxA = -1e300, minB = 1e300, maxB = -1e300;
                for (const Vector2& p : a) {
                    const double d = (p.x * nx + p.y * ny) / length;
                    minA = std::min(minA, d);
                    maxA = std::max(maxA, d);
                }
                for (const Vector2& p : b) {
                    const double d = (p.x * nx + p.y * ny) / length;
                    minB = std::min(minB, d);
                    maxB = std::max(maxB, d);
                }
                if (maxA <= minB + tolerance || maxB <= minA + tolerance) {
                    return true;
                }
            }
        }
        return false;
    }

    void expectNoOverlaps(const Decomposition2D& result, double scale) {
        const double tolerance = 1e-5 * scale;
        for (size_t i = 0; i < result.pieces.size(); i++) {
            for (size_t j = i + 1; j < result.pieces.size(); j++) {
                EXPECT_TRUE(separated(result.pieces[i], result.pieces[j], tolerance))
                    << "pieces " << i << " and " << j << " overlap";
            }
        }
    }

    double piecesArea(const Decomposition2D& result) {
        double area = 0.0;
        for (const Polygon& piece : result.pieces) {
            area += signedArea(piece);
        }
        return area;
    }

    // Convex, at most maxVertices, no overlaps and the polygon's area within 0.1%.
    void expectValidDecomposition(const Polygon& polygon, const Decomposition2D& result, int maxVertices) {
        ASSERT_TRUE(result.error.empty()) << result.error;
        ASSERT_FALSE(result.pieces.empty());
        EXPECT_FALSE(result.selfIntersecting);
        const double scale = extent(polygon);
        expectConvexPieces(result, maxVertices, scale);
        expectNoOverlaps(result, scale);
        const double area = std::fabs(signedArea(polygon));
        EXPECT_NEAR(piecesArea(result), area, area * 0.001);
    }

    void expectIdentical(const Decomposition2D& a, const Decomposition2D& b) {
        ASSERT_EQ(a.pieces.size(), b.pieces.size());
        EXPECT_EQ(a.convexInput, b.convexInput);
        EXPECT_EQ(a.selfIntersecting, b.selfIntersecting);
        EXPECT_EQ(a.droppedPieces, b.droppedPieces);
        EXPECT_EQ(a.error, b.error);
        for (size_t i = 0; i < a.pieces.size(); i++) {
            ASSERT_EQ(a.pieces[i].size(), b.pieces[i].size()) << "piece " << i;
            for (size_t k = 0; k < a.pieces[i].size(); k++) {
                EXPECT_EQ(a.pieces[i][k].x, b.pieces[i][k].x) << "piece " << i << " point " << k;
                EXPECT_EQ(a.pieces[i][k].y, b.pieces[i][k].y) << "piece " << i << " point " << k;
            }
        }
    }

    Polygon square() {
        return {Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 1)};
    }

    Polygon lShape() {
        return {Vector2(0, 0), Vector2(2, 0), Vector2(2, 1), Vector2(1, 1), Vector2(1, 2), Vector2(0, 2)};
    }

    // A base 0..17 x 0..1 with 9 teeth of width 1 up to y = 5, gaps of width 1.
    Polygon comb() {
        Polygon polygon = {Vector2(0, 0), Vector2(17, 0)};
        for (int k = 8; k >= 0; k--) {
            polygon.push_back(Vector2((float)(2 * k + 1), 5));
            polygon.push_back(Vector2((float)(2 * k), 5));
            if (k > 0) {
                polygon.push_back(Vector2((float)(2 * k), 1));
                polygon.push_back(Vector2((float)(2 * k - 1), 1));
            }
        }
        return polygon;
    }

    Polygon regularPolygon(int count, float radius) {
        Polygon polygon;
        for (int i = 0; i < count; i++) {
            const double angle = 2.0 * 3.14159265358979323846 * i / count;
            polygon.push_back(Vector2((float)(radius * std::cos(angle)), (float)(radius * std::sin(angle))));
        }
        return polygon;
    }

    // A star of 250 spikes, radii varied by an integer generator.
    Polygon star500() {
        Polygon polygon;
        uint32_t state = 12345u;
        for (int i = 0; i < 500; i++) {
            state = state * 1664525u + 1013904223u;
            const double jitter = (double)(state >> 8) / (double)(1u << 24);
            const double radius = (i % 2 == 0 ? 100.0 : 40.0) + 10.0 * jitter;
            const double angle = 2.0 * 3.14159265358979323846 * i / 500;
            polygon.push_back(Vector2((float)(radius * std::cos(angle)), (float)(radius * std::sin(angle))));
        }
        return polygon;
    }

    // A noisy terrain strip: 498 points along the top, two bottom corners.
    Polygon terrain500() {
        Polygon polygon;
        uint32_t state = 777u;
        for (int i = 0; i < 498; i++) {
            state = state * 1664525u + 1013904223u;
            const float noise = (float)(state >> 8) / (float)(1u << 24);
            polygon.push_back(Vector2((float)(497 - i) * 2.0f, 20.0f + 15.0f * noise));
        }
        polygon.push_back(Vector2(0, 0));
        polygon.push_back(Vector2(994, 0));
        return polygon;
    }

    Polygon loadTerrain226() {
        const std::string path = std::string(JASPER_EDITOR_TEST_FIXTURES) + "/physics2d/terrain_226.txt";
        std::ifstream file(path);
        Polygon polygon;
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            std::istringstream values(line);
            float x = 0.0f, y = 0.0f;
            if (values >> x >> y) {
                polygon.push_back(Vector2(x, y));
            }
        }
        return polygon;
    }

    // Best of a few runs, in milliseconds.
    double decompositionMilliseconds(const Polygon& polygon, float weld) {
        double best = 1e9;
        for (int run = 0; run < 5; run++) {
            const auto start = std::chrono::steady_clock::now();
            const Decomposition2D result = decomposePolygon2D(polygon, weld);
            const auto end = std::chrono::steady_clock::now();
            EXPECT_FALSE(result.pieces.empty());
            best = std::min(best, std::chrono::duration<double, std::milli>(end - start).count());
        }
        return best;
    }

}

TEST(PolygonDecomposition2D, SquareIsOneConvexPiece) {
    const Decomposition2D result = decomposePolygon2D(square(), 0.005f);
    ASSERT_TRUE(result.error.empty()) << result.error;
    ASSERT_EQ(result.pieces.size(), 1u);
    EXPECT_TRUE(result.convexInput);
    EXPECT_FALSE(result.selfIntersecting);
    EXPECT_EQ(result.droppedPieces, 0);
    EXPECT_EQ(result.pieces[0].size(), 4u);
    expectValidDecomposition(square(), result, 8);
}

TEST(PolygonDecomposition2D, LShapeIsTwoPieces) {
    const Decomposition2D result = decomposePolygon2D(lShape(), 0.005f);
    EXPECT_FALSE(result.convexInput);
    EXPECT_EQ(result.pieces.size(), 2u);
    expectValidDecomposition(lShape(), result, 8);
}

TEST(PolygonDecomposition2D, CombWithNineTeeth) {
    const Polygon polygon = comb();
    const Decomposition2D result = decomposePolygon2D(polygon, 0.005f);
    EXPECT_GE(result.pieces.size(), 9u);
    expectValidDecomposition(polygon, result, 8);
}

TEST(PolygonDecomposition2D, Terrain226Fixture) {
    const Polygon polygon = loadTerrain226();
    ASSERT_EQ(polygon.size(), 226u);
    const Decomposition2D result = decomposePolygon2D(polygon, 0.005f);
    EXPECT_FALSE(result.convexInput);
    EXPECT_EQ(result.droppedPieces, 0);
    expectValidDecomposition(polygon, result, 8);
    std::printf("terrain_226: %d pieces\n", (int)result.pieces.size());
}

TEST(PolygonDecomposition2D, ConvexInputAboveMaxVerticesIsSplit) {
    const Polygon polygon = regularPolygon(20, 10.0f);
    const Decomposition2D result = decomposePolygon2D(polygon, 0.005f);
    EXPECT_TRUE(result.convexInput);
    EXPECT_GE(result.pieces.size(), 3u);
    expectValidDecomposition(polygon, result, 8);
}

TEST(PolygonDecomposition2D, DuplicateAndCollinearPointsAreWelded) {
    const Polygon polygon = {Vector2(0, 0), Vector2(0.5f, 0), Vector2(1, 0), Vector2(1, 0), Vector2(1.001f, 0.0005f),
                             Vector2(1, 1), Vector2(0, 1), Vector2(0, 0.0001f)};
    const Decomposition2D result = decomposePolygon2D(polygon, 0.005f);
    ASSERT_TRUE(result.error.empty()) << result.error;
    ASSERT_EQ(result.pieces.size(), 1u);
    EXPECT_TRUE(result.convexInput);
    const Polygon& piece = result.pieces[0];
    ASSERT_EQ(piece.size(), 4u);
    for (const Vector2& corner : {Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 1)}) {
        EXPECT_NE(std::find(piece.begin(), piece.end(), corner), piece.end()) << corner.toString();
    }
    EXPECT_NEAR(piecesArea(result), 1.0, 1e-6);
}

TEST(PolygonDecomposition2D, ClockwiseInputGivesCounterClockwisePieces) {
    Polygon polygon = lShape();
    std::reverse(polygon.begin(), polygon.end());
    ASSERT_LT(signedArea(polygon), 0.0);
    const Decomposition2D result = decomposePolygon2D(polygon, 0.005f);
    EXPECT_EQ(result.pieces.size(), 2u);
    expectValidDecomposition(polygon, result, 8);

    Polygon clockwiseSquare = square();
    std::reverse(clockwiseSquare.begin(), clockwiseSquare.end());
    const Decomposition2D squareResult = decomposePolygon2D(clockwiseSquare, 0.005f);
    ASSERT_EQ(squareResult.pieces.size(), 1u);
    EXPECT_GT(signedArea(squareResult.pieces[0]), 0.0);
}

TEST(PolygonDecomposition2D, BowTieGivesTheHull) {
    const Polygon bowTie = {Vector2(0, 0), Vector2(2, 2), Vector2(2, 0), Vector2(0, 2)};
    const Decomposition2D result = decomposePolygon2D(bowTie, 0.005f);
    EXPECT_TRUE(result.selfIntersecting);
    EXPECT_TRUE(result.error.empty()) << result.error;
    ASSERT_EQ(result.pieces.size(), 1u);
    const Polygon& hull = result.pieces[0];
    ASSERT_EQ(hull.size(), 4u);
    for (const Vector2& corner : bowTie) {
        EXPECT_NE(std::find(hull.begin(), hull.end(), corner), hull.end()) << corner.toString();
    }
    EXPECT_NEAR(signedArea(hull), 4.0, 1e-9);

    // a pentagram (5 crossing edges): its hull, 5 points
    Polygon pentagram;
    for (int i = 0; i < 5; i++) {
        const double angle = 2.0 * 3.14159265358979323846 * (2 * i) / 5;
        pentagram.push_back(Vector2((float)(10 * std::cos(angle)), (float)(10 * std::sin(angle))));
    }
    const Decomposition2D star = decomposePolygon2D(pentagram, 0.005f);
    EXPECT_TRUE(star.selfIntersecting);
    ASSERT_EQ(star.pieces.size(), 1u);
    EXPECT_EQ(star.pieces[0].size(), 5u);
    expectConvexPieces(star, 8, 20.0);
}

TEST(PolygonDecomposition2D, SelfIntersectingHullIsSplitByMaxVertices) {
    // a 12-point outline with one crossing: its hull (12 points) in fans of at most 4
    Polygon polygon = regularPolygon(12, 10.0f);
    std::swap(polygon[3], polygon[4]);
    const Decomposition2D result = decomposePolygon2D(polygon, 0.005f, 4);
    EXPECT_TRUE(result.selfIntersecting);
    EXPECT_GE(result.pieces.size(), 5u);
    expectConvexPieces(result, 4, 20.0);
    expectNoOverlaps(result, 20.0);
    EXPECT_NEAR(piecesArea(result), std::fabs(signedArea(regularPolygon(12, 10.0f))), 1e-3);
}

TEST(PolygonDecomposition2D, TooFewPointsOrZeroAreaIsAnError) {
    const Decomposition2D two = decomposePolygon2D({Vector2(0, 0), Vector2(1, 0)}, 0.005f);
    EXPECT_FALSE(two.error.empty());
    EXPECT_TRUE(two.pieces.empty());

    const Decomposition2D welded = decomposePolygon2D({Vector2(0, 0), Vector2(0.001f, 0), Vector2(0, 0.001f)}, 0.005f);
    EXPECT_FALSE(welded.error.empty());
    EXPECT_TRUE(welded.pieces.empty());

    const Decomposition2D collinear =
        decomposePolygon2D({Vector2(0, 0), Vector2(1, 1), Vector2(2, 2), Vector2(3, 3), Vector2(1.5f, 1.5f)}, 0.005f);
    EXPECT_FALSE(collinear.error.empty());
    EXPECT_TRUE(collinear.pieces.empty());

    const Decomposition2D empty = decomposePolygon2D({}, 0.005f);
    EXPECT_FALSE(empty.error.empty());
    EXPECT_TRUE(empty.pieces.empty());
}

TEST(PolygonDecomposition2D, SameInputGivesIdenticalOutput) {
    const Polygon terrain = loadTerrain226();
    ASSERT_EQ(terrain.size(), 226u);
    expectIdentical(decomposePolygon2D(terrain, 0.005f), decomposePolygon2D(terrain, 0.005f));
    expectIdentical(decomposePolygon2D(comb(), 0.005f), decomposePolygon2D(comb(), 0.005f));
    expectIdentical(decomposePolygon2D(star500(), 0.005f, 5), decomposePolygon2D(star500(), 0.005f, 5));
}

TEST(PolygonDecomposition2D, FiveHundredPointsAreValidAndFast) {
    const Polygon star = star500();
    const Polygon terrain = terrain500();
    ASSERT_EQ(star.size(), 500u);
    ASSERT_EQ(terrain.size(), 500u);
    expectValidDecomposition(star, decomposePolygon2D(star, 0.005f), 8);
    expectValidDecomposition(terrain, decomposePolygon2D(terrain, 0.005f), 8);

    const double starMs = decompositionMilliseconds(star, 0.005f);
    const double terrainMs = decompositionMilliseconds(terrain, 0.005f);
    std::printf("500 points: star %.3f ms, terrain %.3f ms\n", starMs, terrainMs);
#ifdef NDEBUG
    EXPECT_LT(starMs, 2.0);
    EXPECT_LT(terrainMs, 2.0);
#endif
}

TEST(PolygonDecomposition2D, MaxVerticesIsRespected) {
    const Polygon terrain = loadTerrain226();
    ASSERT_EQ(terrain.size(), 226u);
    const Decomposition2D four = decomposePolygon2D(terrain, 0.005f, 4);
    expectValidDecomposition(terrain, four, 4);
    const Decomposition2D eight = decomposePolygon2D(terrain, 0.005f, 8);
    EXPECT_GE(four.pieces.size(), eight.pieces.size());

    expectValidDecomposition(comb(), decomposePolygon2D(comb(), 0.005f, 4), 4);

    // a convex octagon with maxVertices 4: split, still marked convex
    const Polygon octagon = regularPolygon(8, 5.0f);
    const Decomposition2D split = decomposePolygon2D(octagon, 0.005f, 4);
    EXPECT_TRUE(split.convexInput);
    EXPECT_GE(split.pieces.size(), 3u);
    expectValidDecomposition(octagon, split, 4);

    // below 3 is taken as 3: triangles
    const Decomposition2D triangles = decomposePolygon2D(lShape(), 0.005f, 1);
    EXPECT_EQ(triangles.pieces.size(), 4u);
    expectValidDecomposition(lShape(), triangles, 3);
}

// ---- fidelity on large outlines (review): straight-on removal is local and absolute ----------

namespace {

    // distance from v to the convex counter-clockwise piece (0 inside or on it)
    double distanceToPiece(const Vector2& v, const Polygon& piece) {
        bool inside = true;
        double best = 1e300;
        for (size_t i = 0; i < piece.size(); i++) {
            const Vector2& a = piece[i];
            const Vector2& b = piece[(i + 1) % piece.size()];
            const double ex = (double)b.x - a.x, ey = (double)b.y - a.y;
            const double px = (double)v.x - a.x, py = (double)v.y - a.y;
            if (ex * py - ey * px < -1e-9 * std::sqrt(ex * ex + ey * ey)) inside = false;
            const double len2 = ex * ex + ey * ey;
            const double t = len2 > 0.0 ? std::min(1.0, std::max(0.0, (px * ex + py * ey) / len2)) : 0.0;
            const double dx = px - t * ex, dy = py - t * ey;
            best = std::min(best, std::sqrt(dx * dx + dy * dy));
        }
        return inside ? 0.0 : best;
    }

    // every input point lies on the union of the pieces (within `tolerance`) and the union has
    // the input's area: the pieces are the outline, not a simplification of it
    void expectFaithful(const Polygon& input, const Decomposition2D& result, double tolerance, const char* what) {
        ASSERT_TRUE(result.error.empty()) << what << ": " << result.error;
        double maxDeviation = 0.0;
        for (const Vector2& v : input) {
            double d = 1e300;
            for (const Polygon& piece : result.pieces) d = std::min(d, distanceToPiece(v, piece));
            maxDeviation = std::max(maxDeviation, d);
        }
        EXPECT_LE(maxDeviation, tolerance) << what;
        // straight-on points within the tolerance may go: at most a band of it along the outline
        double perimeter = 0.0;
        for (size_t i = 0; i < input.size(); i++) perimeter += (input[(i + 1) % input.size()] - input[i]).length();
        double area = 0.0;
        for (const Polygon& piece : result.pieces) area += signedArea(piece);
        EXPECT_NEAR(area, std::fabs(signedArea(input)), perimeter * tolerance) << what;
    }

}

TEST(PolygonDecomposition2D, LargeOutlinesKeepTheirDetail) {
    // a 3000 m terrain with 1 m hills every 20 m (points each metre), in metres as the body has them
    Polygon terrain;
    for (int i = 0; i <= 3000; i++) terrain.push_back(Vector2((float)i, (float)(10.0 + std::sin(i * 2.0 * M_PI / 20.0))));
    terrain.push_back(Vector2(3000.0f, -5.0f));
    terrain.push_back(Vector2(0.0f, -5.0f));
    const Decomposition2D hills = decomposePolygon2D(terrain, 0.005f, 8);
    EXPECT_GT(hills.pieces.size(), 100u);
    expectFaithful(terrain, hills, 0.006, "terrain");

    // a comb of 2000 teeth
    Polygon comb;
    for (int i = 0; i < 2000; i++) {
        const float x = i * 2.0f;
        comb.push_back(Vector2(x, 0));
        comb.push_back(Vector2(x + 1, 0));
        comb.push_back(Vector2(x + 1, -10));
        comb.push_back(Vector2(x + 2, -10));
    }
    comb.push_back(Vector2(4000.0f, 0));
    comb.push_back(Vector2(4000.0f, 5));
    comb.push_back(Vector2(0, 5));
    const Decomposition2D teeth = decomposePolygon2D(comb, 0.005f, 8);
    EXPECT_GE(teeth.pieces.size(), 2000u);
    expectFaithful(comb, teeth, 0.006, "comb");

    // points on a gentle arc: removing them one after another must not drift off it
    Polygon arc;
    for (int i = 0; i <= 2000; i++) {
        const double a = M_PI * i / 2000.0;
        arc.push_back(Vector2((float)(500.0 * std::cos(a)), (float)(500.0 * std::sin(a))));
    }
    const Decomposition2D half = decomposePolygon2D(arc, 0.005f, 8);
    expectFaithful(arc, half, 0.006, "arc");
}
