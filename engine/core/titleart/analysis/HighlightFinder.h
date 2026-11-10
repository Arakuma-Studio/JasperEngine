// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Ported from Pachislop src/generators/glints.ts by SafEight, used with the author's permission.

// Contract (docs/specs/S83-title-effects-generators.md §6.7.8, R10, R14, R20, AC5, AC8)
// - Purpose: Glints' highlight analysis, Pachislop's findHighlights / pickHighlights / gridBox /
//   regionalMax / highlightSize / wobble ported as pure functions of an analysis Grid (the
//   followed layer as styled, Luma packing, 4 canvas px cells: ReadGrid.h). Peaks are local
//   maxima of the smoothed luminance on the shape scored by prominence over a +-6 cell box mean,
//   a share of absolute brightness, a depth weight (face_bias) and a Hessian ridge weight, against
//   a reference blending the global and the regional (~128 px) maximum; picking is greedy with
//   spacing, near ties blended and the last picks faded against the runner-up, so the stars sit
//   in the same canvas place at every px_scale (R10).
// - Units: positions, radii and boxes are canvas px about the canvas centre, +y up (document
//   space, the grid's own frame); option fractions are 0..1 (the params' percent / 100).
// - Bake products (R14, S81 R36): highlightAnchors() runs the "highlights" placement with the
//   Glints defaults on any grid (the bake calls it on the grid of the baked image), and
//   sidecarAnchors() turns anchors into S80's sidecar shape {x, y, size, strength, fade},
//   frame-normalised, at most 16.
// - Caching: findHighlightsCached() keeps results by (grid key, options), the grid key being
//   gridKeyOf(the source's content key, the grid's spec) (key 0 = not cached), bounded to the 32
//   most recent; the grid itself is cached by readGrid.
// - Thread-safety: the functions are pure; the cache is guarded by a mutex.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"
#include "titleart/analysis/ReadGrid.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace jasperengine::titleart::analysis {

    // Pachislop's constants (glints.ts)
    constexpr double HL_CELL = 4.0;             // analysis cell, canvas px
    constexpr int HL_BG = 6;                    // cells each way the prominence is measured against
    constexpr double HL_FADE = 0.015;           // score margin (of the top score) of fades and near ties
    constexpr double HL_TIE_REACH = 0.25;       // near ties within this fraction of a star's radius blend
    constexpr double SMALL_SPARKLE = 0.34;      // small glints' radius, fraction of the size
    constexpr double HERO_STAR = 0.45;          // the hero's companion stars' radius, fraction of the size
    constexpr double SPARKLE_RIDGE = 0.7;       // how much sparkles avoid lit edges (ridges)
    constexpr double SPARKLE_REGIONAL = 0.7;    // how far sparkles are judged against their own region
    constexpr size_t SIDECAR_ANCHORS_MAX = 16;  // S81 R36

    // a highlight peak
    struct Highlight {
        double x = 0.0, y = 0.0;    // canvas px about the canvas centre, +y up
        double score = 0.0;
        int64_t key = 0;            // the cell counted from the canvas centre: breaks score ties
    };

    // a box in canvas px about the canvas centre, +y up
    struct InkBox {
        double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    };

    struct FinderOptions {
        double threshold = 0.3;     // cells dimmer than this fraction of the reference are skipped
        double faceBias = 0.0;      // damps peaks near the silhouette against deeper ones
        double ridge = 0.0;         // damps peaks on bright ridges; 0 = w_ridge forced to 1
        double regional = 0.0;      // blends the reference from the global toward the regional maximum
    };

    struct Highlights {
        std::vector<Highlight> peaks;   // strongest first (score, then key)
        bool hasBox = false;            // false: the layer has no cell with alpha >= 0.5
        InkBox box;                     // the cells with alpha >= 0.5
    };

    // a picked star
    struct Pick {
        Highlight h;                // its peak; a star between near-tied peaks has their blended position
        int source = -1;            // the peak's index in the list it was picked from
        bool blended = false;       // h is a blend (no longer the peak itself)
        double r = 0.0;             // radius, canvas px
        double fade = 1.0;          // 0..1 fade-in against the runner-up
    };

    // Glints' "highlights" placement (the params as fractions and canvas px; the defaults are
    // the Glints defaults)
    struct HighlightOptions {
        int large = 5, small = 6;
        double size = 50.0;             // star size, canvas px
        double sizeVariation = 0.6;
        double threshold = 0.3;
        double spacing = 80.0;          // minimum spacing, canvas px
        double faceBias = 0.7;
        double ridge = SPARKLE_RIDGE;
        double regional = SPARKLE_REGIONAL;
        double seed = 1.0;              // the element seed (the twinkle phase)
    };

    struct GlintPicks {
        std::vector<Pick> large, small;
    };

    // an anchor of the bake (S83 §6.7.8, S81 R36)
    struct HighlightAnchor {
        double x = 0.0, y = 0.0;    // canvas px about the canvas centre, +y up
        double radius = 0.0;        // canvas px
        double score = 0.0;
        double phase = 0.0;         // 0..1, the twinkle's
        double fade = 1.0;          // the pick's fade against the runner-up
    };

    // S80's sidecar anchor: frame-normalised (x, y in image space: 0, 0 at the frame's top-left,
    // 1, 1 at its bottom-right; size = the radius over the frame's height), strength = the score
    // over the strongest anchor's
    struct SidecarAnchor {
        double x = 0.0, y = 0.0, size = 0.0, strength = 0.0, fade = 1.0;
    };

    // the box of the cells with alpha >= minAlpha (Pachislop gridBox at 0.5); false when none
    JASPERENGINE_API bool gridBox(const Grid& g, InkBox& box, float minAlpha = 0.5f);

    // Pachislop findHighlights on a grid
    JASPERENGINE_API Highlights findHighlights(const Grid& g, const FinderOptions& options);
    // the grid key findHighlightsCached keys by: 0 when sourceKey is 0
    JASPERENGINE_API uint64_t gridKeyOf(uint64_t sourceKey, const GridSpec& spec);
    // findHighlights kept by (gridKey, options); gridKey 0 computes without keeping
    JASPERENGINE_API std::shared_ptr<const Highlights> findHighlightsCached(const Grid& g, uint64_t gridKey, const FinderOptions& options);
    // forgets every kept result (tests)
    JASPERENGINE_API void clearHighlightCache();

    // Pachislop pickHighlights: up to `count` of the peaks `order` lists (indices into `peaks`,
    // strongest first), apart by max(minDist, spacing x (r1 + r2)), also from `taken` (picks of
    // the same list)
    JASPERENGINE_API std::vector<Pick> pickHighlights(const std::vector<Highlight>& peaks, const std::vector<int>& order, int count,
                                                      const std::function<double(const Highlight&)>& radius, double spacing,
                                                      double minDist = 0.0, const std::vector<Pick>& taken = {});
    // over every peak
    JASPERENGINE_API std::vector<Pick> pickHighlights(const std::vector<Highlight>& peaks, int count,
                                                      const std::function<double(const Highlight&)>& radius, double spacing,
                                                      double minDist = 0.0, const std::vector<Pick>& taken = {});

    // size factor (0..1] of a star from its score relative to the strongest (Pachislop highlightSize)
    JASPERENGINE_API double highlightSize(double score, double top, double variation);
    // smooth pseudo-random 0..1 of a canvas position, channel k (Pachislop wobble)
    JASPERENGINE_API double glintWobble(double x, double y, double seed, int k);

    // the finder options of the "highlights" placement
    JASPERENGINE_API FinderOptions finderOptions(const HighlightOptions& options);
    // the large and small glints of the "highlights" placement (renderPeaks); empty without a box
    JASPERENGINE_API GlintPicks pickGlints(const Highlights& found, const HighlightOptions& options);
    // anchors of picks (large then small), the phase from the seed and the position
    JASPERENGINE_API std::vector<HighlightAnchor> anchorsOf(const GlintPicks& picks, double seed);
    // the "highlights" placement's anchors on a grid (gridKey as findHighlightsCached)
    JASPERENGINE_API std::vector<HighlightAnchor> highlightAnchors(const Grid& g, const HighlightOptions& options = {}, uint64_t gridKey = 0);

    // anchors in S80's sidecar shape, strongest first, at most SIDECAR_ANCHORS_MAX: `frame` is the
    // bake's frame in canvas px, image space (TitleArtBake), on a canvas of this size
    JASPERENGINE_API std::vector<SidecarAnchor> sidecarAnchors(const std::vector<HighlightAnchor>& anchors, const TitleRectI& frame,
                                                               double canvasWidth, double canvasHeight);
    // the sidecar's `anchors` member: [{x, y, size, strength, fade}, ...]
    JASPERENGINE_API Json sidecarAnchorsJson(const std::vector<SidecarAnchor>& anchors);

}
