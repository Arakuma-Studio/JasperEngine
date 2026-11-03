// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Tests/SplineWallWalk.h, Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.
//
// Station walks, wall profiles and mesh probes shared by the spline geometry tests, and the
// well-formedness checks every builder test runs (S18 AC5, D6): each triangle's geometric normal
// agrees with its vertex normals, no edge is shared by more than two triangles, and each tangent
// frame matches the UVs (tangent along +U, cross(normal, tangent) * w along +V).
//
// The test data is OpenEngine's mirrored into this right-handed frame by z -> -z: OpenEngine's
// runs along +Z with right +X become runs along -Z with right = forward x up = +X, so lateral
// coordinates and "right of travel" keep their meaning and every expected z changes sign.

#ifndef SPLINE_GEOMETRY_TEST_SUPPORT_H
#define SPLINE_GEOMETRY_TEST_SUPPORT_H

#include "spline/SplineCorner.h"
#include "spline/SplineProfile.h"
#include "spline/SplineStation.h"
#include "spline/SplineStripBuilder.h"
#include "spline/SplineVertex.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine{ namespace spline{ namespace geometrytest{

    inline constexpr float PI = std::numbers::pi_v<float>;
    inline const Vector3 UP(0.0f, 1.0f, 0.0f);

    inline Vector3 crossOf(const Vector3& a, const Vector3& b){
        return Vector3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    }

    inline float dotOf(const Vector3& a, const Vector3& b){
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    inline float lengthOf(const Vector3& v){
        return std::sqrt(dotOf(v, v));
    }

    inline Vector3 unit(const Vector3& v){
        return v * (1.0f / lengthOf(v));
    }

    inline bool bitsEqual(float a, float b){
        return std::memcmp(&a, &b, sizeof(float)) == 0;
    }

    inline bool bitsEqual(const Vector3& a, const Vector3& b){
        return std::memcmp(&a, &b, sizeof(Vector3)) == 0;
    }

    inline bool vertexBitsEqual(const SplineVertex& a, const SplineVertex& b){
        return bitsEqual(a.position, b.position) && bitsEqual(a.normal, b.normal) && bitsEqual(a.uv.x, b.uv.x) &&
               bitsEqual(a.uv.y, b.uv.y) && bitsEqual(a.tangent.x, b.tangent.x) && bitsEqual(a.tangent.y, b.tangent.y) &&
               bitsEqual(a.tangent.z, b.tangent.z) && bitsEqual(a.tangent.w, b.tangent.w) &&
               bitsEqual(a.color.x, b.color.x) && bitsEqual(a.color.y, b.color.y) && bitsEqual(a.color.z, b.color.z) &&
               bitsEqual(a.color.w, b.color.w);
    }

    // the right of travel of a level-projected forward: forward x up
    inline Vector3 rightOf(const Vector3& forward){
        return unit(crossOf(Vector3(forward.x, 0.0f, forward.z), UP));
    }

    // A level polyline walked at `step` metres the way the extrude samples a Linear spline once its
    // authored points are stations: every authored point lands on a station, and each station looks
    // along the central difference of its neighbours.
    struct Walk{
        std::vector<SplineStation> stations;
        std::vector<SplineCornerSite> corners;
    };

    inline Walk walkPolyline(const std::vector<Vector3>& points, float step, float halfWidth){
        Walk walk;
        std::vector<Vector3> positions;
        for (size_t leg = 0; leg + 1u < points.size(); leg++){
            const Vector3 from = points[leg];
            const Vector3 to = points[leg + 1u];
            const uint32_t pieces = std::max(1u, static_cast<uint32_t>(std::ceil(lengthOf(to - from) / step)));
            if (leg > 0u){
                walk.corners.push_back({static_cast<uint32_t>(positions.size()), static_cast<uint32_t>(leg)});
            }
            for (uint32_t i = 0; i < pieces; i++){
                positions.push_back(from + (to - from) * (static_cast<float>(i) / static_cast<float>(pieces)));
            }
        }
        positions.push_back(points.back());

        float travelled = 0.0f;
        walk.stations.resize(positions.size());
        for (size_t i = 0; i < positions.size(); i++){
            if (i > 0u){
                travelled += lengthOf(positions[i] - positions[i - 1u]);
            }
            const Vector3 ahead = positions[std::min(i + 1u, positions.size() - 1u)];
            const Vector3 behind = positions[i == 0u ? 0u : i - 1u];
            SplineStation& s = walk.stations[i];
            s.position = positions[i];
            s.forward = unit(ahead - behind);
            s.right = rightOf(s.forward);
            s.up = crossOf(s.right, s.forward);
            s.halfWidthLeft = halfWidth;
            s.halfWidthRight = halfWidth;
            s.distance = travelled;
        }
        return walk;
    }

    inline SplineProfile wallProfile(float thickness, float height){
        SplineProfileParams params;
        params.shape = SplineProfileShape::Rectangle;
        params.thickness = thickness;
        params.height = height;
        return buildProfile(params);
    }

    inline SplineProfile flatRibbon(float width){
        SplineProfileParams params;
        params.shape = SplineProfileShape::Flat;
        params.width = width;
        return buildProfile(params);
    }

    // Two legs from the origin: `legA` metres along -Z, then `legB` metres along a direction turned
    // `turnDegrees` to the right of -Z (toward +X).
    inline std::vector<Vector3> elbow(float legA, float legB, float turnDegrees){
        const float turn = turnDegrees * PI / 180.0f;
        const Vector3 corner(0.0f, 0.0f, -legA);
        return {Vector3(0.0f, 0.0f, 0.0f), corner, corner + Vector3(std::sin(turn), 0.0f, -std::cos(turn)) * legB};
    }

    // the bottom vertex on each side of ring `r`, by its lateral coordinate in the ring's own frame
    struct RingBase{
        Vector3 left;
        Vector3 right;
    };

    inline RingBase baseOf(const SplineStripMesh& mesh, uint32_t r, const Vector3& origin, const Vector3& right){
        RingBase base{};
        float least = 1.0e9f;
        float most = -1.0e9f;
        for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
            const Vector3& p = mesh.vertices[r * mesh.ringVertexCount + k].position;
            if (p.y > 0.5f){
                continue;
            }
            const float lateral = dotOf(p - origin, right);
            if (lateral < least){
                least = lateral;
                base.left = p;
            }
            if (lateral > most){
                most = lateral;
                base.right = p;
            }
        }
        return base;
    }

    // ring triangles whose winding disagrees with their normals, among side faces (lateral normal)
    // or top faces (normal up): a face folded back through itself
    inline uint32_t foldedRingTriangles(const SplineStripMesh& mesh, bool topFaces){
        uint32_t folded = 0;
        const uint32_t ringVertices = mesh.ringCount * mesh.ringVertexCount;
        for (size_t t = 0; t + 2u < mesh.indices.size(); t += 3u){
            if (mesh.indices[t] >= ringVertices){
                continue;
            }
            const SplineVertex& a = mesh.vertices[mesh.indices[t]];
            const SplineVertex& b = mesh.vertices[mesh.indices[t + 1u]];
            const SplineVertex& c = mesh.vertices[mesh.indices[t + 2u]];
            if (topFaces ? a.normal.y < 0.5f : std::abs(a.normal.y) > 0.5f){
                continue;
            }
            const Vector3 geometric = crossOf(b.position - a.position, c.position - a.position);
            if (lengthOf(geometric) < 1.0e-8f){
                continue;
            }
            if (dotOf(geometric, a.normal + b.normal + c.normal) < 0.0f){
                folded++;
            }
        }
        return folded;
    }

    inline uint32_t invertedSideTriangles(const SplineStripMesh& mesh){
        return foldedRingTriangles(mesh, false);
    }

    inline uint32_t foldedTopTriangles(const SplineStripMesh& mesh){
        return foldedRingTriangles(mesh, true);
    }

    inline SplineStripMesh sweepFromZero(const SplineProfile& profile, const std::vector<SplineStation>& stations){
        SplineStripParams params;
        params.uOriginMetres = 0.0f;
        return buildSplineStrip(profile, stations, params);
    }

    // What the well-formedness check found, so a test reports every class of failure at once.
    struct MeshFindings{
        uint32_t triangles = 0;
        uint32_t badIndices = 0;
        uint32_t degenerate = 0;
        // a vertex normal at or past 90 degrees from its triangle's geometric normal (D6)
        uint32_t normalDisagreements = 0;
        // an edge (by vertex index) used by three or more triangles
        uint32_t overSharedEdges = 0;
        // tangent not along +U, or cross(normal, tangent) * w not along +V, where the triangle's
        // UVs name both directions
        uint32_t tangentMismatches = 0;
    };

    // `checkTangents` off for meshes whose V names no direction across the surface (the water
    // fill's uv0.y is a depth)
    inline MeshFindings inspectMesh(std::span<const SplineVertex> vertices, std::span<const uint32_t> indices,
                                    bool checkTangents){
        MeshFindings findings;
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> edgeUses;
        for (size_t t = 0; t + 2u < indices.size(); t += 3u){
            findings.triangles++;
            const uint32_t i[3] = {indices[t], indices[t + 1u], indices[t + 2u]};
            if (i[0] >= vertices.size() || i[1] >= vertices.size() || i[2] >= vertices.size()){
                findings.badIndices++;
                continue;
            }
            for (int e = 0; e < 3; e++){
                const uint32_t a = i[e];
                const uint32_t b = i[(e + 1) % 3];
                edgeUses[a < b ? std::make_pair(a, b) : std::make_pair(b, a)]++;
            }
            const SplineVertex& a = vertices[i[0]];
            const SplineVertex& b = vertices[i[1]];
            const SplineVertex& c = vertices[i[2]];
            const Vector3 e1 = b.position - a.position;
            const Vector3 e2 = c.position - a.position;
            const Vector3 geometric = crossOf(e1, e2);
            const float area = lengthOf(geometric);
            const float scale = std::max(dotOf(e1, e1), dotOf(e2, e2));
            if (!(area > 1.0e-7f * scale) || !(area > 0.0f)){
                findings.degenerate++;
                continue;
            }
            for (const SplineVertex* v : {&a, &b, &c}){
                if (!(dotOf(geometric, v->normal) > 0.0f)){
                    findings.normalDisagreements++;
                }
            }
            if (!checkTangents){
                continue;
            }
            // dP/du and dP/dv of the triangle from its UVs
            const float du1 = b.uv.x - a.uv.x;
            const float dv1 = b.uv.y - a.uv.y;
            const float du2 = c.uv.x - a.uv.x;
            const float dv2 = c.uv.y - a.uv.y;
            const float det = du1 * dv2 - du2 * dv1;
            if (std::abs(det) < 1.0e-8f){
                continue;
            }
            const Vector3 dPdu = (e1 * dv2 - e2 * dv1) * (1.0f / det);
            const Vector3 dPdv = (e2 * du1 - e1 * du2) * (1.0f / det);
            if (lengthOf(dPdu) < 1.0e-6f || lengthOf(dPdv) < 1.0e-6f){
                continue;
            }
            for (const SplineVertex* v : {&a, &b, &c}){
                const Vector3 tangent(v->tangent.x, v->tangent.y, v->tangent.z);
                const Vector3 bitangent = crossOf(v->normal, tangent) * v->tangent.w;
                if (!(dotOf(tangent, dPdu) > 0.0f) || !(dotOf(bitangent, dPdv) > 0.0f)){
                    findings.tangentMismatches++;
                }
            }
        }
        for (const auto& [edge, uses] : edgeUses){
            if (uses > 2u){
                findings.overSharedEdges++;
            }
        }
        return findings;
    }

    // The AC5 / D6 assertion: no bad index, no zero-area triangle, every vertex normal on the side
    // its triangle faces, no edge shared by more than two triangles, tangent frames matching the UVs.
    inline void expectWellFormed(std::span<const SplineVertex> vertices, std::span<const uint32_t> indices,
                                 const std::string& what, bool checkTangents = true){
        const MeshFindings findings = inspectMesh(vertices, indices, checkTangents);
        EXPECT_GT(findings.triangles, 0u) << what;
        EXPECT_EQ(findings.badIndices, 0u) << what;
        EXPECT_EQ(findings.degenerate, 0u) << what << ": zero-area triangles";
        EXPECT_EQ(findings.normalDisagreements, 0u) << what << ": triangles wound against their normals";
        EXPECT_EQ(findings.overSharedEdges, 0u) << what << ": edges shared by more than two triangles";
        EXPECT_EQ(findings.tangentMismatches, 0u) << what << ": tangent frames disagreeing with the UVs";
    }

    inline void expectWellFormed(const SplineStripMesh& mesh, const std::string& what){
        expectWellFormed(mesh.vertices, mesh.indices, what);
    }

}}}

#endif // SPLINE_GEOMETRY_TEST_SUPPORT_H
