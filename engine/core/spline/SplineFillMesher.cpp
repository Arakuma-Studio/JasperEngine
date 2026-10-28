// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Source/SplineFillMesher.cpp,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "SplineFillMesher.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    constexpr uint32_t NO_VERTEX = 0xFFFFFFFFu;

    // Where along an edge the region boundary sits. inside is a continuous metre-valued field whose
    // zero is the shoreline, and the two ends of any boundary edge bracket it: two 4-adjacent
    // corners both inside are in the SAME flooded component, so "one wet, one dry" means the dry one
    // has inside <= 0.
    float crossingFraction(float insideA, float insideB){
        const float denominator = insideA - insideB;
        if (!std::isfinite(denominator) || !(denominator > 0.0f)){
            return 0.5f; // only reachable through an unreadable corner
        }
        return std::clamp(insideA / denominator, 0.0f, 1.0f);
    }

}

std::vector<SplineFillChunkRange> spline::carveFillChunks(const SplineFillResult& field, uint32_t cellsPerChunk){
    std::vector<SplineFillChunkRange> chunks;
    if (field.countX < 2u || field.countZ < 2u){
        return chunks;
    }
    const uint32_t cells = std::clamp(cellsPerChunk, 1u, SPLINE_FILL_MAX_CELLS_PER_CHUNK);
    const uint32_t endX = field.minCornerX + field.countX;
    const uint32_t endZ = field.minCornerZ + field.countZ;
    for (uint32_t z = (field.minCornerZ / cells) * cells; z + 1u < endZ; z += cells){
        for (uint32_t x = (field.minCornerX / cells) * cells; x + 1u < endX; x += cells){
            SplineFillChunkRange range;
            range.minCornerX = std::max(x, field.minCornerX);
            range.minCornerZ = std::max(z, field.minCornerZ);
            // one past the last corner: a block's cells are bounded by corners it SHARES with the
            // next block, so both sides see the same lattice line
            range.maxCornerX = std::min(x + cells + 1u, endX);
            range.maxCornerZ = std::min(z + cells + 1u, endZ);
            if (range.maxCornerX > range.minCornerX + 1u && range.maxCornerZ > range.minCornerZ + 1u){
                chunks.push_back(range);
            }
        }
    }
    return chunks;
}

