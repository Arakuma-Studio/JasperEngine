// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Brush Swirl's CPU layout (analysis/BrushStroke.h, docs/specs/S83-title-effects-generators.md
// §6.7.7, R15):
// - BrushStroke.StrokeCoordinatesMonotonic: on every path, each stroke's arc length starts at 0
//   and grows sample by sample, its tangents are unit vectors and its round caps close both
//   ends; the ribbon is two triangles per sample step whose u runs 0 .. 1 in order along each
//   stroke, as wide as the ink plus the rim padding.
// - BrushStroke.RevealCutsAtU: the splatter at reveal r keeps exactly the full stroke's droplets
//   thrown at u <= r, bit for bit and in order (red control: the full set has droplets past r).
// - BrushStroke.Deterministic: the same params build the same strokes; another seed other ones.
// - BrushStroke.MainStrokeAndSlash: the middle stroke is the full width and runs the whole path;
//   a slash's main stroke is about twice the radius long.

#include <gtest/gtest.h>

#include "titleart/analysis/BrushStroke.h"

#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::analysis;

namespace {

    BrushStrokeParams paramsOf(BrushPath path) {
        BrushStrokeParams p;
        p.path = path;
        p.cx = 40.0;
        p.cy = -25.0;
        return p;
    }

    bool sameDroplet(const BrushDroplet& a, const BrushDroplet& b) {
        return std::memcmp(&a, &b, sizeof(BrushDroplet)) == 0;
    }

}

TEST(BrushStroke, StrokeCoordinatesMonotonic) {
    for (BrushPath path : {BrushPath::Arc, BrushPath::Spiral, BrushPath::Slash, BrushPath::Ring}) {
        SCOPED_TRACE((int)path);
        const BrushStrokeParams p = paramsOf(path);
        const std::vector<BrushStroke> strokes = buildBrushStrokes(p);
        ASSERT_EQ(strokes.size(), 4u);
        size_t steps = 0;
        for (const BrushStroke& s : strokes) {
            const size_t M = s.size();
            ASSERT_GE(M, 16u);
            ASSERT_LE(M, 1000u);
            steps += M - 1;
            EXPECT_EQ(s.len[0], 0.0f);
            for (size_t k = 1; k < M; k++) EXPECT_GT(s.len[k], s.len[k - 1]) << "sample " << k;
            for (size_t k = 0; k < M; k++) {
                EXPECT_NEAR(std::hypot((double)s.tx[k], (double)s.ty[k]), 1.0, 1e-5);
                EXPECT_GE(s.hw[k], 0.0f);
            }
            // the round caps close both ends
            EXPECT_EQ(s.hw[0], 0.0f);
            EXPECT_EQ(s.hw[M - 1], 0.0f);
        }

        const double pad = 1.5;
        const std::vector<BrushRibbonVertex> ribbon = brushRibbon(strokes, pad);
        ASSERT_EQ(ribbon.size(), steps * 6);
        size_t at = 0;
        for (const BrushStroke& s : strokes) {
            const size_t M = s.size();
            float previous = -1.0f;
            for (size_t k = 1; k < M; k++) {
                // one step: (k-1, -), (k-1, +), (k, -), then (k-1, +), (k, +), (k, -), the signs the
                // centreline's sides
                const BrushRibbonVertex* q = &ribbon[at];
                const float u0 = (float)((double)(k - 1) / (double)(M - 1)), u1 = (float)((double)k / (double)(M - 1));
                EXPECT_EQ(q[0].u, u0);
                EXPECT_EQ(q[1].u, u0);
                EXPECT_EQ(q[2].u, u1);
                EXPECT_EQ(q[4].u, u1);
                EXPECT_GT(q[0].u, previous) << "u runs forward along the stroke";
                previous = q[0].u;
                EXPECT_LE(q[0].len, q[2].len);
                EXPECT_FLOAT_EQ(q[2].across, -(s.hw[k] + (float)pad));
                EXPECT_FLOAT_EQ(q[4].across, s.hw[k] + (float)pad);
                at += 6;
            }
            EXPECT_EQ(ribbon[at - 6].u, (float)((double)(M - 2) / (double)(M - 1)));
            EXPECT_EQ(ribbon[at - 2].u, 1.0f) << "the last step ends at u = 1";
        }
    }
}

