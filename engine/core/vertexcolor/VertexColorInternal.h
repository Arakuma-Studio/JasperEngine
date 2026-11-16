// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R27: what the vertexcolor sources share and nothing outside them
// includes. Vector and matrix maths are written here rather than called in engine/core/math, whose
// sources build with the platform's floating-point contraction: the module builds without it
// (VertexColorSources.cmake), so these give the same bits on every target. Powers and
// trigonometry are fixed series in double precision, never libm.

#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "Export.h"
#include "vertexcolor/VertexColorBake.h"
#include "vertexcolor/VertexColorMesh.h"
#include "vertexcolor/VertexColorOps.h"
#include "vertexcolor/VertexColorTypes.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace jasperengine::vertexcolor::detail {

    constexpr double kPi = 3.14159265358979323846;

    inline float dot3(const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Vector3 cross3(const Vector3& a, const Vector3& b) {
        return Vector3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    }
    float length3(const Vector3& v);
    // zero for a zero (or non-finite) vector
    Vector3 normalize3(const Vector3& v);

    double detSin(double radians);
    double detCos(double radians);
    // x^y for x >= 0 (0 for x <= 0, 1 for y == 0)
    double detPow(double x, double y);
    // the angle of (x, y) in [-pi, pi]
    double detAtan2(double y, double x);

    // A transform of points and normals: identity (local space) or the target's model (D12)
    struct Space {
        float m[16];        // [column][row], as Matrix4
        float normal[9];    // cofactor of the linear part (normals: inverse transpose up to scale), column major
        bool identity = true;

        static Space of(const Matrix4& model);
        static Space local();
        Vector3 point(const Vector3& p) const;
        // unit (zero for a zero normal)
        Vector3 direction(const Vector3& n) const;
    };

    // rotation matrix rows of a quaternion: out = R * v and R^T * v without libm
    Vector3 rotate(const Quaternion& q, const Vector3& v);
    Vector3 rotateInverse(const Quaternion& q, const Vector3& v);
    // Unity's order (z, then x, then y), degrees
    Quaternion eulerDegrees(const Vector3& degrees);

    // HSV of the engine's display encoding (D4: what the color field shows), h in [0, 1)
    struct Hsv {
        float h = 0.0f, s = 0.0f, v = 0.0f;
    };
    Hsv toHsv(const Vector4& linear);
    Vector4 fromHsv(const Hsv& hsv, float alpha);

    // R23 color target / R52 Similar Color / R49 replace_color (ColorMatch::tolerance's rule)
    bool colorMatches(const Vector4& value, const ColorMatch& match);

    // Dense corner values of a target: overrides where present, else the vertex's base value.
    // A target whose base does not cover the mesh reads as white.
    void denseValues(const TargetData& data, const MeshView& mesh, std::vector<Vector4>& base, std::vector<Vector4>& corners);
    // the mean of a logical vertex's corner values (its members' base values when no triangle uses it)
    Vector4 logicalMean(const MeshView& mesh, const std::vector<Vector4>& base, const std::vector<Vector4>& corners, uint32_t logical);

    // FNV-1a 64: stable on every platform (little-endian bytes of the values)
    constexpr uint64_t kFnvOffset = 1469598103934665603ull;
    uint64_t fnvU32(uint64_t hash, uint32_t value);
    uint64_t fnvFloat(uint64_t hash, float value);

    // applyOps with the generator ops' field given instead of computed: the tests reach R41's seam
    // rule and R60's unmatched elements through it without a texture or another mesh
    JASPERENGINE_API OpsResult applyOpsWithField(TargetData& data, const MeshView& mesh, std::span<const Op> ops, const ApplyOptions& options,
                                                 const bake::Field& field, bool dryRun = false);

    inline uint32_t sortedUniqueCount(std::vector<uint32_t>& ids) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        return (uint32_t)ids.size();
    }

}
