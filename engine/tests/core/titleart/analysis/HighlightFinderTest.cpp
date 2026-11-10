// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Glints' highlight analysis on synthetic grids (docs/specs/S83-title-effects-generators.md AC8,
// AC5's anchor invariance, R14's sidecar shape). The scenes are analytic images in canvas px
// sampled into 4 px cells, so each test states exactly the picture it judges.

#include <gtest/gtest.h>

#include "titleart/analysis/HighlightFinder.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::analysis;

namespace {

    constexpr double PI = 3.14159265358979323846;

    using Image = std::function<void(double x, double y, double& alpha, double& lum)>;

    // a w x h grid of 4 canvas px cells centred on the canvas centre, each cell the image at its centre
    Grid sampledGrid(int w, int h, const Image& image) {
        Grid g;
        g.w = w;
        g.h = h;
        g.cellComp = HL_CELL;
        g.cell = HL_CELL;
        g.x0 = -w * HL_CELL / 2.0;
        g.y0 = -h * HL_CELL / 2.0;
        g.alpha.resize((size_t)w * h);
        g.lum.resize((size_t)w * h);
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                double a = 0.0, l = 0.0;
                image(g.x0 + (i + 0.5) * HL_CELL, g.y0 + (j + 0.5) * HL_CELL, a, l);
                g.alpha[(size_t)j * w + i] = (float)a;
                g.lum[(size_t)j * w + i] = (float)l;
            }
        }
        return g;
    }

    // As the Grid program reads a render at px_scale k: taps on a lattice inside each cell (one a
    // target px, at most 16 a side), each the image at that pixel, then the 8-bit alpha and the
    // 16-bit luminance of the packed readback
    Grid renderedGrid(int w, int h, double k, const Image& image) {
        Grid g;
        g.w = w;
        g.h = h;
        g.cellComp = HL_CELL;
        g.cell = HL_CELL * k;
        g.x0 = -w * HL_CELL / 2.0;
        g.y0 = -h * HL_CELL / 2.0;
        g.alpha.resize((size_t)w * h);
        g.lum.resize((size_t)w * h);
        const int taps = std::min(16, std::max(1, (int)std::lround(HL_CELL * k)));
        const double pitch = HL_CELL / taps;
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                double sa = 0.0, sl = 0.0;
                for (int ty = 0; ty < taps; ty++) {
                    for (int tx = 0; tx < taps; tx++) {
                        double a = 0.0, l = 0.0;
                        image(g.x0 + i * HL_CELL + (tx + 0.5) * pitch, g.y0 + j * HL_CELL + (ty + 0.5) * pitch, a, l);
                        sa += a;
                        sl += l;
                    }
                }
                const double n = (double)taps * taps;
                uint8_t hi = 0, lo = 0;
                encodeGridLuma((float)(sl / n), hi, lo);
                g.alpha[(size_t)j * w + i] = (float)std::lround(sa / n * 255.0) / 255.0f;
                g.lum[(size_t)j * w + i] = decodeGridLuma(hi, lo);
            }
        }
        return g;
    }

    // coverage of a shape at signed distance d (canvas px, + outside) with an edge `soft` px wide
    double edge(double d, double soft = 4.0) {
        return std::clamp(0.5 - d / soft, 0.0, 1.0);
    }

    double gauss(double dx, double dy, double sigma) {
        return std::exp(-(dx * dx + dy * dy) / (2.0 * sigma * sigma));
    }

    // letter-like discs (radius 40) of base luminance 0.3 with one highlight (sigma 8 px) at
    // their centres, the luminance times gain(centre y)
    Image discs(std::vector<TitleVec2> centres, double highlight, std::function<double(double)> gain = [](double) { return 1.0; }) {
        return [centres, highlight, gain](double x, double y, double& a, double& l) {
            a = 0.0;
            l = 0.0;
            for (const TitleVec2& c : centres) {
                const double e = edge(std::hypot(x - c.x, y - c.y) - 40.0);
                if (e <= 0.0) continue;
                a = std::max(a, e);
                l = std::max(l, gain(c.y) * e * (0.3 + highlight * gauss(x - c.x, y - c.y, 8.0)));
            }
        };
    }

    int countIn(const std::vector<HighlightAnchor>& anchors, double y0, double y1) {
        int n = 0;
        for (const HighlightAnchor& a : anchors) n += a.y > y0 && a.y < y1 ? 1 : 0;
        return n;
    }

    bool anchorNear(const std::vector<HighlightAnchor>& anchors, double x, double y, double within) {
        for (const HighlightAnchor& a : anchors) {
            if (std::hypot(a.x - x, a.y - y) <= within) return true;
        }
        return false;
    }

    // A 1040 x 80 px dark bar lit along its centre line (a lit edge running along a stroke) with
    // a 3 % ripple every 32 px, which non-maximum suppression turns into a row of peaks, and twelve
    // highlight discs above it, enough to fill the 5 large and 6 small glints
    Grid litBarScene() {
        std::vector<TitleVec2> spots;
        for (int k = 0; k < 6; k++) spots.push_back({-480.0 + 180.0 * k, 80.0});
        for (int k = 0; k < 6; k++) spots.push_back({-390.0 + 180.0 * k, 192.0});
        const Image disc = discs(spots, 0.25);
        return sampledGrid(288, 128, [disc](double x, double y, double& a, double& l) {
            disc(x, y, a, l);
            const double dx = std::max(std::abs(x) - 520.0, 0.0), dy = std::max(std::abs(y + 184.0) - 40.0, 0.0);
            const double d = dx > 0.0 || dy > 0.0 ? std::hypot(dx, dy) : -std::min(520.0 - std::abs(x), 40.0 - std::abs(y + 184.0));
            const double e = edge(d);
            if (e <= 0.0) return;
            const double ridge = 0.7 * std::exp(-((y + 184.0) / 4.0) * ((y + 184.0) / 4.0) / 2.0) * (1.0 + 0.03 * std::sin(2.0 * PI * x / 32.0));
            a = std::max(a, e);
            l = std::max(l, e * (0.05 + ridge));
        });
    }

    // the bar's band of canvas y
    constexpr double BAR_Y0 = -230.0, BAR_Y1 = -140.0;

}

