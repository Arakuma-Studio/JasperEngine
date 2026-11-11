// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/engine/three/inflate.ts by SafEight, used with the author's permission.
//
// Contract (docs/specs/S86-title-3d.md R11)
// - Purpose: inflate mode. The front of every shape is a height field over its outline's distance
//   field on a square grid of spacing `step` clipped exactly to the outline: full cells are two
//   triangles with alternating diagonals, crossed cells are the polygons cell & shape, ear
//   clipped; every outline / grid-line crossing is one shared vertex, so the surface, the wall
//   and neighbouring cells meet without cracks or T-junctions. Node distances are exact segment
//   distances (4h buckets) with exact gradients, relaxed twice; z = height (profile(d / maxDist)
//   - 1) with maxDist shared over the title; normals are area weighted (smoothness 0) or the
//   profile's exact slope along the gradient averaged ceil(smoothness / 10) times; a straight wall
//   (32 degree crease) and a flat back close the solid.
// - Fields depend only on the outline and the step: InflateFieldCache keys them by (outline hash,
//   step), so height, profile and depth edits only re-emit triangles.
// - Thread-safety: buildInflateField and emitInflate are pure; the cache locks.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/three_d/mesher/RawMesh.h"
#include "titleart/three_d/mesher/TitleOutline3D.h"

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <vector>

namespace jasperengine::titleart::three_d {

    constexpr int INFLATE_NODES_PER_INSET = 12;
    constexpr double INFLATE_MIN_STEP_PX = 0.75;

    struct InflateField {
        std::vector<double> xy;         // vertex positions, interleaved x, y
        std::vector<double> dist;       // distance to the outline (0 on it), relaxed
        std::vector<double> grad;       // unit gradient of the exact distance, interleaved
        std::vector<uint32_t> tris;     // front triangles, counter-clockwise from +z
        std::vector<std::vector<uint32_t>> rings;   // outline contours with every crossing inserted
        double maxDist = 0.0;
        size_t nodes = 0;               // grid nodes inside (the node cap counts them)
    };

    // the output scale as the grid sees it: the nearest power of two
    JASPERENGINE_API double inflateScale(double outputScale);
    // the grid step of shapes inflated together: max(s / (12 detail), sqrt(area / maxNodes),
    // 0.75 / outputScale), s = the largest 1.5 area / perimeter
    JASPERENGINE_API double inflateStep(const Shapes& shapes, double outputScale, int detail, size_t maxNodes);
    // s of inflateStep: the largest 1.5 area / perimeter of the shapes (0 when none has area)
    JASPERENGINE_API double inflateInset(const Shapes& shapes);
    // the coarsest grid a tier's triangle budget may take: this many nodes across the largest inset,
    // so the dome keeps its rise and curvature
    constexpr double INFLATE_MIN_NODES_PER_INSET = 3.0;
    // the field of one normalised shape at `step` (empty when every phase retry is degenerate)
    JASPERENGINE_API std::shared_ptr<const InflateField> buildInflateField(const ShapeContours& shape, double step);

    // Fields keyed by (outline hash, step), most recent first
    class JASPERENGINE_API InflateFieldCache {
    public:
        explicit InflateFieldCache(size_t capacity = 256);
        std::shared_ptr<const InflateField> get(const ShapeContours& shape, double step);
        uint64_t hits() const;
        uint64_t misses() const;
        void clear();

    private:
        struct Entry {
            uint64_t key;
            std::shared_ptr<const InflateField> field;
        };
        mutable std::mutex lock;
        std::list<Entry> entries;
        size_t capacity;
        uint64_t hitCount = 0, missCount = 0;
    };

    struct InflateSpec {
        int faceSlot = 0, wallSlot = 0, backSlot = 0;
        double height = 0.0;            // rise above the silhouette at full profile (layer units)
        TitleCurve profile;             // distance / maxDist (0..1) -> 0..1
        double maxDist = 1.0;           // shared by every shape of a title
        double zBack = 0.0;
        double smoothness = 0.0;        // 0..100
    };

    // the silhouette's z (the wall top): height x (profile(0) - 1)
    JASPERENGINE_API double inflateWallTop(double height, const TitleCurve& profile);
    JASPERENGINE_API void emitInflate(RawMesh& mesh, const InflateField& field, const InflateSpec& spec);

}
