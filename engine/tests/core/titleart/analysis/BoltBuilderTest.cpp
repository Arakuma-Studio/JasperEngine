// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lightning's CPU layout (analysis/BoltBuilder.h, docs/specs/S83-title-effects-generators.md
// §6.7.14, R10, R15) on synthetic coverage grids:
// - BoltBuilder.Deterministic: the same edges and params give the same segments; another seed
//   other ones.
// - BoltBuilder.MidpointDisplacement: a bolt of depth 6 is 64 segments from a to b, its points
//   within the summed displacement amplitudes of the chord, its widths tapering 1 .. 0.5.
// - BoltBuilder.BranchesInRange: with every branch taken, each of the 60 inner segments forks
//   one 16-segment branch (depth 4) at intensity x 0.7 and width x 0.55, whose chord is 0.25 to
//   0.55 of the bolt's times (1 - i/n); with branching off there are none.
// - BoltBuilder.TwelveBoltsLayout: twelve outward bolts on a filled box each start on an edge
//   cell, end outside the box at 0.55 .. 1 x the length, and land on the same canvas spots at
//   px_scale 1 and 2.
// - BoltBuilder.EdgeCells: the edge cells of a box are its border with outward normals.

#include <gtest/gtest.h>

#include "titleart/analysis/BoltBuilder.h"

#include <cmath>
#include <set>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::analysis;

namespace {

    // a w x h coverage grid with the cells of [x0, x1) x [y0, y1) full (row 0 at the bottom)
    std::vector<float> boxGrid(int w, int h, int x0, int y0, int x1, int y1) {
        std::vector<float> alpha((size_t)w * h, 0.0f);
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) alpha[(size_t)y * w + x] = 1.0f;
        }
        return alpha;
    }

    bool sameSegments(const std::vector<BoltSegment>& a, const std::vector<BoltSegment>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (a[i].ax != b[i].ax || a[i].ay != b[i].ay || a[i].bx != b[i].bx || a[i].by != b[i].by || a[i].width != b[i].width ||
                a[i].intensity != b[i].intensity) {
                return false;
            }
        }
        return true;
    }

    // a 64 x 48 grid of 6 px cells with a 24 x 16 box of ink in the middle, at px_scale `scale`
    struct BoxFixture {
        static constexpr int W = 64, H = 48;
        static constexpr int X0 = 20, Y0 = 16, X1 = 44, Y1 = 32;
        double scale = 1.0;
        double cell() const { return BOLT_EDGE_CELL * scale; }
        std::vector<BoltEdge> edges() const { return findBoltEdges(boxGrid(W, H, X0, Y0, X1, Y1), W, H, 0.0, 0.0, cell()); }
        bool insideBox(double x, double y) const {
            return x > X0 * cell() && x < X1 * cell() && y > Y0 * cell() && y < Y1 * cell();
        }
    };

}

TEST(BoltBuilder, EdgeCells) {
    const BoxFixture box;
    const std::vector<BoltEdge> edges = box.edges();
    // the box's border cells: 2 x 24 + 2 x 14
    EXPECT_EQ(edges.size(), (size_t)(2 * 24 + 2 * 14));
    for (const BoltEdge& e : edges) {
        EXPECT_NEAR(std::hypot(e.nx, e.ny), 1.0, 1e-9);
        // the normal points away from the box's centre
        const double cx = (BoxFixture::X0 + BoxFixture::X1) * 0.5 * box.cell(), cy = (BoxFixture::Y0 + BoxFixture::Y1) * 0.5 * box.cell();
        EXPECT_GT((e.x - cx) * e.nx + (e.y - cy) * e.ny, 0.0) << "at " << e.x << ", " << e.y;
    }
    // an empty grid has no edge; a full one neither (cells past the grid read as their neighbours)
    EXPECT_TRUE(findBoltEdges(std::vector<float>(16, 0.0f), 4, 4, 0.0, 0.0, 6.0).empty());
    EXPECT_TRUE(findBoltEdges(std::vector<float>(16, 1.0f), 4, 4, 0.0, 0.0, 6.0).empty());
}

TEST(BoltBuilder, Deterministic) {
    const std::vector<BoltEdge> edges = BoxFixture().edges();
    for (BoltMode mode : {BoltMode::Mixed, BoltMode::Outward, BoltMode::Crawl, BoltMode::Edge}) {
        SCOPED_TRACE((int)mode);
        BoltParams p;
        p.mode = mode;
        const std::vector<BoltSegment> a = buildBolts(edges, p), b = buildBolts(edges, p);
        ASSERT_FALSE(a.empty());
        EXPECT_TRUE(sameSegments(a, b));
        p.seed = 2;
        EXPECT_FALSE(sameSegments(a, buildBolts(edges, p))) << "another seed draws the same bolts";
    }
    EXPECT_TRUE(buildBolts({}, BoltParams{}).empty());
}