// AC8: the Hessian ridge weight keeps a lit straight edge from becoming a row of stars
TEST(HighlightFinderTest, LitBarGetsAtMostOneGlint) {
    const Grid g = litBarScene();
    const std::vector<HighlightAnchor> anchors = highlightAnchors(g);
    EXPECT_EQ(anchors.size(), 11u);
    EXPECT_LE(countIn(anchors, BAR_Y0, BAR_Y1), 1);
}

// AC8's red control: with the ridge weight forced to 1 the bar's peaks outscore the discs and
// string into a row
TEST(HighlightFinderTest, LitBarWithoutRidgeWeightGetsARow) {
    const Grid g = litBarScene();
    HighlightOptions options;
    options.ridge = 0.0;
    EXPECT_GE(countIn(highlightAnchors(g, options), BAR_Y0, BAR_Y1), 4);
}

TEST(HighlightFinderTest, TwoIsolatedSpotsAreBothPicked) {
    const Grid g = sampledGrid(160, 64, discs({{-200.0, 0.0}, {200.0, 0.0}}, 0.25));
    const Highlights found = findHighlights(g, finderOptions(HighlightOptions{}));
    EXPECT_EQ(found.peaks.size(), 2u);
    const GlintPicks picks = pickGlints(found, HighlightOptions{});
    const std::vector<HighlightAnchor> anchors = anchorsOf(picks, 1.0);
    EXPECT_EQ(picks.large.size(), 2u);
    EXPECT_TRUE(anchorNear(anchors, -200.0, 0.0, 4.0));
    EXPECT_TRUE(anchorNear(anchors, 200.0, 0.0, 4.0));
}

// AC8: a second text line at 60 % brightness still gets a large glint (the regional reference)
TEST(HighlightFinderTest, DimSecondLineGetsALargeGlint) {
    const std::vector<TitleVec2> letters = {{-200.0, 100.0}, {0.0, 100.0}, {200.0, 100.0}, {-200.0, -100.0}, {0.0, -100.0}, {200.0, -100.0}};
    const Grid g = sampledGrid(160, 96, discs(letters, 0.5, [](double y) { return y > 0.0 ? 1.0 : 0.6; }));
    const HighlightOptions options;
    const GlintPicks picks = pickGlints(findHighlights(g, finderOptions(options)), options);
    int secondLine = 0;
    for (const Pick& pk : picks.large) secondLine += pk.h.y < 0.0 ? 1 : 0;
    EXPECT_GE(secondLine, 1);

    // the regional reference is what keeps it: at a threshold above 60 % of the global maximum
    // only the blend toward the line's own maximum finds its peaks
    auto secondLinePeaks = [&](double regional) {
        FinderOptions f = finderOptions(options);
        f.threshold = 0.65;
        f.regional = regional;
        int n = 0;
        for (const Highlight& h : findHighlights(g, f).peaks) n += h.y < 0.0 ? 1 : 0;
        return n;
    };
    EXPECT_GE(secondLinePeaks(SPARKLE_REGIONAL), 1);
    EXPECT_EQ(secondLinePeaks(0.0), 0);
}

// AC8: ties go to the first cell in scan order, so a flat plateau yields one peak
TEST(HighlightFinderTest, FlatPlateauGivesOnePeak) {
    const Grid g = sampledGrid(64, 64, [](double x, double y, double& a, double& l) {
        const bool in = std::abs(x) < 80.0 && std::abs(y) < 80.0;
        a = in ? 1.0 : 0.0;
        l = in ? 0.5 : 0.0;
    });
    EXPECT_EQ(findHighlights(g, FinderOptions{}).peaks.size(), 1u);
}

