// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/engine/three/extrude.ts by SafEight, used with the author's permission.
//
// Contract (docs/specs/S86-title-3d.md R8, R9, R12)
// - Purpose: the solids of a 3D title. A solid is a PROFILE, a polyline in (offset, z) walked
//   from the front cap outward and down to the side wall (offset = signed distance from the
//   outline in its plane, + outward). Every profile point becomes a ring (the outline moved along
//   per-vertex miter vectors), rings are joined by quad strips, each cap is triangulated on its
//   own ring, the side wall runs down to zBack and a flat back cap closes the solid.
// - Clamps (RingExtruder): an offset stops at 45 % of the distance to the nearest other edge
//   along its miter ray (found through a uniform edge grid whose cell is the mean edge length,
//   so a clamp costs about O(1) instead of Pachislop's O(V) scan; equal to the brute force) and
//   where an adjacent edge collapses (d* = -|e|^2 / (dm.e)); `untangle` then halves offsets of
//   vertices on meeting ring edges for up to 8 rounds and pins the rest.
// - Outward bevels (OutwardBevel): ring sets are Clipper offsets of each connected component,
//   filled band by band by contour nesting, with the 0.1 % area check and the Clipper difference
//   fallback (`fallbackUsed`), and adaptive refinement where the outline changes shape.
// - The orb: a spherified cube with analytic ellipsoid normals.
// - Normals: in-plane smooth below a 32 degree turn, else the current edge's; N = (n_xy x radial,
//   z); caps (0, 0, +-1), wound as one surface; each quad wound from its area normal against its
//   vertex normals.
// - Thread-safety: pure functions over their arguments.

#pragma once

#include "Export.h"
#include "titleart/three_d/BevelProfile.h"
#include "titleart/three_d/mesher/RawMesh.h"
#include "titleart/three_d/mesher/TitleOutline3D.h"

#include <functional>
#include <optional>
#include <vector>

namespace jasperengine::titleart::three_d {

    // red controls of the mesher's tests (MesherOptions::testFlags)
    constexpr uint32_t MESHER_TEST_NO_RAY_CLAMP = 1u << 0;     // the 45 % miter ray clamp and untangle off
    constexpr uint32_t MESHER_TEST_CAP_ON_OUTLINE = 1u << 1;   // caps triangulated on the outline, not their ring
    constexpr uint32_t MESHER_TEST_FORCE_FALLBACK = 1u << 2;   // every outward band through the Clipper difference

    struct ProfilePoint {
        double off = 0.0;
        double z = 0.0;
        bool hasNormal = false;     // analytic (radial, z) normal; faceted from the polyline when false
        double nr = 0.0, nz = 0.0;
    };

    struct ProfileSpan {
        int slot = 0;
        ProfilePoint a, b;
    };

    struct SolidSpec {
        // the front cap ring
        double capOff = 0.0;
        double capZ = 0.0;
        int capSlot = 0;
        std::vector<ProfileSpan> spans;
        int wallSlot = 0;
        int backSlot = 0;
        double zBack = 0.0;
        // stop at the last span (which ends on the outline): no wall, no back cap
        bool open = false;
        uint32_t testFlags = 0;
    };

    struct OutwardSpec {
        // from the outline (offset 0) outward: offsets never decrease
        std::vector<ProfileSpan> spans;
        int wallSlot = 0;
        int backSlot = 0;
        double zBack = 0.0;
        // the finest ring step (layer units): a span halves down to it where its rings change shape
        double step = 1.0;
        double eps = 1e-4;
        // the span-table entry of the bands around one component of the outermost ring
        std::function<uint16_t(const ShapeContours& component)> span;
        uint32_t testFlags = 0;
    };

    struct ExtrudeStats {
        bool fallbackUsed = false;      // an outward band was cut by the Clipper difference
        uint32_t ringLevels = 0;        // annuli an outward profile was filled with (refinement included)
    };

    // the spans of one bevel band running outward from (off0, z0) across `width` and down `depth`,
    // appended to `out`; returns the end point
    JASPERENGINE_API ProfilePoint bandSpans(std::vector<ProfileSpan>& out, int slot, double off0, double z0, double width, double depth,
                                            const BevelShape& shape, int segments);

    // every shape extruded per `spec`
    JASPERENGINE_API void extrudeSolid(RawMesh& mesh, const Shapes& shapes, const SolidSpec& spec);
    // the bands grown outward from `shapes` (their outline is offset 0), then the wall and back cap
    JASPERENGINE_API void extrudeOutward(RawMesh& mesh, const Shapes& shapes, const OutwardSpec& spec, ExtrudeStats* stats = nullptr);

    // the orb's per-face grid for a radius of `radiusPx` render px: (pi/2) sqrt(r), 24..128 in steps of 8
    JASPERENGINE_API int orbGridFor(double radiusPx);
    // a closed ellipsoid (rx, ry, rz) centred at (cx, cy, cz), all in `slot`, `grid` quads per face edge
    JASPERENGINE_API void extrudeOrb(RawMesh& mesh, int slot, double rx, double ry, double rz, double cx, double cy, double cz, int grid);

    // ---- the clamp, exposed for its tests ----
    // per ring vertex the largest inward / outward travel (multiples of its miter vector)
    struct RingLimits {
        std::vector<std::vector<double>> maxIn, maxOut;
    };
    // the ray limits of every vertex of `contours` (one shape) through the edge grid, or the
    // O(V^2) scan when `bruteForce` (the tests' reference)
    JASPERENGINE_API RingLimits rayLimits(const std::vector<Contour>& contours, bool bruteForce);

}