SplineFillMesh spline::buildSplineFillMesh(const SplineFillResult& field, const SplineGroundGrid& ground,
                                           std::span<const SplineStation> stations, const SplineFillChunkRange& range,
                                           const SplineFillMeshParams& params){
    SplineFillMesh mesh;
    if (!ground.isValid() || stations.empty() || field.corners.size() != static_cast<size_t>(field.countX) * field.countZ ||
        field.minCornerX + field.countX > ground.countX || field.minCornerZ + field.countZ > ground.countZ){
        return mesh;
    }
    if (range.maxCornerX > ground.countX || range.maxCornerZ > ground.countZ){
        return mesh;
    }
    // Cells outside the field's block are dry on all four corners, so the block bounds the cells
    // that can emit; the order cells emit in, and so every vertex, is the same either way.
    const uint32_t minX = std::max(range.minCornerX, field.minCornerX);
    const uint32_t minZ = std::max(range.minCornerZ, field.minCornerZ);
    const uint32_t maxX = std::min(range.maxCornerX, field.minCornerX + field.countX);
    const uint32_t maxZ = std::min(range.maxCornerZ, field.minCornerZ + field.countZ);
    if (maxX <= minX + 1u || maxZ <= minZ + 1u){
        return mesh;
    }

    const uint32_t nx = maxX - minX;
    const uint32_t nz = maxZ - minZ;
    const float drop = std::max(0.0f, params.edgeDrop);
    const float speed = std::isfinite(params.flowSpeedScale) ? std::clamp(params.flowSpeedScale, 0.0f, 1.0f) : 1.0f;

    // Dense slot arrays over the block: one per corner, per horizontal and per vertical lattice
    // edge. A crossing belongs to its EDGE, so the two cells sharing it index one vertex.
    std::vector<uint32_t> cornerSlot(static_cast<size_t>(nx) * nz, NO_VERTEX);
    std::vector<uint32_t> edgeXSlot(static_cast<size_t>(nx) * nz, NO_VERTEX); // (cx, cz) -> (cx + 1, cz)
    std::vector<uint32_t> edgeZSlot(static_cast<size_t>(nx) * nz, NO_VERTEX); // (cx, cz) -> (cx, cz + 1)

    const auto localIndex = [&](uint32_t cx, uint32_t cz){
        return static_cast<size_t>(cz - minZ) * nx + (cx - minX);
    };
    const auto cornerAt = [&](uint32_t cx, uint32_t cz) -> const SplineFillCorner&{
        return field.corners[static_cast<size_t>(cz - field.minCornerZ) * field.countX + (cx - field.minCornerX)];
    };

    const auto emit = [&](const Vector3& position, uint32_t station, float arc, float depth){
        const uint32_t governing = station < stations.size() ? station : static_cast<uint32_t>(stations.size() - 1u);
        const SplineStation& frame = stations[governing];

        SplineVertex vertex;
        vertex.position = position;
        // level across the flow, sloping along it at the centreline's grade: the station's up
        vertex.normal = frame.up;
        vertex.uv = Vector2(arc, depth);
        Vector3 tangent = frame.forward;
        const float length = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z);
        tangent = length > 1.0e-6f ? tangent * (1.0f / length) : Vector3(0.0f, 0.0f, -1.0f);
        vertex.tangent = Vector4(tangent.x, tangent.y, tangent.z, -1.0f);
        float fx = frame.forward.x;
        float fz = frame.forward.z;
        const float planar = std::sqrt(fx * fx + fz * fz);
        if (planar > 1.0e-6f){
            fx /= planar;
            fz /= planar;
        }else{
            fx = 0.0f;
            fz = 0.0f;
        }
        vertex.color = Vector4(0.5f + 0.5f * fx, 0.5f + 0.5f * fz, speed, 1.0f);

        mesh.vertices.push_back(vertex);
        return static_cast<uint32_t>(mesh.vertices.size() - 1u);
    };

    const auto cornerVertex = [&](uint32_t cx, uint32_t cz){
        uint32_t& slot = cornerSlot[localIndex(cx, cz)];
        if (slot != NO_VERTEX){
            return slot;
        }
        const SplineFillCorner& corner = cornerAt(cx, cz);
        // interior vertices sit at the WATER surface: the distance to the bed is the depth
        slot = emit(Vector3(ground.worldX(cx), corner.waterline, ground.worldZ(cz)), corner.station, corner.arcDistance,
                    corner.waterline - ground.heights[ground.index(cx, cz)]);
        return slot;
    };

    // A shoreline vertex, owned by the lattice edge it crosses. Everything it reads is a pure
    // function of that edge's two corners, so a neighbouring chunk, which recomputes rather than
    // shares it, lands on bit-identical values.
    const auto edgeVertex = [&](uint32_t cx, uint32_t cz, bool alongX){
        uint32_t& slot = (alongX ? edgeXSlot : edgeZSlot)[localIndex(cx, cz)];
        if (slot != NO_VERTEX){
            return slot;
        }

        const uint32_t bx = alongX ? cx + 1u : cx;
        const uint32_t bz = alongX ? cz : cz + 1u;
        const SplineFillCorner& a = cornerAt(cx, cz);
        const SplineFillCorner& b = cornerAt(bx, bz);

        // interpolate from the corner further inside, so both cells and both chunks compute the
        // same t from the same operands in the same order
        const bool aFirst = a.inside >= b.inside;
        const SplineFillCorner& first = aFirst ? a : b;
        const SplineFillCorner& second = aFirst ? b : a;
        const uint32_t firstCx = aFirst ? cx : bx;
        const uint32_t firstCz = aFirst ? cz : bz;
        const uint32_t secondCx = aFirst ? bx : cx;
        const uint32_t secondCz = aFirst ? bz : cz;
        const float firstX = ground.worldX(firstCx);
        const float firstZ = ground.worldZ(firstCz);
        const float secondX = ground.worldX(secondCx);
        const float secondZ = ground.worldZ(secondCz);
        const float firstGround = ground.heights[ground.index(firstCx, firstCz)];
        const float secondGround = ground.heights[ground.index(secondCx, secondCz)];

        const float t = crossingFraction(first.inside, second.inside);
        const float waterline = first.waterline + (second.waterline - first.waterline) * t;

        // Depth and arc interpolate on the SAME t as the position. Wherever the ground decides the
        // crossing, inside IS depth + edgeDrop, so this lands on exactly -edgeDrop; inside is a
        // minimum over three terms, so uv0.y >= -edgeDrop everywhere.
        const float firstDepth = first.waterline - firstGround;
        const float secondDepth = second.waterline - secondGround;

        // the rim is BURIED: only the position drops, uv0.y stays the water's depth here
        slot = emit(Vector3(firstX + (secondX - firstX) * t, waterline - drop, firstZ + (secondZ - firstZ) * t), first.station,
                    first.arcDistance + (second.arcDistance - first.arcDistance) * t,
                    firstDepth + (secondDepth - firstDepth) * t);
        return slot;
    };

    // one cell's inside-polygon: the boundary walk in order, remembering which entries are lattice
    // corners so the saddle split finds its ears without searching
    struct PolygonEntry{
        uint32_t slot = NO_VERTEX;
        bool isCorner = false;
    };
    PolygonEntry polygon[8];
    size_t polygonSize = 0;

    for (uint32_t cz = minZ; cz + 1u < maxZ; cz++){
        for (uint32_t cx = minX; cx + 1u < maxX; cx++){
            const SplineFillCorner& c00 = cornerAt(cx, cz);
            const SplineFillCorner& c01 = cornerAt(cx, cz + 1u);
            const SplineFillCorner& c11 = cornerAt(cx + 1u, cz + 1u);
            const SplineFillCorner& c10 = cornerAt(cx + 1u, cz);

            const bool w00 = c00.wet != 0u;
            const bool w01 = c01.wet != 0u;
            const bool w11 = c11.wet != 0u;
            const bool w10 = c10.wet != 0u;
            const uint32_t wetCount = (w00 ? 1u : 0u) + (w01 ? 1u : 0u) + (w11 ? 1u : 0u) + (w10 ? 1u : 0u);
            if (wetCount == 0u){
                continue;
            }

            // Walk the cell boundary c00 -> c01 -> c11 -> c10, taking each wet corner and each
            // crossing of an edge whose ends disagree. That order makes cross(p1 - p0, p2 - p0)
            // point UP (+z then +x is counter-clockwise seen from above).
            polygonSize = 0;
            const auto step = [&](bool wetHere, uint32_t cornerX, uint32_t cornerZ, bool wetNext, uint32_t edgeX,
                                  uint32_t edgeZ, bool alongX){
                if (wetHere){
                    polygon[polygonSize++] = {cornerVertex(cornerX, cornerZ), true};
                }
                if (wetHere != wetNext){
                    polygon[polygonSize++] = {edgeVertex(edgeX, edgeZ, alongX), false};
                }
            };
            step(w00, cx, cz, w01, cx, cz, false);
            step(w01, cx, cz + 1u, w11, cx, cz + 1u, true);
            step(w11, cx + 1u, cz + 1u, w10, cx + 1u, cz, false);
            step(w10, cx + 1u, cz, w00, cx, cz, true);

            if (polygonSize < 3u){
                continue;
            }

            // The two ambiguous cases, wet corners on a diagonal: the mean of the four corner
            // values (the asymptotic decider) says whether the region passes through the middle.
            // It chooses only how THIS cell's interior connects; no edge crossing moves.
            const bool diagonal = wetCount == 2u && (w00 == w11) && (w01 == w10) && (w00 != w01);
            if (diagonal){
                const float centre = (c00.inside + c01.inside + c11.inside + c10.inside) * 0.25f;
                if (!(centre > 0.0f) && polygonSize == 6u){
                    // separate: one triangle around each wet corner, between its two crossings
                    for (size_t i = 0; i < polygonSize; i++){
                        if (!polygon[i].isCorner){
                            continue;
                        }
                        mesh.indices.push_back(polygon[(i + polygonSize - 1u) % polygonSize].slot);
                        mesh.indices.push_back(polygon[i].slot);
                        mesh.indices.push_back(polygon[(i + 1u) % polygonSize].slot);
                    }
                    continue;
                }
            }

            // every case's inside-polygon is convex (a square, or one with one or two opposite
            // corners cut off), so a fan is a valid triangulation
            for (size_t i = 1; i + 1u < polygonSize; i++){
                mesh.indices.push_back(polygon[0].slot);
                mesh.indices.push_back(polygon[i].slot);
                mesh.indices.push_back(polygon[i + 1u].slot);
            }
        }
    }

    if (mesh.vertices.empty() || mesh.indices.empty()){
        mesh.vertices.clear();
        mesh.indices.clear();
        return mesh;
    }

    Vector3 minimum = mesh.vertices.front().position;
    Vector3 maximum = minimum;
    for (const SplineVertex& vertex : mesh.vertices){
        minimum.x = std::min(minimum.x, vertex.position.x);
        minimum.y = std::min(minimum.y, vertex.position.y);
        minimum.z = std::min(minimum.z, vertex.position.z);
        maximum.x = std::max(maximum.x, vertex.position.x);
        maximum.y = std::max(maximum.y, vertex.position.y);
        maximum.z = std::max(maximum.z, vertex.position.z);
    }
    mesh.bounds = AABB(minimum, maximum);
    return mesh;
}