// AC5: the grid is fixed in canvas px, so the anchors of renders at px_scale 1 and 4 agree
TEST(HighlightFinderTest, AnchorsAgreeAcrossPxScale) {
    struct Spot {
        double x, y, hx, hy;
    };
    const Spot spots[] = {{-110.0, 10.0, 6.0, -8.0}, {0.0, -15.0, -10.0, 5.0}, {115.0, 20.0, 4.0, 12.0}};
    const Image image = [&spots](double x, double y, double& a, double& l) {
        a = 0.0;
        l = 0.0;
        for (const Spot& s : spots) {
            // 1 px antialiased edges, so the two scales really sample them differently
            const double e = edge(std::hypot(x - s.x, y - s.y) - 34.0, 1.0);
            if (e <= 0.0) continue;
            a = std::max(a, e);
            const double main = 0.6 * gauss(x - s.x - s.hx, y - s.y - s.hy, 7.0);
            const double second = 0.3 * gauss(x - s.x + s.hy + 3.0, y - s.y - s.hx + 17.0, 5.0);
            l = std::max(l, e * (0.3 + main + second));
        }
    };
    const std::vector<HighlightAnchor> at1 = highlightAnchors(renderedGrid(96, 48, 1.0, image));
    const std::vector<HighlightAnchor> at4 = highlightAnchors(renderedGrid(96, 48, 4.0, image));
    ASSERT_EQ(at1.size(), 3u);
    ASSERT_EQ(at1.size(), at4.size());
    for (size_t i = 0; i < at1.size(); i++) {
        EXPECT_LE(std::hypot(at1[i].x - at4[i].x, at1[i].y - at4[i].y), 0.5) << "anchor " << i;
        EXPECT_NEAR(at1[i].radius, at4[i].radius, 0.5) << "anchor " << i;
    }
}

TEST(HighlightFinderTest, CachedResultsAreKeptByGridAndOptions) {
    clearHighlightCache();
    const Grid g = sampledGrid(160, 64, discs({{-200.0, 0.0}, {200.0, 0.0}}, 0.25));
    GridSpec spec;
    spec.cellComp = HL_CELL;
    spec.w = g.w;
    spec.h = g.h;
    const uint64_t key = gridKeyOf(42, spec);
    EXPECT_EQ(gridKeyOf(0, spec), 0u);
    const FinderOptions options = finderOptions(HighlightOptions{});
    const std::shared_ptr<const Highlights> first = findHighlightsCached(g, key, options);
    EXPECT_EQ(findHighlightsCached(g, key, options).get(), first.get());
    FinderOptions other = options;
    other.threshold = 0.5;
    EXPECT_NE(findHighlightsCached(g, key, other).get(), first.get());
    // key 0 is never kept
    EXPECT_NE(findHighlightsCached(g, 0, options).get(), findHighlightsCached(g, 0, options).get());
    clearHighlightCache();
}

// R14 / S81 R36: the sidecar's anchors are frame-normalised in image space, strongest first,
// strength relative to the strongest, at most 16
TEST(HighlightFinderTest, SidecarAnchorsAreFrameNormalised) {
    std::vector<HighlightAnchor> anchors;
    for (int i = 0; i < 20; i++) {
        HighlightAnchor a;
        a.x = -40.0 + 4.0 * i;
        a.y = 10.0;
        a.radius = 10.0;
        a.score = 0.2 + 0.01 * i;
        a.fade = i == 19 ? 0.5 : 1.0;
        anchors.push_back(a);
    }
    // a 400 x 200 canvas, the frame its middle 200 x 100 (image space)
    const TitleRectI frame{100, 50, 200, 100};
    const std::vector<SidecarAnchor> out = sidecarAnchors(anchors, frame, 400.0, 200.0);
    ASSERT_EQ(out.size(), SIDECAR_ANCHORS_MAX);
    // the strongest (index 19) at canvas (36, 10): image (236, 90), frame (136, 40)
    EXPECT_NEAR(out[0].x, 136.0 / 200.0, 1e-12);
    EXPECT_NEAR(out[0].y, 40.0 / 100.0, 1e-12);
    EXPECT_NEAR(out[0].size, 0.1, 1e-12);
    EXPECT_DOUBLE_EQ(out[0].strength, 1.0);
    EXPECT_DOUBLE_EQ(out[0].fade, 0.5);
    for (size_t i = 1; i < out.size(); i++) EXPECT_LT(out[i].strength, out[i - 1].strength);
    const Json json = sidecarAnchorsJson(out);
    ASSERT_EQ(json.size(), SIDECAR_ANCHORS_MAX);
    EXPECT_TRUE(json[0].contains("x") && json[0].contains("y") && json[0].contains("size") && json[0].contains("strength") && json[0].contains("fade"));
}