TEST(BrushStroke, RevealCutsAtU) {
    const std::vector<BrushStroke> strokes = buildBrushStrokes(paramsOf(BrushPath::Arc));
    const std::vector<BrushDroplet> full = brushSplatter(strokes, 30.0, 1.0);
    ASSERT_EQ(full.size(), 36u) << "30 % of 120 droplets";
    for (double reveal : {0.0, 0.25, 0.5, 0.8}) {
        SCOPED_TRACE(reveal);
        const std::vector<BrushDroplet> cut = brushSplatter(strokes, 30.0, 1.0, reveal);
        std::vector<BrushDroplet> expected;
        for (const BrushDroplet& d : full) {
            if (d.u <= reveal) expected.push_back(d);
        }
        ASSERT_EQ(cut.size(), expected.size());
        for (size_t i = 0; i < cut.size(); i++) {
            EXPECT_LE(cut[i].u, reveal);
            EXPECT_TRUE(sameDroplet(cut[i], expected[i])) << "droplet " << i << " is not the full stroke's";
        }
    }
    // red control: the full stroke throws droplets past the middle (and flicks past the end)
    size_t pastMiddle = 0, flicks = 0;
    for (const BrushDroplet& d : full) {
        pastMiddle += d.u > 0.5f ? 1 : 0;
        flicks += d.u == 1.0f ? 1 : 0;
    }
    EXPECT_GT(pastMiddle, 0u);
    EXPECT_GT(flicks, 0u);
    EXPECT_EQ(brushSplatter(strokes, 30.0, 1.0, 1.0).size(), full.size());
    EXPECT_TRUE(brushSplatter(strokes, 0.0, 1.0).empty()) << "Splatter 0 throws nothing";
}

TEST(BrushStroke, Deterministic) {
    const BrushStrokeParams p = paramsOf(BrushPath::Spiral);
    const std::vector<BrushStroke> a = buildBrushStrokes(p), b = buildBrushStrokes(p);
    ASSERT_EQ(a.size(), b.size());
    for (size_t j = 0; j < a.size(); j++) {
        EXPECT_EQ(a[j].x, b[j].x);
        EXPECT_EQ(a[j].y, b[j].y);
        EXPECT_EQ(a[j].hw, b[j].hw);
        EXPECT_EQ(a[j].seed, b[j].seed);
    }
    BrushStrokeParams other = p;
    other.seed = 2.0;
    const std::vector<BrushStroke> c = buildBrushStrokes(other);
    EXPECT_NE(a[0].x, c[0].x) << "another seed wobbles otherwise";
}

TEST(BrushStroke, MainStrokeAndSlash) {
    const BrushStrokeParams p = paramsOf(BrushPath::Arc);
    const std::vector<BrushStroke> strokes = buildBrushStrokes(p);
    // floor((4 - 1) / 2) = 1: the main stroke has the full width; the others 45 .. 95 % of it
    EXPECT_EQ(strokes[1].width, p.width);
    for (size_t j : {0u, 2u, 3u}) {
        EXPECT_GE(strokes[j].width, 0.45 * p.width);
        EXPECT_LE(strokes[j].width, 0.95 * p.width);
    }
    // the main stroke spans the whole path (t 0 .. 1): about the arc's length
    EXPECT_NEAR(strokes[1].len.back(), brushPathLength(p), 0.15 * brushPathLength(p));

    BrushStrokeParams slash = paramsOf(BrushPath::Slash);
    slash.strokes = 1;
    const std::vector<BrushStroke> one = buildBrushStrokes(slash);
    ASSERT_EQ(one.size(), 1u);
    EXPECT_NEAR(one[0].len.back(), 2.0 * slash.radius, 0.05 * 2.0 * slash.radius);
}