TEST(BoltBuilder, MidpointDisplacement) {
    const double ax = 10, ay = 20, bx = 210, by = 20, jag = 0.55;
    std::vector<BoltSegment> segs;
    gen::Rng r(1234.0, gen::Rng::Seeding::EffectsB);
    appendBolt(segs, r, ax, ay, bx, by, jag, 0.0, 1.0, 1.0, 6, 0.0);
    ASSERT_EQ(segs.size(), 64u);
    EXPECT_EQ(segs.front().ax, ax);
    EXPECT_EQ(segs.front().ay, ay);
    EXPECT_EQ(segs.back().bx, bx);
    EXPECT_EQ(segs.back().by, by);
    // amplitude jag x len x 0.35 x 0.55^level: the points stay within the sum of the levels
    const double bound = jag * 200.0 * 0.35 * (1.0 - std::pow(0.55, 6)) / (1.0 - 0.55) + 1e-9;
    for (size_t i = 0; i < segs.size(); i++) {
        EXPECT_LE(std::abs(segs[i].ay - ay), bound);
        if (i) { EXPECT_EQ(segs[i].ax, segs[i - 1].bx) << "the bolt is continuous"; }
        EXPECT_DOUBLE_EQ(segs[i].width, 1.0 - 0.5 * (double)i / 65.0);
        EXPECT_EQ(segs[i].intensity, 1.0);
    }
    // under a pixel long there is nothing to draw
    std::vector<BoltSegment> none;
    appendBolt(none, r, 0, 0, 0.5, 0.5, jag, 0.0, 1.0, 1.0, 6, 1.0);
    EXPECT_TRUE(none.empty());
}

TEST(BoltBuilder, BranchesInRange) {
    const double len = 300.0;
    std::vector<BoltSegment> segs;
    gen::Rng r(77.0, gen::Rng::Seeding::EffectsB);
    appendBolt(segs, r, 0, 0, len, 0, 0.5, 0.0, 1.0, 1.0, 6, 1.0);
    // the main bolt's 64 segments, a branch of 2^4 at each of its inner segments i = 2 .. 61
    ASSERT_EQ(segs.size(), 64u + 60u * 16u);
    size_t at = 0;
    int main = 0, branches = 0;
    while (at < segs.size()) {
        const BoltSegment& s = segs[at];
        if (s.intensity == 1.0) {
            main++;
            at++;
            continue;
        }
        // a branch: 16 segments at intensity 0.7, width 0.55 of the main taper, from the main bolt
        ASSERT_LE(at + 16, segs.size());
        const int i = main - 1;     // the main segment it forks from
        EXPECT_GE(i, 2);
        EXPECT_LE(i, 61);
        EXPECT_EQ(s.ax, segs[at - 1].ax) << "a branch starts where its main segment does";
        for (size_t k = at; k < at + 16; k++) {
            EXPECT_DOUBLE_EQ(segs[k].intensity, 0.7);
            EXPECT_LE(segs[k].width, 0.55 + 1e-12);
        }
        const double chord = std::hypot(segs[at + 15].bx - s.ax, segs[at + 15].by - s.ay);
        const double taper = 1.0 - (double)i / 65.0;
        EXPECT_GE(chord, 0.25 * taper * len - 1e-6);
        EXPECT_LE(chord, 0.55 * taper * len + 1e-6);
        branches++;
        at += 16;
    }
    EXPECT_EQ(main, 64);
    EXPECT_EQ(branches, 60);
    // branching off: the main bolt alone
    std::vector<BoltSegment> plain;
    appendBolt(plain, r, 0, 0, len, 0, 0.5, 0.0, 1.0, 1.0, 6, 0.0);
    EXPECT_EQ(plain.size(), 64u);
}

TEST(BoltBuilder, TwelveBoltsLayout) {
    BoxFixture one, two;
    two.scale = 2.0;
    BoltParams p;
    p.mode = BoltMode::Outward;
    p.count = 12;
    p.length = 120.0;
    p.cell = one.cell();
    const std::vector<BoltEdge> edges = one.edges();
    const std::vector<BoltSegment> segs = buildBolts(edges, p);
    BoltParams p2 = p;
    p2.length = p.length * 2.0;
    p2.cell = two.cell();
    const std::vector<BoltSegment> segs2 = buildBolts(two.edges(), p2);

    // a bolt's first segment is the only one of full width
    auto starts = [](const std::vector<BoltSegment>& s) {
        std::vector<size_t> out;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i].width == 1.0) out.push_back(i);
        }
        return out;
    };
    const std::vector<size_t> first = starts(segs), first2 = starts(segs2);
    ASSERT_EQ(first.size(), 12u);
    ASSERT_EQ(first2.size(), 12u);
    std::set<std::pair<double, double>> edgeCells;
    for (const BoltEdge& e : edges) edgeCells.insert({e.x, e.y});
    for (size_t b = 0; b < first.size(); b++) {
        SCOPED_TRACE(b);
        const BoltSegment& s = segs[first[b]];
        EXPECT_TRUE(edgeCells.count({s.ax, s.ay})) << "starts off the edge cells";
        // the bolt's last main segment ends where it aimed: outward, 0.55 .. 1 x the length away
        const size_t lastMain = (b + 1 < first.size() ? first[b + 1] : segs.size()) - 1;
        size_t end = lastMain;
        while (segs[end].intensity != 1.0) end--;
        const double chord = std::hypot(segs[end].bx - s.ax, segs[end].by - s.ay);
        EXPECT_GE(chord, 0.55 * p.length - 1e-6);
        EXPECT_LE(chord, p.length + 1e-6);
        EXPECT_FALSE(one.insideBox(segs[end].bx, segs[end].by)) << "an outward bolt ends inside the shape";
        // R10: at px_scale 2 the bolt starts at the same canvas spot
        const BoltSegment& s2 = segs2[first2[b]];
        EXPECT_NEAR(s2.ax / two.scale, s.ax, 1e-9);
        EXPECT_NEAR(s2.ay / two.scale, s.ay, 1e-9);
    }
}
