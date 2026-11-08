// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md §6.7.14, R10, R15, R21)
// - Purpose: Lightning's CPU layout, Pachislop's src/generators/energy/lightning.ts (findEdges,
//   bolt, lightningBolts, edgeArcs, drawBolts' quad geometry) ported bit for bit: the edge cells
//   of the followed layer's content grid with Sobel normals, midpoint-displacement bolts with
//   branches, outward and crawling bolts started from cells picked by position, and Electrify's
//   arcs walking the edge through a hash grid of the cells.
// - Units: Pachislop's render px of the whole frame (the canvas at px_scale), origin at its
//   bottom-left, +y up; lengths are render px (Px params times px_scale). Its comparisons and
//   floors (`len < 1`, the hash grid's cells) are not scale-free, so the layout runs in those
//   units and the generator maps the segments onto its target afterwards.
// - Randomness: each bolt or arc draws from its own mulberry32 stream (Rng, effects/b seeding)
//   seeded by hashN(seed, index, salt), in Pachislop's draw order; start cells are picked by
//   position, so a bolt lands on the same canvas spot at any px_scale (R10).
// - Pure functions of their inputs (unit-testable without a GPU); any thread.

#pragma once

#include "Export.h"
#include "titleart/generators/TitleGen.h"

#include <cstdint>
#include <vector>

namespace jasperengine::titleart::analysis {

    // the lightning edge grid's cell side, canvas px (Pachislop EDGE_CELL)
    constexpr double BOLT_EDGE_CELL = 6.0;

    // an edge cell's centre and outward unit normal
    struct BoltEdge {
        double x = 0.0, y = 0.0;
        double nx = 0.0, ny = 0.0;
    };

    // one straight piece of a bolt: its ends, its core width multiplier and its intensity
    struct BoltSegment {
        double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
        double width = 1.0;
        double intensity = 1.0;
    };

    enum class BoltMode : uint8_t { Mixed, Outward, Crawl, Edge };

    struct BoltParams {
        BoltMode mode = BoltMode::Mixed;
        int count = 12;             // bolts (mixed, outward, crawl)
        int arcs = 24;              // arcs (edge)
        double length = 240.0;      // render px
        double jaggedness = 0.55;   // 0..1.5
        double branches = 0.6;      // 0..1
        double wrinkle = 0.6;       // 0..1.5 (edge)
        double branchiness = 0.4;   // 0..1 (edge)
        double seed = 1.0;
        double cell = BOLT_EDGE_CELL;   // the edge grid's cell side, render px
    };

    // the edge cells of a coverage grid (w x h means, row 0 at the bottom): cells at alpha >= 0.5
    // with a 4-neighbour under 0.5, their outward normal minus the Sobel gradient of coverage.
    // Cell (0, 0)'s lower-left corner lies at (px0, py0), cells are `cell` render px
    JASPERENGINE_API std::vector<BoltEdge> findBoltEdges(const std::vector<float>& alpha, int w, int h, double px0, double py0, double cell);

    // the segments of every bolt (or arc) of `params` over `edges` (none without edges)
    JASPERENGINE_API std::vector<BoltSegment> buildBolts(const std::vector<BoltEdge>& edges, const BoltParams& params);

    // A midpoint-displacement bolt from a to b appended to `out` (Pachislop bolt): `depth` halvings
    // with amplitude jag x length x 0.35 x 0.55^level, the first level bent by bulge x length,
    // widths tapering to 1 - 0.5 i/n, and at each inner segment a branch with probability
    // branchProb (+-0.35..0.85 rad, 0.25..0.55 of the length, width x 0.55, intensity x 0.7,
    // depth - 2). `random` is the bolt's stream, consumed in Pachislop's order
    JASPERENGINE_API void appendBolt(std::vector<BoltSegment>& out, gen::Rng& random, double ax, double ay, double bx, double by, double jag,
                                     double bulge, double width, double intensity, int depth, double branchProb);

    // a segment's core quad (Pachislop drawBolts): `coreWidth` render px (the core width param x
    // px_scale x 0.5) gives the half width max(0.7, coreWidth x width) x 1.6, across which the
    // profile reaches ~0; the quad runs half that width past each end so joints have no gaps
    struct BoltQuad {
        double cx = 0.0, cy = 0.0;      // centre
        double length = 0.0;            // full length along the segment
        double halfWidth = 0.0;         // half the size across
        double rotation = 0.0;          // the segment's direction, radians counter-clockwise
    };
    // false for a segment of no length (Pachislop's quad collapses: it draws nothing)
    JASPERENGINE_API bool boltQuad(const BoltSegment& segment, double coreWidth, BoltQuad& out);

}
