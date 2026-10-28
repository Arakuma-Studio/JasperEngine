// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Source/SplineStripBuilder.cpp,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "SplineStripBuilder.h"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    // Smallest advance that counts as a new station: below it the two rings are one ring, the quad
    // between them has no area and its zero-length longitudinal edge no direction.
    constexpr float MIN_STATION_ADVANCE = 1.0e-5f;

    // below this a scaled profile edge has collapsed (a taper pinching the width to nothing), and
    // its direction is taken from the unscaled profile instead
    constexpr float MIN_EDGE_LENGTH = 1.0e-6f;

    // A triangle counts as folded only when it faces more than about 95 degrees away from its
    // vertices' normals: paths and roads carry triangles facing just past 90 degrees from them,
    // which stay on the default diagonal.
    constexpr float FOLDED_COSINE = -0.1f;

    inline Vector3 crossOf(const Vector3& a, const Vector3& b){
        return Vector3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    }

    inline float dotOf(const Vector3& a, const Vector3& b){
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    bool isFinite(const Vector3& v){
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    Vector3 normalizedOr(const Vector3& v, const Vector3& fallback){
        const float lengthSquared = dotOf(v, v);
        if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-12f){
            return fallback;
        }
        return v * (1.0f / std::sqrt(lengthSquared));
    }

    const Vector3 FALLBACK_FORWARD(0.0f, 0.0f, -1.0f);
    const Vector3 FALLBACK_RIGHT(1.0f, 0.0f, 0.0f);
    const Vector3 FALLBACK_UP(0.0f, 1.0f, 0.0f);

    struct Planar{
        float x = 0.0f;
        float y = 0.0f;
    };

    Planar normalizedOr2D(const Planar& v, const Planar& fallback){
        const float lengthSquared = v.x * v.x + v.y * v.y;
        if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-12f){
            return fallback;
        }
        const float inverse = 1.0f / std::sqrt(lengthSquared);
        return Planar{v.x * inverse, v.y * inverse};
    }

    // One emitted vertex position within a ring. A profile point becomes two slots where it
    // creases, and a closed profile gains one more so its V can reach the full perimeter at the
    // seam without unwelding the geometry.
    struct RingSlot{
        uint32_t pointIndex = 0;
        int32_t edgeA = -1;
        int32_t edgeB = -1; // -1 when the slot takes a single edge's normal
        bool seamV = false;
    };

    struct ProfileTopology{
        std::vector<RingSlot> slots;
        // profile edges as slot pairs; each becomes one quad per station interval
        std::vector<std::pair<uint32_t, uint32_t>> quads;
        uint32_t edgeCount = 0;
    };

    ProfileTopology buildTopology(const SplineProfile& profile){
        ProfileTopology topo;
        const uint32_t pointCount = static_cast<uint32_t>(profile.points.size());
        topo.edgeCount = profile.closed ? pointCount : pointCount - 1u;

        std::vector<int32_t> incomingSlot(pointCount, -1);
        std::vector<int32_t> outgoingSlot(pointCount, -1);
        topo.slots.reserve(static_cast<size_t>(pointCount) * 2u + 1u);

        for (uint32_t j = 0; j < pointCount; j++){
            const int32_t inEdge = profile.closed ? static_cast<int32_t>((j + pointCount - 1u) % pointCount)
                                                  : (j == 0u ? -1 : static_cast<int32_t>(j - 1u));
            const int32_t outEdge = profile.closed ? static_cast<int32_t>(j)
                                                   : (j + 1u == pointCount ? -1 : static_cast<int32_t>(j));

            // an open profile's ends have one adjacent edge each and split whether flagged or not
            const bool split = profile.points[j].hard != 0u || inEdge < 0 || outEdge < 0;
            if (!split){
                const int32_t slot = static_cast<int32_t>(topo.slots.size());
                topo.slots.push_back(RingSlot{j, inEdge, outEdge, false});
                incomingSlot[j] = slot;
                outgoingSlot[j] = slot;
                continue;
            }
            if (inEdge >= 0){
                incomingSlot[j] = static_cast<int32_t>(topo.slots.size());
                topo.slots.push_back(RingSlot{j, inEdge, -1, false});
            }
            if (outEdge >= 0){
                outgoingSlot[j] = static_cast<int32_t>(topo.slots.size());
                topo.slots.push_back(RingSlot{j, outEdge, -1, false});
            }
        }

        topo.quads.reserve(topo.edgeCount);
        for (uint32_t e = 0; e < topo.edgeCount; e++){
            const uint32_t pointA = e;
            const uint32_t pointB = (e + 1u) % pointCount;
            uint32_t slotB = static_cast<uint32_t>(incomingSlot[pointB]);
            // The wrap edge lands back on point 0, whose V is 0 while the seam needs the whole
            // perimeter. A split point 0 gives this edge sole ownership of the incoming slot, which
            // then carries the seam V itself; an unsplit one shares its slot with edge 0, so the seam
            // gets a duplicate. Either way the UV splits and the geometry stays welded.
            if (profile.closed && e + 1u == topo.edgeCount){
                if (incomingSlot[0] != outgoingSlot[0]){
                    topo.slots[slotB].seamV = true;
                }else{
                    RingSlot seam = topo.slots[slotB];
                    seam.seamV = true;
                    slotB = static_cast<uint32_t>(topo.slots.size());
                    topo.slots.push_back(seam);
                }
            }
            topo.quads.emplace_back(static_cast<uint32_t>(outgoingSlot[pointA]), slotB);
        }
        return topo;
    }

    bool usableStation(const SplineStation& s){
        return isFinite(s.position) && isFinite(s.right) && isFinite(s.up) && isFinite(s.forward) &&
               std::isfinite(s.distance) && std::isfinite(s.halfWidthLeft) && std::isfinite(s.halfWidthRight) &&
               std::isfinite(s.rollRadians) && isFinite(s.mitreRight) && std::isfinite(s.groundOffset) &&
               std::isfinite(s.topOffset) && std::isfinite(s.faceTurnLeft) && std::isfinite(s.faceTurnRight) &&
               std::isfinite(s.topArcAllowance);
    }

    // Walk the stations that are finite and strictly advance, calling keep(index, distance) on each
    // with the distance the sweep gives it. A repeated station is not a shorter strip, it is a ring
    // with no interval. A corner's Crease and Pivot rings are the exception: they stand at the
    // previous ring's distance by design and take it exactly, so every ring of a corner carries one
    // U; with nothing before it such a ring simply opens the strip.
    template<typename Keep>
    void walkKeptStations(std::span<const SplineStation> stations, Keep&& keep){
        bool any = false;
        float lastDistance = 0.0f;
        for (size_t i = 0; i < stations.size(); i++){
            const SplineStation& s = stations[i];
            if (!usableStation(s)){
                continue;
            }
            if (any && s.join == SplineStationJoin::Advance && s.distance <= lastDistance + MIN_STATION_ADVANCE){
                continue;
            }
            const float distance = (any && s.join != SplineStationJoin::Advance) ? lastDistance : s.distance;
            keep(i, distance, !any);
            lastDistance = distance;
            any = true;
        }
    }

    void filterStations(std::span<const SplineStation> stations, std::vector<SplineStation>& kept,
                        std::vector<uint32_t>* keptIndex){
        kept.clear();
        kept.reserve(stations.size());
        walkKeptStations(stations, [&](size_t i, float distance, bool first){
            kept.push_back(stations[i]);
            kept.back().distance = distance;
            if (first){
                kept.back().join = SplineStationJoin::Advance;
            }
            if (keptIndex){
                keptIndex->push_back(static_cast<uint32_t>(i));
            }
        });
    }

    // Whether triangle (a, b, c) faces the way its vertices' normals do: false only for a triangle
    // clearly folded back through itself.
    bool facesItsNormals(const SplineVertex& a, const SplineVertex& b, const SplineVertex& c){
        const Vector3 geometric = crossOf(b.position - a.position, c.position - a.position);
        const Vector3 normals = a.normal + b.normal + c.normal;
        const float area = std::sqrt(dotOf(geometric, geometric));
        const float normalLength = std::sqrt(dotOf(normals, normals));
        if (!(area > 0.0f) || !(normalLength > 0.0f)){
            return true; // a degenerate triangle has no facing to judge
        }
        return dotOf(geometric, normals) >= FOLDED_COSINE * area * normalLength;
    }

    // A triangle with two coincident corners has no surface: a round corner's fan pivoting on its
    // inner mitre point, or a ring pinched to nothing. It is left out rather than handed to the
    // renderer and the physics mesh.
    void appendTriangle(std::vector<uint32_t>& indices, const std::vector<SplineVertex>& vertices,
                        uint32_t a, uint32_t b, uint32_t c){
        const Vector3& pa = vertices[a].position;
        const Vector3& pb = vertices[b].position;
        const Vector3& pc = vertices[c].position;
        if (pa == pb || pb == pc || pc == pa){
            return;
        }
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
    }

    // Append one band quad (a, b on this ring; d, c on the next, d beside a) as two triangles in the
    // outward winding: (a, b, d) and (b, c, d), whose geometric normals point along the profile's
    // outward normal in this right-handed frame (OpenEngine's (a, d, b), (b, d, c) reversed). A quad
    // that is not convex, the top where a corner's inside narrows toward its bounded mitre point,
    // folds one of those back and is split on the other diagonal when that one does not.
    void appendBandQuad(std::vector<uint32_t>& indices, const std::vector<SplineVertex>& vertices,
                        uint32_t a, uint32_t b, uint32_t c, uint32_t d){
        const bool defaultHolds = facesItsNormals(vertices[a], vertices[b], vertices[d]) &&
                                  facesItsNormals(vertices[b], vertices[c], vertices[d]);
        const bool otherHolds = facesItsNormals(vertices[a], vertices[c], vertices[d]) &&
                                facesItsNormals(vertices[a], vertices[b], vertices[c]);
        if (!defaultHolds && otherHolds){
            appendTriangle(indices, vertices, a, c, d);
            appendTriangle(indices, vertices, a, b, c);
            return;
        }
        appendTriangle(indices, vertices, a, b, d);
        appendTriangle(indices, vertices, b, c, d);
    }

    // Width scale for one station, per side of travel, shared by the ring sweep, the risers and the
    // caps. A point picks its side by the sign of its lateral coordinate; equal half-widths resolve
    // both sides to one value, the single-half-width arithmetic unchanged.
    struct StationScale{
        float left = 1.0f;
        float right = 1.0f;
        bool scaleVertical = false;
    };

    StationScale scaleOfStation(const SplineStation& station, const SplineProfile& profile, SplineProfileScale mode){
        StationScale scale;
        if (mode != SplineProfileScale::None){
            const float left = std::max(0.0f, station.halfWidthLeft) / profile.nominalHalfWidth;
            const float right = std::max(0.0f, station.halfWidthRight) / profile.nominalHalfWidth;
            scale.left = std::isfinite(left) ? left : 1.0f;
            scale.right = std::isfinite(right) ? right : 1.0f;
            scale.scaleVertical = mode == SplineProfileScale::Uniform;
        }
        return scale;
    }

    std::pair<float, float> scaleForPoint(const StationScale& scale, float lateral){
        const float s = (lateral < 0.0f) ? scale.left : scale.right;
        return {s, scale.scaleVertical ? s : 1.0f};
    }

    // A station's frame rolled about forward by its roll channel, OpenEngine's evaluator
    // convention: a positive roll raises the right edge (right * cos + up * sin).
    struct RingBasis{
        Vector3 right;
        Vector3 up;
        Vector3 mitreRight;
    };

    RingBasis rolledBasis(const SplineStation& station){
        RingBasis basis{station.right, station.up, station.mitreRight};
        if (std::abs(station.rollRadians) > 0.0f){
            const float c = std::cos(station.rollRadians);
            const float s = std::sin(station.rollRadians);
            basis.right = station.right * c + station.up * s;
            basis.up = station.up * c - station.right * s;
            basis.mitreRight = station.mitreRight * c + station.up * (s * std::sqrt(dotOf(station.mitreRight, station.mitreRight)));
        }
        return basis;
    }

    Vector4 flowColor(const SplineStation& station, const SplineStripParams& params){
        if (!params.writeFlow){
            return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        }
        float fx = station.forward.x;
        float fz = station.forward.z;
        const float length = std::sqrt(fx * fx + fz * fz);
        if (length > 1.0e-6f){
            fx /= length;
            fz /= length;
        }else{
            fx = 0.0f;
            fz = 0.0f;
        }
        const float speed = std::isfinite(params.flowSpeedScale) ? std::clamp(params.flowSpeedScale, 0.0f, 1.0f) : 1.0f;
        return Vector4(0.5f + 0.5f * fx, 0.5f + 0.5f * fz, speed, 1.0f);
    }

    // How a riser or cap measures its UVs: lateral metres across, V the height above a datum or the
    // profile's own height.
    struct EndFaceUV{
        float tilesPerMetreV = 1.0f;
        bool heightV = false;
        float heightVDatumMetres = 0.0f;
    };

    // Close the step between ring `earlier` and the Crease ring after it, whose raised (not
    // grounded) points stand at another height: one polygon in the crease plane, the earlier ring's
    // raised points in profile order, then the later ring's in reverse, facing the lower side.
    void emitRiser(SplineStripMesh& mesh, const SplineProfile& profile, std::span<const uint32_t> raised,
                   std::span<const uint32_t> firstSlotOfPoint, uint32_t earlier, const SplineStation& earlierStation,
                   const SplineStation& laterStation, const SplineStripParams& params, const EndFaceUV& uvs){
        const uint32_t ringVertexCount = mesh.ringVertexCount;
        const float rise = laterStation.topOffset - earlierStation.topOffset;
        const Vector3 laterForward = normalizedOr(laterStation.forward, FALLBACK_FORWARD);
        const Vector3 ahead = normalizedOr(normalizedOr(earlierStation.forward, FALLBACK_FORWARD) + laterForward, laterForward);
        const auto ringPosition = [&](uint32_t ring, uint32_t point) -> Vector3{
            return mesh.vertices[static_cast<size_t>(ring) * ringVertexCount + firstSlotOfPoint[point]].position;
        };
        // The two rings differ only in their tops, which move along the up they share, so the riser
        // is the plane through the earlier ring's raised points and that up. Stepping up it faces
        // back along the run toward the lower stretch before it; stepping down, ahead.
        const Vector3 lateral = normalizedOr(ringPosition(earlier, raised.back()) - ringPosition(earlier, raised.front()),
                                             normalizedOr(laterStation.right, FALLBACK_RIGHT));
        Vector3 riserNormal = normalizedOr(crossOf(lateral, laterStation.up), ahead);
        if ((dotOf(riserNormal, ahead) > 0.0f) == (rise > 0.0f)){
            riserNormal = riserNormal * -1.0f;
        }
        // U runs along the lateral axis and V up the riser
        const float handedness = dotOf(crossOf(riserNormal, lateral), laterStation.up) >= 0.0f ? 1.0f : -1.0f;
        const StationScale scale = scaleOfStation(laterStation, profile, params.widthScale);
        const Vector4 color = flowColor(laterStation, params);

        const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        const auto emitCorner = [&](uint32_t ring, uint32_t point){
            SplineVertex vertex;
            vertex.position = ringPosition(ring, point);
            vertex.normal = riserNormal;
            const auto [lateralScale, verticalScale] = scaleForPoint(scale, profile.points[point].position.x);
            const float riserV = uvs.heightV ? vertex.position.y - uvs.heightVDatumMetres
                                             : profile.points[point].position.y * verticalScale;
            vertex.uv = Vector2(profile.points[point].position.x * lateralScale * uvs.tilesPerMetreV,
                                riserV * uvs.tilesPerMetreV);
            vertex.tangent = Vector4(lateral.x, lateral.y, lateral.z, handedness);
            vertex.color = color;
            mesh.vertices.push_back(vertex);
        };
        for (const uint32_t point : raised){
            emitCorner(earlier, point);
        }
        for (auto it = raised.rbegin(); it != raised.rend(); ++it){
            emitCorner(earlier + 1u, *it);
        }

        const uint32_t corners = static_cast<uint32_t>(raised.size()) * 2u;
        const Vector3 fanNormal = crossOf(mesh.vertices[base + 1u].position - mesh.vertices[base].position,
                                          mesh.vertices[base + 2u].position - mesh.vertices[base].position);
        const bool flip = dotOf(fanNormal, riserNormal) < 0.0f;
        for (uint32_t k = 1u; k + 1u < corners; k++){
            appendTriangle(mesh.indices, mesh.vertices, base, base + (flip ? k + 1u : k), base + (flip ? k : k + 1u));
        }
    }

    // the U density at which `length` metres span a whole number of tiles, nearest tilesPerMetre
    float wholeTileDensity(float length, float tilesPerMetre){
        if (!(length > MIN_EDGE_LENGTH) || !(tilesPerMetre > 0.0f)){
            return tilesPerMetre;
        }
        return std::max(1.0f, std::round(length * tilesPerMetre)) / length;
    }

    // a Crease ring whose top stands at another height than the ring before it: a riser closes it
    bool stepsAt(const SplineStation& earlier, const SplineStation& later){
        return later.join == SplineStationJoin::Crease && std::abs(later.topOffset - earlier.topOffset) > MIN_EDGE_LENGTH;
    }

    std::vector<uint32_t> raisedPoints(const SplineProfile& profile){
        std::vector<uint32_t> raised;
        if (profile.closed){
            for (uint32_t j = 0; j < profile.points.size(); j++){
                if (profile.points[j].grounded == 0u){
                    raised.push_back(j);
                }
            }
        }
        if (raised.size() < 2u){
            raised.clear();
        }
        return raised;
    }

    bool capsProfile(const SplineProfile& profile){
        return profile.closed && !profile.capTriangles.empty();
    }

    void resetMesh(SplineStripMesh& mesh){
        mesh.vertices.clear();
        mesh.indices.clear();
        mesh.bounds = AABB();
        mesh.ringVertexCount = 0;
        mesh.ringCount = 0;
        mesh.droppedStations = 0;
    }

    void sweep(const SplineProfile& profile, std::span<const SplineStation> stations, const SplineStripParams& params,
               bool capFront, bool capBack, SplineStripMesh& mesh){
        resetMesh(mesh);
        if (!profile.isValid()){
            return;
        }

        std::vector<SplineStation> walk;
        filterStations(stations, walk, nullptr);
        mesh.droppedStations = static_cast<uint32_t>(stations.size() - walk.size());
        if (walk.size() < 2u){
            return;
        }

        const ProfileTopology topo = buildTopology(profile);
        if (topo.slots.empty() || topo.quads.empty()){
            return;
        }

        const uint32_t pointCount = static_cast<uint32_t>(profile.points.size());
        const uint32_t ringVertexCount = static_cast<uint32_t>(topo.slots.size());
        const uint32_t ringCount = static_cast<uint32_t>(walk.size());
        mesh.ringVertexCount = ringVertexCount;
        mesh.ringCount = ringCount;

        // A finite uOriginMetres puts every chunk of a split run on one U axis; the default rebases
        // to this span's own front. The closed-loop snap rounds against THIS span's length.
        const float frontDistance = walk.front().distance;
        const float originDistance = std::isfinite(params.uOriginMetres) ? params.uOriginMetres : frontDistance;
        const float totalLength = walk.back().distance - frontDistance;

        float tilesPerMetreU = std::isfinite(params.tilesPerMetreU) ? params.tilesPerMetreU : 1.0f;
        const float tilesPerMetreV = std::isfinite(params.tilesPerMetreV) ? params.tilesPerMetreV : 1.0f;
        const bool heightV = std::isfinite(params.heightVDatumMetres);
        const bool faceSnap = std::isfinite(params.loopLengthMetres);
        if (params.snapUForClosedLoop && totalLength > MIN_EDGE_LENGTH && tilesPerMetreU > 0.0f){
            // a loop's length is rarely a whole tile count, and the sub-1% density change this costs
            // is invisible where a partial tile at the seam is not
            tilesPerMetreU = std::max(1.0f, std::round(totalLength * tilesPerMetreU)) / totalLength;
        }
        const float lateralOffset = std::isfinite(params.lateralOffset) ? params.lateralOffset : 0.0f;
        const float verticalOffset = std::isfinite(params.verticalOffset) ? params.verticalOffset : 0.0f;

        const std::vector<uint32_t> raised = raisedPoints(profile);
        const bool caps = capsProfile(profile);
        uint32_t risers = 0;
        if (!raised.empty()){
            for (uint32_t r = 0; r + 1u < ringCount; r++){
                risers += stepsAt(walk[r], walk[r + 1u]) ? 1u : 0u;
            }
        }
        const uint32_t capCount = (caps && capFront ? 1u : 0u) + (caps && capBack ? 1u : 0u);
        mesh.vertices.reserve(static_cast<size_t>(ringVertexCount) * ringCount + static_cast<size_t>(risers) * raised.size() * 2u +
                              static_cast<size_t>(capCount) * pointCount);
        mesh.indices.reserve(topo.quads.size() * (ringCount - 1u) * 6u +
                             static_cast<size_t>(risers) * (raised.size() * 2u - 2u) * 3u +
                             static_cast<size_t>(capCount) * profile.capTriangles.size());
        mesh.vertices.resize(static_cast<size_t>(ringVertexCount) * ringCount);

        // scratch reused per station: the scaled cross-section, its edge normals and cumulative
        // perimeter all change when the width channel does
        std::vector<Planar> scaledPoints(pointCount);
        std::vector<Planar> edgeNormals(topo.edgeCount);
        std::vector<float> pointV(pointCount, 0.0f);

        for (uint32_t r = 0; r < ringCount; r++){
            const SplineStation& station = walk[r];
            const StationScale scale = scaleOfStation(station, profile, params.widthScale);

            for (uint32_t j = 0; j < pointCount; j++){
                const auto [lateralScale, verticalScale] = scaleForPoint(scale, profile.points[j].position.x);
                scaledPoints[j] = Planar{profile.points[j].position.x * lateralScale, profile.points[j].position.y * verticalScale};
            }

            float perimeter = 0.0f;
            for (uint32_t e = 0; e < topo.edgeCount; e++){
                const uint32_t a = e;
                const uint32_t b = (e + 1u) % pointCount;
                const Planar delta{scaledPoints[b].x - scaledPoints[a].x, scaledPoints[b].y - scaledPoints[a].y};
                const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
                // a pinched-to-nothing edge keeps the unscaled profile's direction, so its face
                // still has a normal to shade with
                const Planar unscaled{profile.points[b].position.x - profile.points[a].position.x,
                                      profile.points[b].position.y - profile.points[a].position.y};
                const Planar dir = (length > MIN_EDGE_LENGTH) ? Planar{delta.x / length, delta.y / length}
                                                              : normalizedOr2D(unscaled, Planar{1.0f, 0.0f});
                // outward: the edge direction rotated +90 degrees in (lateral, vertical)
                edgeNormals[e] = normalizedOr2D(Planar{-dir.y, dir.x}, Planar{0.0f, 1.0f});

                if (b != 0u){
                    pointV[b] = pointV[a] + length;
                }
                perimeter = pointV[a] + length;
            }

            const RingBasis basis = rolledBasis(station);
            const Vector3& right = basis.right;
            const Vector3& up = basis.up;
            const Vector3 forward = normalizedOr(station.forward, FALLBACK_FORWARD);
            const float u = (station.distance - originDistance) * tilesPerMetreU;
            const uint8_t offsetSide = (lateralOffset < 0.0f) ? SPLINE_MITRE_LEFT_SIDE : SPLINE_MITRE_RIGHT_SIDE;
            const Vector3& offsetAxis = (station.mitreSides & offsetSide) != 0u ? basis.mitreRight : right;
            // no offset adds nothing at all, so an offset-free sweep keeps its arithmetic to the bit
            const Vector3 origin = (lateralOffset == 0.0f && verticalOffset == 0.0f)
                                       ? station.position
                                       : station.position + offsetAxis * lateralOffset + up * verticalOffset;
            const Vector4 color = flowColor(station, params);

            for (uint32_t slotIndex = 0; slotIndex < ringVertexCount; slotIndex++){
                const RingSlot& slot = topo.slots[slotIndex];
                const Planar& local = scaledPoints[slot.pointIndex];
                const SplineProfilePoint& point = profile.points[slot.pointIndex];

                Planar normal2D = edgeNormals[static_cast<uint32_t>(slot.edgeA)];
                if (slot.edgeB >= 0){
                    const Planar& other = edgeNormals[static_cast<uint32_t>(slot.edgeB)];
                    // two exactly opposed edges average to zero and name no direction: one side's
                    // normal is then the only answer available
                    normal2D = normalizedOr2D(Planar{normal2D.x + other.x, normal2D.y + other.y}, normal2D);
                }

                SplineVertex& vertex = mesh.vertices[static_cast<size_t>(r) * ringVertexCount + slotIndex];
                const uint8_t side = (local.x < 0.0f) ? SPLINE_MITRE_LEFT_SIDE : SPLINE_MITRE_RIGHT_SIDE;
                const Vector3& lateralAxis = (station.mitreSides & side) != 0u ? basis.mitreRight : right;
                vertex.position = origin + lateralAxis * local.x + up * local.y;
                // a side face (edge normal more lateral than vertical) runs its own length along
                // the run; the top and bottom take the centreline's
                const bool sideFace = std::abs(normal2D.x) > std::abs(normal2D.y);
                if (point.grounded != 0u && station.groundOffset != 0.0f){
                    vertex.position = vertex.position + up * station.groundOffset;
                }
                if (point.grounded == 0u && station.topOffset != 0.0f){
                    vertex.position = vertex.position + up * station.topOffset;
                }
                vertex.normal = normalizedOr(right * normal2D.x + up * normal2D.y, FALLBACK_UP);
                float v = slot.seamV ? perimeter : pointV[slot.pointIndex];
                // U runs along forward, so the tangent IS forward; V runs along the profile edge,
                // which is -cross(normal, forward) in this frame, so the handedness is -1
                float handedness = -1.0f;
                if (heightV && sideFace){
                    // a side face measures V up from the datum instead: up is cross(normal,
                    // forward) on the right face and its opposite on the left
                    v = vertex.position.y - params.heightVDatumMetres;
                    handedness = normal2D.x > 0.0f ? 1.0f : -1.0f;
                }
                float faceU = u;
                const float faceTurn = !sideFace ? 0.0f
                                                 : (side == SPLINE_MITRE_LEFT_SIDE ? station.faceTurnLeft : station.faceTurnRight);
                // The top and bottom take the centreline U plus the arc the top has swept around
                // round corners so far, and on a corner ring that U projected along the ring's
                // forward, so the bands into and out of every corner run at true scale on both edges.
                const float projected = sideFace ? 0.0f
                    : (station.topArcAllowance + (station.mitreSides == 0u ? 0.0f : dotOf(lateralAxis * local.x, forward))) *
                          params.lateralMetresPerUnit;
                if (faceTurn != 0.0f || faceSnap || projected != 0.0f){
                    const float lateral = std::abs(local.x) * params.lateralMetresPerUnit;
                    const float loopTurn = !sideFace ? 0.0f
                                                     : (side == SPLINE_MITRE_LEFT_SIDE ? params.loopFaceTurnLeft : params.loopFaceTurnRight);
                    const float loopAllowance = sideFace ? 0.0f : params.loopTopArcAllowance * params.lateralMetresPerUnit;
                    const float density = faceSnap ? wholeTileDensity(params.loopLengthMetres + lateral * loopTurn + loopAllowance, tilesPerMetreU)
                                                   : tilesPerMetreU;
                    faceU = (station.distance - originDistance + lateral * faceTurn + projected) * density;
                }
                vertex.uv = Vector2(faceU, v * tilesPerMetreV);
                vertex.tangent = Vector4(forward.x, forward.y, forward.z, handedness);
                vertex.color = color;
            }
        }

        // Stitch each ring to the next. Ring r's vertices are referenced by the quad behind it and
        // the quad ahead of it: that shared reference IS the weld.
        for (uint32_t r = 0; r + 1u < ringCount; r++){
            // a crease repeats its ring's positions: a band to it would have no area
            if (walk[r + 1u].join == SplineStationJoin::Crease){
                continue;
            }
            const uint32_t base = r * ringVertexCount;
            const uint32_t next = (r + 1u) * ringVertexCount;
            for (const auto& [slotA, slotB] : topo.quads){
                appendBandQuad(mesh.indices, mesh.vertices, base + slotA, base + slotB, next + slotB, next + slotA);
            }
        }

        // any slot of a point carries the same position, so risers and caps built from one per
        // point reuse the ring's own vertices and cannot drift off the strip
        std::vector<uint32_t> firstSlotOfPoint(pointCount, 0u);
        for (uint32_t slotIndex = ringVertexCount; slotIndex-- > 0u;){
            firstSlotOfPoint[topo.slots[slotIndex].pointIndex] = slotIndex;
        }

        const EndFaceUV uvs{tilesPerMetreV, heightV, params.heightVDatumMetres};
        if (risers > 0u){
            for (uint32_t r = 0; r + 1u < ringCount; r++){
                if (stepsAt(walk[r], walk[r + 1u])){
                    emitRiser(mesh, profile, raised, firstSlotOfPoint, r, walk[r], walk[r + 1u], params, uvs);
                }
            }
        }

        // End caps close a solid cross-section; an open strip has no polygon to triangulate.
        if (capCount > 0u){
            // the cap triangles' winding in (lateral, vertical): every triangle turns the same way
            const auto& tri = profile.capTriangles;
            const Vector2& p0 = profile.points[tri[0]].position;
            const Vector2& p1 = profile.points[tri[1]].position;
            const Vector2& p2 = profile.points[tri[2]].position;
            const float orientation = ((p1.x - p0.x) * (p2.y - p0.y) - (p1.y - p0.y) * (p2.x - p0.x)) > 0.0f ? 1.0f : -1.0f;

            const auto emitCap = [&](uint32_t ring, bool front){
                const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
                const SplineStation& station = walk[ring];
                const StationScale scale = scaleOfStation(station, profile, params.widthScale);
                const RingBasis basis = rolledBasis(station);
                const Vector3 forward = normalizedOr(station.forward, FALLBACK_FORWARD);
                const Vector3 capNormal = front ? forward * -1.0f : forward;
                const Vector3 capRight = normalizedOr(basis.right, FALLBACK_RIGHT);
                // U runs across (right) and V up: cross(normal, right) * w points up
                const float handedness = dotOf(crossOf(capNormal, capRight), basis.up) >= 0.0f ? 1.0f : -1.0f;
                const Vector4 color = flowColor(station, params);

                for (uint32_t j = 0; j < pointCount; j++){
                    SplineVertex vertex;
                    vertex.position = mesh.vertices[static_cast<size_t>(ring) * ringVertexCount + firstSlotOfPoint[j]].position;
                    vertex.normal = capNormal;
                    // the cap's UVs are its own SCALED cross-section in metres, so a capped end tiles
                    // at the density of the sides
                    const auto [lateralScale, verticalScale] = scaleForPoint(scale, profile.points[j].position.x);
                    const float capV = heightV ? vertex.position.y - params.heightVDatumMetres
                                               : profile.points[j].position.y * verticalScale;
                    vertex.uv = Vector2(profile.points[j].position.x * lateralScale * tilesPerMetreV, capV * tilesPerMetreV);
                    vertex.tangent = Vector4(capRight.x, capRight.y, capRight.z, handedness);
                    vertex.color = color;
                    mesh.vertices.push_back(vertex);
                }

                // a profile triangle (a, b, c) of winding o lands with geometric normal along
                // o * cross(right, up): kept as authored when that faces the cap's way
                const bool keep = orientation * dotOf(crossOf(basis.right, basis.up), capNormal) > 0.0f;
                for (size_t t = 0; t + 2u < tri.size(); t += 3u){
                    if (keep){
                        appendTriangle(mesh.indices, mesh.vertices, base + tri[t], base + tri[t + 1u], base + tri[t + 2u]);
                    }else{
                        appendTriangle(mesh.indices, mesh.vertices, base + tri[t], base + tri[t + 2u], base + tri[t + 1u]);
                    }
                }
            };
            if (caps && capFront){
                emitCap(0u, true);
            }
            if (caps && capBack){
                emitCap(ringCount - 1u, false);
            }
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
    }

}

