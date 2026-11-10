// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md §6.12)
// - Purpose: the mesher's polygon triangulator, Mapbox earcut.hpp (engine/libs/earcut, ISC):
//   the algorithm three.js's ShapeUtils.triangulateShape wraps, so the same Clipper-normalised
//   input gives Pachislop's caps.
// - Indices refer to the points of `outer` then of each hole in order; triangles come in earcut's
//   deterministic order with its winding (callers re-wind them to face their normals).
// - Thread-safety: pure.

#pragma once

#include "titleart/three_d/mesher/TitleOutline3D.h"

#include <cstdint>
#include <vector>

namespace jasperengine::titleart::three_d {

    JASPERENGINE_API std::vector<uint32_t> triangulatePolygon(const Contour& outer, const std::vector<Contour>& holes);

}
