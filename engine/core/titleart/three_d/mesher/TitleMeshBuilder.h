// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R13, R15, §6.3)
// - Purpose: a title's triangle soup (TitleSolids) -> the indexed TitleMesh the renderer draws.
//   Vertices are quantised to the 24-byte format first (octahedral snorm16 normal, ids), then
//   welded by exact (position, normal, ids[, wrap]) in first-seen order, so smoothing groups fall
//   out of the normals: smooth runs share vertices, creases and caps do not. meshoptimizer then
//   runs optimizeVertexCache and optimizeOverdraw (threshold 1.05) per slot range and
//   optimizeVertexFetch over the whole; the mesh is never simplified. 16-bit indices up to
//   65,535 vertices. The wrap stream (u = arc length / size, v = -z / size; caps x / size,
//   y / size) is built only when asked.
// - Thread-safety: pure.

#pragma once

#include "Export.h"
#include "titleart/three_d/TitleMesh.h"
#include "titleart/three_d/mesher/TitleSolids.h"

#include <memory>

namespace jasperengine::titleart::three_d {

    JASPERENGINE_API std::shared_ptr<TitleMesh> buildTitleMeshFromSolids(const TitleSolids& solids, bool wrapStream, double size);

}