void spline::buildSplineStrip(const SplineProfile& profile, std::span<const SplineStation> stations,
                              const SplineStripParams& params, SplineStripMesh& mesh){
    sweep(profile, stations, params, params.generateCaps, params.generateCaps, mesh);
}

SplineStripMesh spline::buildSplineStrip(const SplineProfile& profile, std::span<const SplineStation> stations,
                                         const SplineStripParams& params){
    SplineStripMesh mesh;
    buildSplineStrip(profile, stations, params, mesh);
    return mesh;
}

std::vector<SplineStripChunkRange> spline::planSplineStripChunks(const SplineProfile& profile,
        std::span<const SplineStation> stations, const SplineStripParams& params, float chunkLength, uint32_t maxVertices){
    std::vector<SplineStripChunkRange> chunks;
    if (!profile.isValid()){
        return chunks;
    }
    std::vector<SplineStation> kept;
    std::vector<uint32_t> keptIndex;
    keptIndex.reserve(stations.size());
    filterStations(stations, kept, &keptIndex);
    const size_t count = kept.size();
    if (count < 2u){
        return chunks;
    }
    const ProfileTopology topo = buildTopology(profile);
    if (topo.slots.empty() || topo.quads.empty()){
        return chunks;
    }

    const uint64_t ringVertices = topo.slots.size();
    const uint64_t riserVertices = raisedPoints(profile).size() * 2u;
    const uint64_t capVertices = (capsProfile(profile) && params.generateCaps) ? profile.points.size() : 0u;
    const float length = (std::isfinite(chunkLength) && chunkLength > 0.0f) ? chunkLength : SPLINE_STRIP_CHUNK_LENGTH;
    const uint64_t limit = maxVertices;
    const float front = kept.front().distance;

    // a boundary ring joins plainly on both sides: no crease, pivot or mitre to split, and no
    // riser across it
    const auto plain = [&](size_t k){
        return k > 0u && k + 1u < count && kept[k].join == SplineStationJoin::Advance && kept[k].mitreSides == 0u &&
               kept[k + 1u].join == SplineStationJoin::Advance;
    };
    const auto markAfter = [&](float distance){
        return front + length * (std::floor((distance - front) / length) + 1.0f);
    };
    const auto emit = [&](size_t first, size_t last){
        SplineStripChunkRange chunk;
        chunk.firstStation = keptIndex[first];
        chunk.endStation = keptIndex[last] + 1u;
        chunk.startDistance = kept[first].distance;
        chunk.endDistance = kept[last].distance;
        chunk.opensRun = first == 0u;
        chunk.closesRun = last + 1u == count;
        chunks.push_back(chunk);
    };

    size_t start = 0;
    uint64_t vertices = ringVertices + capVertices;
    size_t lastPlain = 0;
    float nextMark = markAfter(front);
    size_t k = 1;
    while (k < count){
        const uint64_t add = ringVertices + (riserVertices > 0u && stepsAt(kept[k - 1u], kept[k]) ? riserVertices : 0u) +
                             (k + 1u == count ? capVertices : 0u);
        if (vertices + add > limit && k > start + 1u){
            // full: end at the last plain ring, or (a corner alone past the limit) the ring before
            const size_t cut = lastPlain > start ? lastPlain : k - 1u;
            emit(start, cut);
            start = cut;
            vertices = ringVertices;
            lastPlain = 0;
            if (kept[start].distance >= nextMark){
                nextMark = markAfter(kept[start].distance);
            }
            k = cut + 1u;
            continue;
        }
        vertices += add;
        if (plain(k)){
            if (kept[k].distance >= nextMark){
                emit(start, k);
                start = k;
                vertices = ringVertices;
                lastPlain = 0;
                nextMark = markAfter(kept[k].distance);
            }else{
                lastPlain = k;
            }
        }
        k++;
    }
    emit(start, count - 1u);
    return chunks;
}

void spline::buildSplineStripChunk(const SplineProfile& profile, std::span<const SplineStation> stations,
                                   const SplineStripChunkRange& chunk, const SplineStripParams& params, SplineStripMesh& mesh){
    if (chunk.firstStation >= chunk.endStation || chunk.endStation > stations.size()){
        resetMesh(mesh);
        return;
    }
    // the run's U axis: its origin and closed-loop density, from the run's first and last rings
    bool any = false;
    float front = 0.0f;
    float back = 0.0f;
    walkKeptStations(stations, [&](size_t, float distance, bool first){
        if (first){
            front = distance;
        }
        back = distance;
        any = true;
    });
    SplineStripParams resolved = params;
    if (any && !std::isfinite(resolved.uOriginMetres)){
        resolved.uOriginMetres = front;
    }
    if (params.snapUForClosedLoop){
        const float total = back - front;
        const float tiles = std::isfinite(params.tilesPerMetreU) ? params.tilesPerMetreU : 1.0f;
        if (total > MIN_EDGE_LENGTH && tiles > 0.0f){
            resolved.tilesPerMetreU = std::max(1.0f, std::round(total * tiles)) / total;
        }
        resolved.snapUForClosedLoop = false;
    }
    sweep(profile, stations.subspan(chunk.firstStation, chunk.endStation - chunk.firstStation), resolved,
          params.generateCaps && chunk.opensRun, params.generateCaps && chunk.closesRun, mesh);
}
