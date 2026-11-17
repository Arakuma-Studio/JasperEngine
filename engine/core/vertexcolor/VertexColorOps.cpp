// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vertexcolor/VertexColorOps.h"

#include "noise/NoiseKernels.h"
#include "noise/NoiseSwitch.h"
#include "pixelart/PixelColor.h"
#include "vertexcolor/VertexColorBake.h"
#include "vertexcolor/VertexColorInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace jasperengine::vertexcolor {

using detail::cross3;
using detail::dot3;
using detail::length3;
using detail::normalize3;

namespace {

    struct OpsNameEntry {
        OpType type;
        const char* name;
    };

    const OpsNameEntry kOpsNames[] = {
        {OpType::Set, "set"},
        {OpType::Fill, "fill"},
        {OpType::Stroke, "stroke"},
        {OpType::Gradient, "gradient"},
        {OpType::Smooth, "smooth"},
        {OpType::Sharpen, "sharpen"},
        {OpType::Noise, "noise"},
        {OpType::Adjust, "adjust"},
        {OpType::Channel, "channel"},
        {OpType::ReplaceColor, "replace_color"},
        {OpType::FromTexture, "from_texture"},
        {OpType::BakeAO, "bake_ao"},
        {OpType::BakeThickness, "bake_thickness"},
        {OpType::BakeCurvature, "bake_curvature"},
        {OpType::ByDirection, "by_direction"},
        {OpType::Transfer, "transfer"},
        {OpType::Clear, "clear"},
    };

    // a stamp's capsule more than this many stamps long is refused: the caller split it wrong
    constexpr size_t kOpsMaxStamps = 100000;
    constexpr uint32_t kOpsCornerBit = 0x80000000u;

    std::string opsNumber(const char* what, double value) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%s%g", what, value);
        return buffer;
    }

    float opsClamp01(float v) {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    float opsSmoothstep(float edge0, float edge1, float x) {
        if (edge1 == edge0) return x < edge0 ? 0.0f : 1.0f;
        const float t = opsClamp01((x - edge0) / (edge1 - edge0));
        return t * t * (3.0f - 2.0f * t);
    }

    Vector4 opsLerp4(const Vector4& a, const Vector4& b, float t) {
        return Vector4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
    }

    Vector3 opsLerp3(const Vector3& a, const Vector3& b, float t) {
        return Vector3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
    }

    // distance from p to the segment a-b and the closest point's parameter
    float opsSegmentDistance(const Vector3& p, const Vector3& a, const Vector3& b, float& t) {
        const Vector3 ab = b - a;
        const float len2 = dot3(ab, ab);
        t = len2 > 0.0f ? opsClamp01(dot3(p - a, ab) / len2) : 0.0f;
        const Vector3 d = p - opsLerp3(a, b, t);
        return length3(d);
    }

    ElementKind opsElementKind(const Target& target) {
        switch (target.kind) {
            case TargetKind::Vertices: return ElementKind::Vertex;
            case TargetKind::Faces: return ElementKind::Face;
            case TargetKind::Corners: return ElementKind::Corner;
            default: return target.element == Element::Face ? ElementKind::Face : ElementKind::Vertex;
        }
    }

    const char* opsKindNoun(ElementKind kind) {
        switch (kind) {
            case ElementKind::Vertex: return "vertices";
            case ElementKind::Face: return "faces";
            case ElementKind::Corner: return "corners";
        }
        return "elements";
    }

    float opsFalloff(const Op& op, float x) {
        if (!(x <= 1.0f)) return 0.0f;
        const float hardness = opsClamp01(op.hardness);
        if (x <= hardness) return 1.0f;
        const float t = hardness < 1.0f ? (x - hardness) / (1.0f - hardness) : 1.0f;
        switch (op.falloff) {
            case FalloffPreset::Smooth: return 1.0f - t * t * (3.0f - 2.0f * t);
            case FalloffPreset::Linear: return 1.0f - t;
            case FalloffPreset::Constant: return 1.0f;
            case FalloffPreset::Sphere: return std::sqrt(std::max(0.0f, 1.0f - t * t));
            case FalloffPreset::Custom: return op.falloffCurve.empty() ? 1.0f - t : opsClamp01(op.falloffCurve.evaluate(t));
        }
        return 0.0f;
    }

    Vector4 opsOklabLerp(const Vector4& a, const Vector4& b, float f) {
        const pixelart::Lab la = pixelart::linearToOklab(a.x, a.y, a.z);
        const pixelart::Lab lb = pixelart::linearToOklab(b.x, b.y, b.z);
        const pixelart::Lab mix{la.l + (lb.l - la.l) * f, la.a + (lb.a - la.a) * f, la.b + (lb.b - la.b) * f};
        float rgb[3];
        pixelart::oklabToLinear(mix, rgb);
        return Vector4(rgb[0], rgb[1], rgb[2], a.w + (b.w - a.w) * f);
    }

    Vector4 opsHsvLerp(const Vector4& a, const Vector4& b, float f, HueDirection direction) {
        detail::Hsv ha = detail::toHsv(a);
        detail::Hsv hb = detail::toHsv(b);
        // a gray has no hue: it takes the other end's, so a gray-to-red ramp does not sweep the wheel
        if (ha.s <= 0.0f) ha.h = hb.h;
        if (hb.s <= 0.0f) hb.h = ha.h;
        float dh = hb.h - ha.h;
        switch (direction) {
            case HueDirection::Near:
                if (dh > 0.5f) dh -= 1.0f;
                else if (dh < -0.5f) dh += 1.0f;
                break;
            case HueDirection::Far:
                if (dh > 0.0f && dh <= 0.5f) dh -= 1.0f;
                else if (dh < 0.0f && dh >= -0.5f) dh += 1.0f;
                break;
            case HueDirection::Cw:
                if (dh < 0.0f) dh += 1.0f;
                break;
            case HueDirection::Ccw:
                if (dh > 0.0f) dh -= 1.0f;
                break;
        }
        detail::Hsv mix;
        mix.h = ha.h + dh * f;
        mix.h -= std::floor(mix.h);
        mix.s = ha.s + (hb.s - ha.s) * f;
        mix.v = ha.v + (hb.v - ha.v) * f;
        return detail::fromHsv(mix, a.w + (b.w - a.w) * f);
    }

    struct OpsGradientSample {
        Vector4 value;
        float strength = 1.0f;
    };

    OpsGradientSample opsGradientAt(const std::vector<GradientStop>& stops, float u, Interpolation interpolation, HueDirection hue) {
        if (u <= stops.front().t) return OpsGradientSample{stops.front().value, stops.front().strength};
        if (u >= stops.back().t) return OpsGradientSample{stops.back().value, stops.back().strength};
        size_t i = 1;
        while (i < stops.size() && stops[i].t < u) i++;
        const GradientStop& a = stops[i - 1];
        const GradientStop& b = stops[i];
        const float span = b.t - a.t;
        const float f = span > 0.0f ? (u - a.t) / span : 1.0f;
        OpsGradientSample sample;
        sample.strength = a.strength + (b.strength - a.strength) * f;
        switch (interpolation) {
            case Interpolation::Linear: sample.value = opsLerp4(a.value, b.value, f); break;
            case Interpolation::Oklab: sample.value = opsOklabLerp(a.value, b.value, f); break;
            case Interpolation::Hsv: sample.value = opsHsvLerp(a.value, b.value, f, hue); break;
        }
        return sample;
    }

    float opsPercentile(const std::vector<float>& sorted, float p) {
        if (sorted.empty()) return 0.0f;
        const size_t index = (size_t)std::floor(p * (float)(sorted.size() - 1) + 0.5f);
        return sorted[std::min(index, sorted.size() - 1)];
    }

    uint8_t opsByte(float v) {
        return (uint8_t)std::floor(opsClamp01(v) * 255.0f + 0.5f);
    }

    // ---------------------------------------------------------------------------------------------
    // One applyOps call: the working values of a target and the change bookkeeping. The base values
    // are a copy; corners are an overlay over the stored overrides, so a stamp on a big mesh costs
    // what it touches, not the mesh (R96).
    class OpEngine {
    public:
        OpEngine(const TargetData& data, const MeshView& mesh, const ApplyOptions& options)
            : mesh(mesh), options(options), source(data) {
            fresh = data.base.empty();
            if (fresh) {
                base.resize(mesh.vertexCount());
                for (uint32_t v = 0; v < mesh.vertexCount(); v++) base[v] = initialOf(v);
            } else {
                base = data.base;
            }
            const size_t corners = mesh.indices.size();
            cornerSlot.assign(corners, -1);
            opCornerSlot.assign(corners, -1);
            opVertSlot.assign(mesh.vertexCount(), -1);
            space = options.world ? detail::Space::of(options.model) : detail::Space::local();
            worldSpace = detail::Space::of(options.model);
        }

        const MeshView& mesh;
        const ApplyOptions& options;
        const TargetData& source;
        bool fresh = false;
        bool cancelFlag = false;
        detail::Space space;
        detail::Space worldSpace;

        std::vector<Vector4> base;
        std::vector<int32_t> cornerSlot;
        std::vector<uint32_t> cornerIds;
        std::vector<Vector4> cornerValues;

        std::vector<int32_t> opVertSlot;
        std::vector<uint32_t> opVerts;
        std::vector<Vector4> opVertOld;
        std::vector<int32_t> opCornerSlot;
        std::vector<uint32_t> opCorners;
        std::vector<Vector4> opCornerOld;
        uint32_t pollCount = 0;
        // detail::applyOpsWithField: the generators' field, given
        const bake::Field* injectedField = nullptr;

        // ---- values

        Vector4 initialOf(uint32_t vertex) const {
            const Vector4* asset = vertex < mesh.assetColors.size() ? &mesh.assetColors[vertex] : nullptr;
            return initialValueFor(options.initial, options.customInitial, asset);
        }

        Vector4 corner(uint32_t c) const {
            const int32_t slot = cornerSlot[c];
            if (slot >= 0) return cornerValues[(size_t)slot];
            if (!fresh && !source.corners.empty()) {
                const auto it = std::lower_bound(source.corners.begin(), source.corners.end(), c,
                                                 [](const CornerValue& entry, uint32_t k) { return entry.corner < k; });
                if (it != source.corners.end() && it->corner == c) return it->value;
            }
            return base[mesh.indices[c]];
        }

        void setBase(uint32_t v, const Vector4& value) {
            if (opVertSlot[v] < 0) {
                opVertSlot[v] = (int32_t)opVerts.size();
                opVerts.push_back(v);
                opVertOld.push_back(base[v]);
            }
            base[v] = value;
        }

        void setCorner(uint32_t c, const Vector4& value) {
            if (opCornerSlot[c] < 0) {
                opCornerSlot[c] = (int32_t)opCorners.size();
                opCorners.push_back(c);
                opCornerOld.push_back(corner(c));
            }
            if (cornerSlot[c] < 0) {
                cornerSlot[c] = (int32_t)cornerIds.size();
                cornerIds.push_back(c);
                cornerValues.push_back(value);
            } else {
                cornerValues[(size_t)cornerSlot[c]] = value;
            }
        }

        // R4: a vertex write reaches the base value and every corner using it, so corners that end
        // equal to the base lose their override. Corners go first: one without an override reads
        // its vertex's base, which must still be the old one.
        template <class F> void applyVertex(uint32_t v, F&& fn) {
            for (uint32_t k = mesh.vertexCornerStart[v]; k < mesh.vertexCornerStart[v + 1]; k++) {
                const uint32_t c = mesh.vertexCorners[k];
                setCorner(c, fn(corner(c), v));
            }
            setBase(v, fn(base[v], v));
        }

        template <class F> void applyLogical(uint32_t l, F&& fn) {
            for (uint32_t m = mesh.memberStart[l]; m < mesh.memberStart[l + 1]; m++) applyVertex(mesh.members[m], fn);
        }

        template <class F> void applyCorner(uint32_t c, F&& fn) {
            setCorner(c, fn(corner(c), mesh.indices[c]));
        }

        template <class F> void applyElement(ElementKind kind, uint32_t id, F&& fn) {
            switch (kind) {
                case ElementKind::Vertex: applyLogical(id, fn); break;
                case ElementKind::Face:
                    for (uint32_t k = 0; k < 3; k++) applyCorner(id * 3 + k, fn);
                    break;
                case ElementKind::Corner: applyCorner(id, fn); break;
            }
        }

        Vector4 logicalValue(uint32_t l) const {
            double sum[4] = {0.0, 0.0, 0.0, 0.0};
            uint32_t count = 0;
            for (uint32_t m = mesh.memberStart[l]; m < mesh.memberStart[l + 1]; m++) {
                const uint32_t v = mesh.members[m];
                for (uint32_t k = mesh.vertexCornerStart[v]; k < mesh.vertexCornerStart[v + 1]; k++) {
                    const Vector4 value = corner(mesh.vertexCorners[k]);
                    sum[0] += value.x; sum[1] += value.y; sum[2] += value.z; sum[3] += value.w;
                    count++;
                }
            }
            if (count == 0) {
                for (uint32_t m = mesh.memberStart[l]; m < mesh.memberStart[l + 1]; m++) {
                    const Vector4& value = base[mesh.members[m]];
                    sum[0] += value.x; sum[1] += value.y; sum[2] += value.z; sum[3] += value.w;
                    count++;
                }
            }
            if (count == 0) return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
            return Vector4((float)(sum[0] / count), (float)(sum[1] / count), (float)(sum[2] / count), (float)(sum[3] / count));
        }

        Vector4 elementValue(ElementKind kind, uint32_t id) const {
            switch (kind) {
                case ElementKind::Vertex: return logicalValue(id);
                case ElementKind::Face: {
                    const Vector4 a = corner(id * 3), b = corner(id * 3 + 1), c = corner(id * 3 + 2);
                    return Vector4((a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f, (a.w + b.w + c.w) / 3.0f);
                }
                case ElementKind::Corner: return corner(id);
            }
            return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        }

        // the neighbour average of a logical vertex (false without neighbours)
        bool neighbourMean(uint32_t l, Vector4& out) const {
            double sum[4] = {0.0, 0.0, 0.0, 0.0};
            const uint32_t count = mesh.neighborStart[l + 1] - mesh.neighborStart[l];
            if (count == 0) return false;
            for (uint32_t k = mesh.neighborStart[l]; k < mesh.neighborStart[l + 1]; k++) {
                const Vector4 value = logicalValue(mesh.neighbors[k]);
                sum[0] += value.x; sum[1] += value.y; sum[2] += value.z; sum[3] += value.w;
            }
            out = Vector4((float)(sum[0] / count), (float)(sum[1] / count), (float)(sum[2] / count), (float)(sum[3] / count));
            return true;
        }

        // ---- geometry in op space (world when options.world)

        Vector3 localPosition(ElementKind kind, uint32_t id) const {
            switch (kind) {
                case ElementKind::Vertex: return mesh.logicalPositions[id];
                case ElementKind::Face: {
                    const Vector3& a = mesh.logicalPositions[mesh.logical[mesh.indices[id * 3]]];
                    const Vector3& b = mesh.logicalPositions[mesh.logical[mesh.indices[id * 3 + 1]]];
                    const Vector3& c = mesh.logicalPositions[mesh.logical[mesh.indices[id * 3 + 2]]];
                    return Vector3((a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f);
                }
                case ElementKind::Corner: return mesh.logicalPositions[mesh.logical[mesh.indices[id]]];
            }
            return Vector3(0.0f, 0.0f, 0.0f);
        }

        Vector3 localNormal(ElementKind kind, uint32_t id) const {
            switch (kind) {
                case ElementKind::Vertex: return mesh.logicalNormals[id];
                case ElementKind::Face: return mesh.faceNormals[id];
                case ElementKind::Corner: return mesh.faceNormals[id / 3];
            }
            return Vector3(0.0f, 1.0f, 0.0f);
        }

        Vector3 opPosition(ElementKind kind, uint32_t id) const { return space.point(localPosition(kind, id)); }
        Vector3 opNormal(ElementKind kind, uint32_t id) const { return space.direction(localNormal(kind, id)); }
        Vector3 opLogicalPosition(uint32_t l) const { return space.point(mesh.logicalPositions[l]); }

        // R25 curvature: mean cosine between the normal and the directions to the neighbours,
        // positive where the surface bends away (convex), in -1..1
        float logicalCurvature(uint32_t l) const {
            const uint32_t count = mesh.neighborStart[l + 1] - mesh.neighborStart[l];
            if (count == 0) return 0.0f;
            const Vector3& p = mesh.logicalPositions[l];
            const Vector3& n = mesh.logicalNormals[l];
            float sum = 0.0f;
            for (uint32_t k = mesh.neighborStart[l]; k < mesh.neighborStart[l + 1]; k++) {
                const Vector3 d = normalize3(p - mesh.logicalPositions[mesh.neighbors[k]]);
                sum += dot3(n, d);
            }
            return sum / (float)count;
        }

        float elementCurvature(ElementKind kind, uint32_t id) const {
            switch (kind) {
                case ElementKind::Vertex: return logicalCurvature(id);
                case ElementKind::Face: {
                    float sum = 0.0f;
                    for (uint32_t k = 0; k < 3; k++) sum += logicalCurvature(mesh.logical[mesh.indices[id * 3 + k]]);
                    return sum / 3.0f;
                }
                case ElementKind::Corner: return logicalCurvature(mesh.logical[mesh.indices[id]]);
            }
            return 0.0f;
        }

        // the logical vertices whose local position may fall in an op-space box: an exact op-space
        // test follows, so the local box only has to contain the answer
        void candidates(const Vector3& opMin, const Vector3& opMax, std::vector<uint32_t>& out) const {
            out.clear();
            const float diagonal = length3(mesh.bounds.getMaximum() - mesh.bounds.getMinimum());
            const float pad = 1e-4f * diagonal + 1e-6f;
            if (space.identity) {
                mesh.queryLogicalBox(AABB(opMin - Vector3(pad, pad, pad), opMax + Vector3(pad, pad, pad)), out);
                return;
            }
            const Matrix4 inverse = options.model.inverse();
            Vector3 lo(0.0f, 0.0f, 0.0f), hi(0.0f, 0.0f, 0.0f);
            bool finite = true;
            for (int i = 0; i < 8; i++) {
                const Vector3 corner((i & 1) ? opMax.x : opMin.x, (i & 2) ? opMax.y : opMin.y, (i & 4) ? opMax.z : opMin.z);
                const Vector3 local = inverse * corner;
                if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z)) finite = false;
                if (i == 0) {
                    lo = hi = local;
                } else {
                    lo.x = std::min(lo.x, local.x); lo.y = std::min(lo.y, local.y); lo.z = std::min(lo.z, local.z);
                    hi.x = std::max(hi.x, local.x); hi.y = std::max(hi.y, local.y); hi.z = std::max(hi.z, local.z);
                }
            }
            if (!finite) {
                for (uint32_t l = 0; l < mesh.logicalCount(); l++) out.push_back(l);
                return;
            }
            const float localPad = pad + 1e-4f * length3(hi - lo);
            mesh.queryLogicalBox(AABB(lo - Vector3(localPad, localPad, localPad), hi + Vector3(localPad, localPad, localPad)), out);
        }

        // triangles touching any of the logical vertices, sorted
        void trianglesOf(const std::vector<uint32_t>& logicals, std::vector<uint32_t>& out) const {
            out.clear();
            for (uint32_t l : logicals) {
                for (uint32_t m = mesh.memberStart[l]; m < mesh.memberStart[l + 1]; m++) {
                    const uint32_t v = mesh.members[m];
                    for (uint32_t k = mesh.vertexCornerStart[v]; k < mesh.vertexCornerStart[v + 1]; k++) out.push_back(mesh.vertexCorners[k] / 3);
                }
            }
            detail::sortedUniqueCount(out);
        }

        void cornersOf(const std::vector<uint32_t>& logicals, std::vector<uint32_t>& out) const {
            out.clear();
            for (uint32_t l : logicals) {
                for (uint32_t m = mesh.memberStart[l]; m < mesh.memberStart[l + 1]; m++) {
                    const uint32_t v = mesh.members[m];
                    for (uint32_t k = mesh.vertexCornerStart[v]; k < mesh.vertexCornerStart[v + 1]; k++) out.push_back(mesh.vertexCorners[k]);
                }
            }
            detail::sortedUniqueCount(out);
        }

        bool poll() {
            if (cancelFlag) return true;
            if ((++pollCount & 4095u) == 0 && options.cancel && options.cancel->load(std::memory_order_relaxed)) cancelFlag = true;
            return cancelFlag;
        }

        // ---- R23, R24: targets

        bool insideShape(const Target& target, const Vector3& p) const {
            switch (target.kind) {
                case TargetKind::Sphere: {
                    const Vector3 d = p - target.center;
                    return dot3(d, d) <= target.radius * target.radius;
                }
                case TargetKind::Box: {
                    const Vector3 d = detail::rotateInverse(target.rotation, p - target.center);
                    return std::fabs(d.x) <= target.halfExtents.x && std::fabs(d.y) <= target.halfExtents.y && std::fabs(d.z) <= target.halfExtents.z;
                }
                case TargetKind::Capsule: {
                    float t = 0.0f;
                    return opsSegmentDistance(p, target.from, target.to, t) <= target.radius;
                }
                default: return true;
            }
        }

        void shapeBounds(const Target& target, Vector3& lo, Vector3& hi) const {
            switch (target.kind) {
                case TargetKind::Sphere: {
                    const Vector3 r(target.radius, target.radius, target.radius);
                    lo = target.center - r;
                    hi = target.center + r;
                    break;
                }
                case TargetKind::Box: {
                    for (int i = 0; i < 8; i++) {
                        const Vector3 local((i & 1) ? target.halfExtents.x : -target.halfExtents.x, (i & 2) ? target.halfExtents.y : -target.halfExtents.y,
                                            (i & 4) ? target.halfExtents.z : -target.halfExtents.z);
                        const Vector3 p = target.center + detail::rotate(target.rotation, local);
                        if (i == 0) {
                            lo = hi = p;
                        } else {
                            lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
                            hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
                        }
                    }
                    break;
                }
                case TargetKind::Capsule: {
                    const Vector3 r(target.radius, target.radius, target.radius);
                    lo = Vector3(std::min(target.from.x, target.to.x), std::min(target.from.y, target.to.y), std::min(target.from.z, target.to.z)) - r;
                    hi = Vector3(std::max(target.from.x, target.to.x), std::max(target.from.y, target.to.y), std::max(target.from.z, target.to.z)) + r;
                    break;
                }
                default: break;
            }
        }

        // ids of the target's kind, sorted and unique (ids out of range are dropped: opsValidateOp
        // reports them before this runs)
        void resolveTarget(const Target& target, ElementKind& kind, std::vector<uint32_t>& ids) const {
            kind = opsElementKind(target);
            ids.clear();
            const uint32_t logicalTotal = mesh.logicalCount();
            const uint32_t triangleTotal = mesh.triangleCount();
            const uint32_t limit = kind == ElementKind::Vertex ? logicalTotal : (kind == ElementKind::Face ? triangleTotal : triangleTotal * 3);
            switch (target.kind) {
                case TargetKind::All:
                    ids.resize(limit);
                    for (uint32_t i = 0; i < limit; i++) ids[i] = i;
                    return;
                case TargetKind::Vertices:
                case TargetKind::Faces:
                case TargetKind::Corners:
                    for (uint32_t id : target.ids) {
                        if (id < limit) ids.push_back(id);
                    }
                    detail::sortedUniqueCount(ids);
                    return;
                case TargetKind::Sphere:
                case TargetKind::Box:
                case TargetKind::Capsule: {
                    Vector3 lo, hi;
                    shapeBounds(target, lo, hi);
                    std::vector<uint32_t> logicals;
                    candidates(lo, hi, logicals);
                    std::vector<uint32_t> inside;
                    for (uint32_t l : logicals) {
                        if (insideShape(target, opLogicalPosition(l))) inside.push_back(l);
                    }
                    if (kind == ElementKind::Vertex) {
                        ids = std::move(inside);
                        return;
                    }
                    std::vector<uint32_t> triangles;
                    trianglesOf(inside, triangles);
                    for (uint32_t t : triangles) {
                        bool all = true;
                        for (uint32_t k = 0; k < 3 && all; k++) {
                            const uint32_t l = mesh.logical[mesh.indices[t * 3 + k]];
                            all = std::binary_search(inside.begin(), inside.end(), l);
                        }
                        if (all) ids.push_back(t);
                    }
                    return;
                }
                case TargetKind::Color: {
                    if (kind == ElementKind::Vertex) {
                        for (uint32_t l = 0; l < logicalTotal; l++) {
                            bool all = true;
                            bool any = false;
                            for (uint32_t m = mesh.memberStart[l]; m < mesh.memberStart[l + 1] && all; m++) {
                                const uint32_t v = mesh.members[m];
                                for (uint32_t k = mesh.vertexCornerStart[v]; k < mesh.vertexCornerStart[v + 1] && all; k++) {
                                    any = true;
                                    all = detail::colorMatches(corner(mesh.vertexCorners[k]), target.color);
                                }
                            }
                            if (!any) {
                                for (uint32_t m = mesh.memberStart[l]; m < mesh.memberStart[l + 1] && all; m++) all = detail::colorMatches(base[mesh.members[m]], target.color);
                            }
                            if (all) ids.push_back(l);
                        }
                    } else {
                        // R23 coverage: the fraction of a face's corners that must match
                        const float coverage = opsClamp01(target.color.coverage);
                        for (uint32_t t = 0; t < triangleTotal; t++) {
                            int matched = 0;
                            for (uint32_t k = 0; k < 3; k++) matched += detail::colorMatches(corner(t * 3 + k), target.color) ? 1 : 0;
                            if (kind == ElementKind::Corner) {
                                for (uint32_t k = 0; k < 3; k++) {
                                    if (detail::colorMatches(corner(t * 3 + k), target.color)) ids.push_back(t * 3 + k);
                                }
                            } else if ((float)matched / 3.0f + 1e-6f >= coverage && matched > 0) {
                                ids.push_back(t);
                            }
                        }
                    }
                    return;
                }
            }
        }

        // ---- R25: filters

        bool passes(const Filter& filter, ElementKind kind, uint32_t id) const {
            bool keep = true;
            switch (filter.kind) {
                case FilterKind::Facing: {
                    const Vector3 n = opNormal(kind, id);
                    const Vector3 d = normalize3(filter.direction);
                    keep = dot3(n, d) >= (float)detail::detCos((double)filter.angle * detail::kPi / 180.0) - 1e-6f;
                    break;
                }
                case FilterKind::Slope: {
                    // angle from up in [min, max]: cos is decreasing on 0..180
                    const float up = opNormal(kind, id).y;
                    const float cosMin = (float)detail::detCos((double)filter.min * detail::kPi / 180.0);
                    const float cosMax = (float)detail::detCos((double)filter.max * detail::kPi / 180.0);
                    keep = up <= cosMin + 1e-6f && up >= cosMax - 1e-6f;
                    break;
                }
                case FilterKind::Height: {
                    const float y = opPosition(kind, id).y;
                    keep = y >= filter.min && y <= filter.max;
                    break;
                }
                case FilterKind::Curvature: {
                    const float c = elementCurvature(kind, id);
                    keep = c >= filter.min && c <= filter.max;
                    break;
                }
                case FilterKind::Channel: {
                    const float v = elementValue(kind, id)[(size_t)std::max(0, std::min(3, filter.channel))];
                    keep = v >= filter.min && v <= filter.max;
                    break;
                }
                case FilterKind::Occluded: {
                    if (options.visible) keep = options.visible(worldSpace.point(localPosition(kind, id)));
                    break;
                }
            }
            return filter.invert ? !keep : keep;
        }

        bool passesAll(std::span<const Filter> filters, ElementKind kind, uint32_t id) const {
            for (const Filter& filter : filters) {
                if (!passes(filter, kind, id)) return false;
            }
            return true;
        }

        // target then filters; result.inTarget / filteredOut filled
        void select(const Op& op, ElementKind& kind, std::vector<uint32_t>& ids, OpResult& result) {
            resolveTarget(op.target, kind, ids);
            result.inTarget = (uint32_t)ids.size();
            if (op.filters.empty()) return;
            size_t kept = 0;
            for (uint32_t id : ids) {
                if (passesAll(op.filters, kind, id)) ids[kept++] = id;
            }
            ids.resize(kept);
            result.filteredOut = result.inTarget - (uint32_t)kept;
        }

        // ---- per-op bookkeeping (R28)

        void finishOp(OpResult& result) {
            const Format format = options.format;
            std::vector<uint32_t> changed;
            for (size_t i = 0; i < opVerts.size(); i++) {
                const uint32_t v = opVerts[i];
                if (quantize(opVertOld[i], format) != quantize(base[v], format)) changed.push_back(mesh.logical[v]);
            }
            for (size_t i = 0; i < opCorners.size(); i++) {
                const uint32_t c = opCorners[i];
                const uint32_t v = mesh.indices[c];
                const Vector4 oldBase = quantize(opVertSlot[v] >= 0 ? opVertOld[(size_t)opVertSlot[v]] : base[v], format);
                const Vector4 newBase = quantize(base[v], format);
                const Vector4 oldValue = quantize(opCornerOld[i], format);
                const Vector4 newValue = quantize(corner(c), format);
                const bool oldOverride = oldValue != oldBase;
                const bool newOverride = newValue != newBase;
                if (oldOverride != newOverride || (oldOverride && oldValue != newValue)) result.changedCorners++;
                if (!oldOverride && newOverride) result.splitAdded++;
                if (oldValue != newValue) changed.push_back(mesh.logical[v]);
            }
            result.changedVertices = detail::sortedUniqueCount(changed);
            for (uint32_t v : opVerts) opVertSlot[v] = -1;
            for (uint32_t c : opCorners) opCornerSlot[c] = -1;
            opVerts.clear();
            opVertOld.clear();
            opCorners.clear();
            opCornerOld.clear();
        }

        // the batch's values as the format stores them (R26: only now do they reach a TargetData)
        void finish(TargetData& out) const {
            const Format format = options.format;
            out.key = source.key;
            out.fingerprint = source.fingerprint;
            out.snapshotPositions = source.snapshotPositions;
            out.snapshotNormals = source.snapshotNormals;
            out.snapshotIndices = source.snapshotIndices;
            out.base.resize(base.size());
            for (size_t v = 0; v < base.size(); v++) out.base[v] = quantize(base[v], format);
            std::vector<uint32_t> overlay(cornerIds);
            std::sort(overlay.begin(), overlay.end());
            out.corners.clear();
            auto emit = [&](uint32_t c, const Vector4& value) {
                const Vector4 q = quantize(value, format);
                if (q != out.base[mesh.indices[c]]) out.corners.push_back(CornerValue{c, q});
            };
            size_t i = 0;
            const std::vector<CornerValue> none;
            const std::vector<CornerValue>& stored = fresh ? none : source.corners;
            for (const CornerValue& entry : stored) {
                while (i < overlay.size() && overlay[i] < entry.corner) {
                    emit(overlay[i], cornerValues[(size_t)cornerSlot[overlay[i]]]);
                    i++;
                }
                if (cornerSlot[entry.corner] >= 0) continue;
                emit(entry.corner, entry.value);
            }
            for (; i < overlay.size(); i++) emit(overlay[i], cornerValues[(size_t)cornerSlot[overlay[i]]]);
        }

        // ---- the ops

        template <class F> bool forElements(const Op& op, OpResult& result, F&& fn) {
            ElementKind kind;
            std::vector<uint32_t> ids;
            select(op, kind, ids, result);
            for (uint32_t id : ids) {
                if (poll()) return false;
                fn(kind, id);
            }
            return true;
        }

        bool runSimple(const Op& op, OpResult& result) {
            switch (op.type) {
                case OpType::Set:
                    return forElements(op, result, [&](ElementKind kind, uint32_t id) {
                        applyElement(kind, id, [&](const Vector4& old, uint32_t) { return blendValue(old, op.value, Blend::Replace, 1.0f, op.channels); });
                    });
                case OpType::Fill:
                    return forElements(op, result, [&](ElementKind kind, uint32_t id) {
                        applyElement(kind, id, [&](const Vector4& old, uint32_t) { return blendValue(old, op.value, op.blend, op.strength, op.channels); });
                    });
                case OpType::Clear:
                    return forElements(op, result, [&](ElementKind kind, uint32_t id) {
                        applyElement(kind, id, [&](const Vector4& old, uint32_t vertex) {
                            return blendValue(old, clearValue(op.clearTo, vertex), Blend::Replace, op.strength, op.channels);
                        });
                    });
                case OpType::Adjust:
                    return forElements(op, result, [&](ElementKind kind, uint32_t id) {
                        applyElement(kind, id, [&](const Vector4& old, uint32_t) { return blendValue(old, adjust(op, old), op.blend, op.strength, op.channels); });
                    });
                case OpType::Channel: {
                    const ChannelMask mask = (ChannelMask)(op.channels & channelProduces(op));
                    return forElements(op, result, [&](ElementKind kind, uint32_t id) {
                        applyElement(kind, id, [&](const Vector4& old, uint32_t) { return blendValue(old, channelOp(op, old), Blend::Replace, op.strength, mask); });
                    });
                }
                case OpType::ReplaceColor:
                    return forElements(op, result, [&](ElementKind kind, uint32_t id) {
                        applyElement(kind, id, [&](const Vector4& old, uint32_t) {
                            return detail::colorMatches(old, op.target.color) ? blendValue(old, op.value, op.blend, op.strength, op.channels) : old;
                        });
                    });
                case OpType::ByDirection: {
                    const Vector3 direction = normalize3(op.direction);
                    return forElements(op, result, [&](ElementKind kind, uint32_t id) {
                        const float t = opsSmoothstep(op.rangeMin, op.rangeMax, dot3(opNormal(kind, id), direction));
                        if (t <= 0.0f) return;
                        applyElement(kind, id, [&](const Vector4& old, uint32_t) { return blendValue(old, op.value, op.blend, op.strength * t, op.channels); });
                    });
                }
                default: return false;
            }
        }

        Vector4 clearValue(ClearTo to, uint32_t vertex) const {
            switch (to) {
                case ClearTo::Initial: return initialOf(vertex);
                case ClearTo::Asset: return vertex < mesh.assetColors.size() ? mesh.assetColors[vertex] : Vector4(1.0f, 1.0f, 1.0f, 1.0f);
                case ClearTo::White: return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
                case ClearTo::Black: return Vector4(0.0f, 0.0f, 0.0f, 1.0f);
            }
            return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        }

        static Vector4 adjust(const Op& op, const Vector4& v) {
            Vector4 out = v;
            switch (op.adjustKind) {
                case AdjustKind::Levels: {
                    const float span = op.inWhite - op.inBlack;
                    const double exponent = 1.0 / (double)op.gamma;
                    for (size_t c = 0; c < 4; c++) {
                        float x = span != 0.0f ? opsClamp01((v[c] - op.inBlack) / span) : (v[c] >= op.inWhite ? 1.0f : 0.0f);
                        if (op.gamma != 1.0f) x = (float)detail::detPow(x, exponent);
                        out[c] = op.outBlack + x * (op.outWhite - op.outBlack);
                    }
                    break;
                }
                case AdjustKind::BrightnessContrast:
                    for (size_t c = 0; c < 4; c++) out[c] = (v[c] - 0.5f) * (1.0f + op.contrast) + 0.5f + op.brightness;
                    break;
                case AdjustKind::Hsv: {
                    detail::Hsv hsv = detail::toHsv(v);
                    hsv.h += op.hueShift / 360.0f;
                    hsv.h -= std::floor(hsv.h);
                    hsv.s = opsClamp01(hsv.s + op.saturationShift);
                    hsv.v = std::max(0.0f, hsv.v + op.valueShift);
                    out = detail::fromHsv(hsv, v.w);
                    break;
                }
                case AdjustKind::Invert:
                    for (size_t c = 0; c < 4; c++) out[c] = 1.0f - v[c];
                    break;
                case AdjustKind::Clamp:
                    for (size_t c = 0; c < 4; c++) out[c] = std::max(op.outBlack, std::min(op.outWhite, v[c]));
                    break;
            }
            return out;
        }

        static ChannelMask channelProduces(const Op& op) {
            switch (op.channelKind) {
                case ChannelOpKind::Copy: return (ChannelMask)(1u << op.destChannel);
                case ChannelOpKind::Swap: return (ChannelMask)((1u << op.sourceChannel) | (1u << op.destChannel));
                case ChannelOpKind::Invert: return op.channels;
                case ChannelOpKind::Pack: return CHANNELS_RGBA;
            }
            return 0;
        }

        static Vector4 channelOp(const Op& op, const Vector4& v) {
            Vector4 out = v;
            switch (op.channelKind) {
                case ChannelOpKind::Copy: out[(size_t)op.destChannel] = v[(size_t)op.sourceChannel]; break;
                case ChannelOpKind::Swap:
                    out[(size_t)op.destChannel] = v[(size_t)op.sourceChannel];
                    out[(size_t)op.sourceChannel] = v[(size_t)op.destChannel];
                    break;
                case ChannelOpKind::Invert:
                    for (size_t c = 0; c < 4; c++) out[c] = 1.0f - v[c];
                    break;
                case ChannelOpKind::Pack:
                    for (size_t c = 0; c < 4; c++) out[c] = op.packSource[c] >= 0 ? v[(size_t)op.packSource[c]] : op.packConstant[c];
                    break;
            }
            return out;
        }

        bool runNoise(const Op& op, OpResult& result, std::string& error) {
#if JASPERENGINE_NOISE_ENABLED
            (void)error;
            noise::FractalParams params;
            params.basis.basis = op.noiseKind == NoiseKind::Value ? noise::Basis::Value
                                 : (op.noiseKind == NoiseKind::Cellular ? noise::Basis::Cellular : noise::Basis::Simplex);
            params.basis.dimensions = 3;
            params.basis.frequency = 1.0f / op.noiseScale;
            params.basis.seed = op.seed;
            params.octaves = std::max(1, std::min(12, op.octaves));
            const noise::Interval range = noise::basisRange(params.basis);
            const float span = range.max - range.min;
            return forElements(op, result, [&](ElementKind kind, uint32_t id) {
                const Vector3 local = localPosition(kind, id);
                const Vector3 p = op.noiseWorld ? worldSpace.point(local) : local;
                const float n = noise::fbmPoint(params, p.x, p.y, p.z);
                const float t = span > 0.0f ? opsClamp01((n - range.min) / span) : 0.5f;
                const Vector4 src = opsLerp4(op.value, op.value2, t);
                applyElement(kind, id, [&](const Vector4& old, uint32_t) { return blendValue(old, src, op.blend, op.strength, op.channels); });
            });
#else
            // a game exported without the Noise module (T2 R42): the kernels are not linked, so the op
            // says so instead of the player failing to link
            (void)op;
            (void)result;
            error = "noise: this build leaves out the Noise module (JASPERENGINE_NOISE); turn it on in the player's modules, or run the noise op in the editor so the values ship painted";
            return false;
#endif
        }

        bool runGradient(const Op& op, OpResult& result) {
            std::vector<GradientStop> stops(op.stops);
            std::stable_sort(stops.begin(), stops.end(), [](const GradientStop& a, const GradientStop& b) { return a.t < b.t; });
            float lo = 0.0f, hi = 1.0f;
            Vector3 axis = normalize3(op.gradientType == GradientType::Height ? Vector3(0.0f, 1.0f, 0.0f) : op.axis);
            if (op.gradientType == GradientType::Height || op.gradientType == GradientType::Axis) {
                // R45: over the target's bounds, in op space
                for (uint32_t l = 0; l < mesh.logicalCount(); l++) {
                    const float d = dot3(opLogicalPosition(l), axis);
                    if (l == 0) lo = hi = d;
                    lo = std::min(lo, d);
                    hi = std::max(hi, d);
                }
            }
            const Vector3 span = op.gradientTo - op.gradientFrom;
            const float span2 = dot3(span, span);
            const float spanLength = length3(span);
            return forElements(op, result, [&](ElementKind kind, uint32_t id) {
                const Vector3 p = opPosition(kind, id);
                float t = 0.0f;
                switch (op.gradientType) {
                    case GradientType::Linear: t = span2 > 0.0f ? dot3(p - op.gradientFrom, span) / span2 : 0.0f; break;
                    case GradientType::Radial: t = spanLength > 0.0f ? length3(p - op.gradientFrom) / spanLength : 0.0f; break;
                    case GradientType::Height:
                    case GradientType::Axis: t = hi > lo ? (dot3(p, axis) - lo) / (hi - lo) : 0.0f; break;
                }
                if (t < 0.0f) {
                    if (op.extend == Extend::Off || op.extend == Extend::Forward) return;
                    t = 0.0f;
                } else if (t > 1.0f) {
                    if (op.extend == Extend::Off || op.extend == Extend::Backward) return;
                    t = 1.0f;
                }
                const OpsGradientSample sample = opsGradientAt(stops, t, op.interpolation, op.hueDirection);
                const float weight = op.strength * opsClamp01(sample.strength);
                applyElement(kind, id, [&](const Vector4& old, uint32_t) { return blendValue(old, sample.value, op.blend, weight, op.channels); });
            });
        }

        // R34 / R22 smooth and sharpen: every source of an iteration is read before any write, so
        // the result does not depend on the element order
        bool runSmooth(const Op& op, OpResult& result) {
            ElementKind kind;
            std::vector<uint32_t> ids;
            select(op, kind, ids, result);
            const bool sharpen = op.type == OpType::Sharpen;
            std::vector<uint32_t> items;     // logical (Vertex) or corner ids
            if (kind == ElementKind::Vertex) {
                items = ids;
            } else if (kind == ElementKind::Face) {
                for (uint32_t t : ids) {
                    for (uint32_t k = 0; k < 3; k++) items.push_back(t * 3 + k);
                }
            } else {
                items = ids;
            }
            std::vector<Vector4> sources(items.size());
            std::vector<uint8_t> has(items.size());
            for (int iteration = 0; iteration < op.iterations; iteration++) {
                for (size_t i = 0; i < items.size(); i++) {
                    if (poll()) return false;
                    const uint32_t l = kind == ElementKind::Vertex ? items[i] : mesh.logical[mesh.indices[items[i]]];
                    Vector4 mean;
                    has[i] = neighbourMean(l, mean) ? 1 : 0;
                    if (!has[i]) continue;
                    if (sharpen) {
                        const Vector4 self = logicalValue(l);
                        sources[i] = Vector4(2.0f * self.x - mean.x, 2.0f * self.y - mean.y, 2.0f * self.z - mean.z, 2.0f * self.w - mean.w);
                    } else {
                        sources[i] = mean;
                    }
                }
                for (size_t i = 0; i < items.size(); i++) {
                    if (!has[i]) continue;
                    const Vector4& src = sources[i];
                    auto fn = [&](const Vector4& old, uint32_t) { return blendValue(old, src, op.blend, op.strength, op.channels); };
                    if (kind == ElementKind::Vertex) applyLogical(items[i], fn);
                    else applyCorner(items[i], fn);
                }
            }
            return true;
        }

        // ---- R30-R35: strokes

        struct Stamp {
            Vector3 position;
            Vector3 from;
            float radius = 0.0f;
            float fromRadius = 0.0f;
            float pressure = 1.0f;
            float fromPressure = 1.0f;
        };

        bool makeStamps(const Op& op, StrokeState& state, std::vector<Stamp>& stamps, std::string& error) const {
            const float step = op.spacing * op.radius;
            auto radiusOf = [&](float pressure) { return op.pressureSize ? op.radius * opsClamp01(pressure) : op.radius; };
            auto emit = [&](const Vector3& position, float pressure) {
                Stamp stamp;
                stamp.position = position;
                stamp.pressure = opsClamp01(pressure);
                stamp.radius = radiusOf(pressure);
                if (state.hasLastStamp) {
                    stamp.from = state.lastStamp;
                    stamp.fromRadius = state.lastStampRadius;
                    stamp.fromPressure = state.lastStampPressure;
                } else {
                    stamp.from = position;
                    stamp.fromRadius = stamp.radius;
                    stamp.fromPressure = stamp.pressure;
                }
                state.hasLastStamp = true;
                state.lastStamp = position;
                state.lastStampRadius = stamp.radius;
                state.lastStampPressure = stamp.pressure;
                stamps.push_back(stamp);
            };
            Vector3 a;
            float pressureA = 1.0f;
            size_t first = 0;
            if (!state.hasLastPoint) {
                emit(op.points[0].position, op.points[0].pressure);
                a = op.points[0].position;
                pressureA = op.points[0].pressure;
                state.carry = 0.0f;
                first = 1;
            } else {
                a = state.lastPoint;
                pressureA = state.lastPressure;
            }
            for (size_t i = first; i < op.points.size(); i++) {
                const Vector3& b = op.points[i].position;
                const float pressureB = op.points[i].pressure;
                const float length = length3(b - a);
                if (length > 0.0f) {
                    float d = step - state.carry;
                    while (d <= length) {
                        const float t = d / length;
                        emit(opsLerp3(a, b, t), pressureA + (pressureB - pressureA) * t);
                        if (stamps.size() > kOpsMaxStamps) {
                            error = "points: the stroke needs more than " + std::to_string(kOpsMaxStamps) + " stamps; raise spacing or radius, or split the stroke";
                            return false;
                        }
                        d += step;
                    }
                    state.carry = length - (d - step);
                }
                a = b;
                pressureA = pressureB;
            }
            state.hasLastPoint = true;
            state.lastPoint = a;
            state.lastPressure = pressureA;
            return true;
        }

        // R33: a grayscale stencil, triplanar in world space
        float stencilAt(const Op& op, ElementKind kind, uint32_t id) const {
            if (!op.stencil || op.stencil->width <= 0 || op.stencil->height <= 0) return 1.0f;
            const Vector3 p = worldSpace.point(localPosition(kind, id));
            const Vector3 n = worldSpace.direction(localNormal(kind, id));
            float wx = std::fabs(n.x), wy = std::fabs(n.y), wz = std::fabs(n.z);
            float sum = wx + wy + wz;
            if (!(sum > 0.0f)) {
                wy = 1.0f;
                sum = 1.0f;
            }
            const double angle = (double)op.stencilRotation * detail::kPi / 180.0;
            const float c = (float)detail::detCos(angle), s = (float)detail::detSin(angle);
            const float scale = op.stencilScale;
            auto sampleAt = [&](float u, float v) {
                const float ru = (c * u - s * v) / scale;
                const float rv = (s * u + c * v) / scale;
                return op.stencil->sample(ru, rv).x;
            };
            float value = 0.0f;
            if (wx > 0.0f) value += wx * sampleAt(p.z, p.y);
            if (wy > 0.0f) value += wy * sampleAt(p.x, p.z);
            if (wz > 0.0f) value += wz * sampleAt(p.x, p.y);
            return opsClamp01(value / sum);
        }

        bool runStroke(const Op& op, OpResult& result, StrokeState& state, std::string& error) {
            const ElementKind kind = opsElementKind(op.target);
            const bool restricted = op.target.kind != TargetKind::All;
            std::vector<uint32_t> allowed;
            if (restricted) {
                ElementKind targetKind;
                resolveTarget(op.target, targetKind, allowed);
            }
            if (state.slot.size() != mesh.vertexCount() || state.cornerSlot.size() != mesh.indices.size()) {
                // a fresh state, or one from another mesh, whose per-vertex memory means nothing here
                state.touched.clear();
                state.start.clear();
                state.weight.clear();
                state.slot.assign(mesh.vertexCount(), -1);
                state.cornerSlot.assign(mesh.indices.size(), -1);
            }
            std::vector<Stamp> stamps;
            if (!makeStamps(op, state, stamps, error)) return false;

            const Blend blend = op.mode == StrokeMode::Paint ? op.blend : Blend::Replace;
            std::vector<uint32_t> reached;
            std::vector<uint32_t> filtered;
            std::vector<uint32_t> logicals;
            std::vector<uint32_t> elements;
            struct Hit {
                uint32_t id;
                float weight;
                Vector4 source;
            };
            std::vector<Hit> hits;

            auto applyItem = [&](bool isCorner, uint32_t item, const Vector4& src, float weight) {
                const Vector4 current = isCorner ? corner(item) : base[item];
                Vector4 next;
                if (op.buildup) {
                    next = blendValue(current, src, blend, weight * op.strength, op.channels);
                } else {
                    // R31: the strongest weight so far from the value the stroke found
                    int32_t& slot = isCorner ? state.cornerSlot[item] : state.slot[item];
                    if (slot < 0) {
                        slot = (int32_t)state.touched.size();
                        state.touched.push_back(isCorner ? (item | kOpsCornerBit) : item);
                        state.start.push_back(current);
                        state.weight.push_back(0.0f);
                    }
                    float& strongest = state.weight[(size_t)slot];
                    strongest = std::max(strongest, weight);
                    next = blendValue(state.start[(size_t)slot], src, blend, strongest * op.strength, op.channels);
                }
                if (isCorner) setCorner(item, next);
                else setBase(item, next);
            };
            auto sourceFor = [&](const Hit& hit, uint32_t vertex) { return op.mode == StrokeMode::Erase ? initialOf(vertex) : hit.source; };

            for (const Stamp& stamp : stamps) {
                if (poll()) return false;
                const float reach = std::max(stamp.radius, stamp.fromRadius);
                if (!(reach > 0.0f)) continue;
                const Vector3 lo(std::min(stamp.from.x, stamp.position.x) - reach, std::min(stamp.from.y, stamp.position.y) - reach,
                                 std::min(stamp.from.z, stamp.position.z) - reach);
                const Vector3 hi(std::max(stamp.from.x, stamp.position.x) + reach, std::max(stamp.from.y, stamp.position.y) + reach,
                                 std::max(stamp.from.z, stamp.position.z) + reach);
                candidates(lo, hi, logicals);
                // distance to the swept capsule as a fraction of its radius there
                auto reachOf = [&](const Vector3& p, float& x, float& pressure) {
                    float t = 0.0f;
                    const float d = opsSegmentDistance(p, stamp.from, stamp.position, t);
                    const float r = stamp.fromRadius + (stamp.radius - stamp.fromRadius) * t;
                    pressure = stamp.fromPressure + (stamp.pressure - stamp.fromPressure) * t;
                    x = r > 0.0f ? d / r : 2.0f;
                    return x <= 1.0f;
                };
                std::vector<uint32_t> inside;
                for (uint32_t l : logicals) {
                    float x, pressure;
                    if (reachOf(opLogicalPosition(l), x, pressure)) inside.push_back(l);
                }
                elements.clear();
                if (kind == ElementKind::Vertex) {
                    elements = inside;
                } else if (kind == ElementKind::Face) {
                    std::vector<uint32_t> triangles;
                    trianglesOf(inside, triangles);
                    for (uint32_t t : triangles) {
                        bool all = true;
                        for (uint32_t k = 0; k < 3 && all; k++) all = std::binary_search(inside.begin(), inside.end(), mesh.logical[mesh.indices[t * 3 + k]]);
                        if (all) elements.push_back(t);
                    }
                } else {
                    cornersOf(inside, elements);
                }
                hits.clear();
                for (uint32_t id : elements) {
                    if (restricted && !std::binary_search(allowed.begin(), allowed.end(), id)) continue;
                    reached.push_back(id);
                    if (!passesAll(op.filters, kind, id)) {
                        filtered.push_back(id);
                        continue;
                    }
                    float x, pressure;
                    reachOf(opPosition(kind, id), x, pressure);
                    float weight = opsFalloff(op, std::min(x, 1.0f));
                    if (op.pressureStrength) weight *= opsClamp01(pressure);
                    if (op.stencil) weight *= stencilAt(op, kind, id);
                    if (!(weight > 0.0f)) continue;
                    Hit hit{id, weight, op.value};
                    if (op.mode == StrokeMode::Smooth || op.mode == StrokeMode::Sharpen) {
                        const uint32_t l = kind == ElementKind::Vertex ? id : mesh.logical[mesh.indices[kind == ElementKind::Face ? id * 3 : id]];
                        Vector4 mean;
                        if (!neighbourMean(l, mean)) continue;
                        if (op.mode == StrokeMode::Sharpen) {
                            const Vector4 self = logicalValue(l);
                            mean = Vector4(2.0f * self.x - mean.x, 2.0f * self.y - mean.y, 2.0f * self.z - mean.z, 2.0f * self.w - mean.w);
                        }
                        hit.source = mean;
                    }
                    hits.push_back(hit);
                }
                // sources are read for the whole stamp before it writes (smooth stays order independent)
                for (const Hit& hit : hits) {
                    if (kind == ElementKind::Vertex) {
                        for (uint32_t m = mesh.memberStart[hit.id]; m < mesh.memberStart[hit.id + 1]; m++) {
                            const uint32_t v = mesh.members[m];
                            // corners before their base, as applyVertex
                            for (uint32_t k = mesh.vertexCornerStart[v]; k < mesh.vertexCornerStart[v + 1]; k++) applyItem(true, mesh.vertexCorners[k], sourceFor(hit, v), hit.weight);
                            applyItem(false, v, sourceFor(hit, v), hit.weight);
                        }
                    } else if (kind == ElementKind::Face) {
                        for (uint32_t k = 0; k < 3; k++) {
                            const uint32_t c = hit.id * 3 + k;
                            applyItem(true, c, sourceFor(hit, mesh.indices[c]), hit.weight);
                        }
                    } else {
                        applyItem(true, hit.id, sourceFor(hit, mesh.indices[hit.id]), hit.weight);
                    }
                }
            }
            result.inTarget = detail::sortedUniqueCount(reached);
            result.filteredOut = detail::sortedUniqueCount(filtered);
            return true;
        }

        // ---- R41-R43, R60: a generated field written like any op (channels, blend, strength)

        bool writeField(const Op& op, const bake::Field& field, OpResult& result, std::string& error) {
            if (field.vertexValues.size() != mesh.vertexCount() || (field.perCorner && field.cornerValues.size() != mesh.indices.size()) ||
                (!field.valid.empty() && field.valid.size() != mesh.vertexCount())) {
                error = "the generator returned " + std::to_string(field.vertexValues.size()) + " values for " + std::to_string(mesh.vertexCount()) +
                        " vertices; rebuild the mesh view and retry";
                return false;
            }
            auto valid = [&](uint32_t v) { return field.valid.empty() || field.valid[v] != 0; };
            auto blendTo = [&](const Vector4& src) {
                return [&op, src](const Vector4& old, uint32_t) { return blendValue(old, src, op.blend, op.strength, op.channels); };
            };
            ElementKind kind;
            std::vector<uint32_t> ids;
            select(op, kind, ids, result);
            for (uint32_t id : ids) {
                if (poll()) return false;
                if (kind != ElementKind::Vertex) {
                    const uint32_t firstCorner = kind == ElementKind::Face ? id * 3 : id;
                    const uint32_t count = kind == ElementKind::Face ? 3u : 1u;
                    for (uint32_t c = firstCorner; c < firstCorner + count; c++) {
                        const uint32_t v = mesh.indices[c];
                        if (!valid(v)) continue;
                        applyCorner(c, blendTo(field.perCorner ? field.cornerValues[c] : field.vertexValues[v]));
                    }
                    continue;
                }
                const uint32_t l = id;
                if (field.exactCorners) {
                    // R60 with corners: every corner takes its own value (overrides where it differs
                    // from its vertex), the base its vertex's most common corner value (fewest
                    // overrides; ties: the first). Corners first: an untouched one reads the old base
                    for (uint32_t m = mesh.memberStart[l]; m < mesh.memberStart[l + 1]; m++) {
                        const uint32_t v = mesh.members[m];
                        if (!valid(v)) continue;
                        const uint32_t first = mesh.vertexCornerStart[v], last = mesh.vertexCornerStart[v + 1];
                        Vector4 baseSource = field.vertexValues[v];
                        uint32_t most = 0;
                        for (uint32_t k = first; k < last; k++) {
                            const Vector4& value = field.cornerValues[mesh.vertexCorners[k]];
                            uint32_t count = 0;
                            for (uint32_t j = first; j < last; j++) count += field.cornerValues[mesh.vertexCorners[j]] == value ? 1u : 0u;
                            if (count > most) {
                                most = count;
                                baseSource = value;
                            }
                        }
                        const Vector4 oldBase = base[v];
                        for (uint32_t k = first; k < last; k++) {
                            const uint32_t c = mesh.vertexCorners[k];
                            setCorner(c, blendValue(corner(c), field.cornerValues[c], op.blend, op.strength, op.channels));
                        }
                        setBase(v, blendValue(oldBase, baseSource, op.blend, op.strength, op.channels));
                    }
                    continue;
                }
                // the corners of the logical vertex in member, then corner order
                struct Sample {
                    uint32_t corner;
                    Vector4 value;
                };
                std::vector<Sample> samples;
                double sum[4] = {0.0, 0.0, 0.0, 0.0};
                uint32_t count = 0;
                auto add = [&](const Vector4& value) {
                    sum[0] += value.x; sum[1] += value.y; sum[2] += value.z; sum[3] += value.w;
                    count++;
                };
                for (uint32_t m = mesh.memberStart[l]; m < mesh.memberStart[l + 1]; m++) {
                    const uint32_t v = mesh.members[m];
                    if (!valid(v)) continue;
                    if (field.perCorner && mesh.vertexCornerStart[v + 1] > mesh.vertexCornerStart[v]) {
                        for (uint32_t k = mesh.vertexCornerStart[v]; k < mesh.vertexCornerStart[v + 1]; k++) {
                            const uint32_t c = mesh.vertexCorners[k];
                            samples.push_back(Sample{c, field.cornerValues[c]});
                            add(field.cornerValues[c]);
                        }
                    } else {
                        add(field.vertexValues[v]);
                    }
                }
                if (count == 0) continue;
                const Vector4 mean((float)(sum[0] / count), (float)(sum[1] / count), (float)(sum[2] / count), (float)(sum[3] / count));
                if (!field.perCorner || !op.keepSeams || samples.size() < 2) {
                    applyLogical(l, blendTo(mean));
                    continue;
                }
                // R41 Keep texture seams: corners closer than seamDeltaE to a cluster's first value
                // share its mean; more than one cluster keeps the hard edge as corner overrides
                std::vector<Vector4> repr;
                std::vector<std::array<double, 5>> clusterSum;
                std::vector<uint32_t> clusterOf(samples.size());
                for (size_t i = 0; i < samples.size(); i++) {
                    size_t found = repr.size();
                    for (size_t k = 0; k < repr.size(); k++) {
                        if (deltaE(repr[k], samples[i].value) <= op.seamDeltaE) {
                            found = k;
                            break;
                        }
                    }
                    if (found == repr.size()) {
                        repr.push_back(samples[i].value);
                        clusterSum.push_back({0.0, 0.0, 0.0, 0.0, 0.0});
                    }
                    clusterOf[i] = (uint32_t)found;
                    std::array<double, 5>& s = clusterSum[found];
                    s[0] += samples[i].value.x; s[1] += samples[i].value.y; s[2] += samples[i].value.z; s[3] += samples[i].value.w; s[4] += 1.0;
                }
                if (repr.size() == 1) {
                    applyLogical(l, blendTo(mean));
                    continue;
                }
                std::vector<Vector4> clusterMean(repr.size());
                for (size_t k = 0; k < repr.size(); k++) {
                    const std::array<double, 5>& s = clusterSum[k];
                    clusterMean[k] = Vector4((float)(s[0] / s[4]), (float)(s[1] / s[4]), (float)(s[2] / s[4]), (float)(s[3] / s[4]));
                }
                size_t i = 0;
                for (uint32_t m = mesh.memberStart[l]; m < mesh.memberStart[l + 1]; m++) {
                    const uint32_t v = mesh.members[m];
                    if (!valid(v)) continue;
                    const bool hasCorners = mesh.vertexCornerStart[v + 1] > mesh.vertexCornerStart[v];
                    // the base takes the vertex's first corner's cluster: fewest overrides
                    const Vector4 baseSource = hasCorners ? clusterMean[clusterOf[i]] : mean;
                    const Vector4 oldBase = base[v];
                    for (uint32_t k = mesh.vertexCornerStart[v]; k < mesh.vertexCornerStart[v + 1]; k++, i++) {
                        const uint32_t c = mesh.vertexCorners[k];
                        setCorner(c, blendValue(corner(c), clusterMean[clusterOf[i]], op.blend, op.strength, op.channels));
                    }
                    setBase(v, blendValue(oldBase, baseSource, op.blend, op.strength, op.channels));
                }
            }
            return true;
        }

        bool runBake(const Op& op, OpResult& result, std::string& error) {
            if (injectedField) return writeField(op, *injectedField, result, error);
            bake::Field field;
            OpResult generated;
            bool ok = false;
            switch (op.type) {
                case OpType::FromTexture: ok = bake::fromTexture(mesh, op, options, field, generated, error); break;
                case OpType::BakeAO: ok = bake::ambientOcclusion(mesh, op, options, false, field, error); break;
                case OpType::BakeThickness: ok = bake::ambientOcclusion(mesh, op, options, true, field, error); break;
                case OpType::BakeCurvature: ok = bake::curvature(mesh, op, field, error); break;
                case OpType::Transfer: ok = bake::transfer(mesh, op, options, field, generated, error); break;
                default: break;
            }
            if (!ok) {
                if (options.cancel && options.cancel->load(std::memory_order_relaxed)) cancelFlag = true;
                return false;
            }
            result.meanDeltaE = generated.meanDeltaE;
            result.p95DeltaE = generated.p95DeltaE;
            result.underSampledFaces = generated.underSampledFaces;
            result.unmatched = generated.unmatched;
            result.meanDistance = generated.meanDistance;
            return writeField(op, field, result, error);
        }
    };

    // ---- validation: every op before any runs, so a bad op late in a batch costs nothing

    std::string opsValidateTarget(const Target& target, const MeshView& mesh) {
        switch (target.kind) {
            case TargetKind::Vertices:
            case TargetKind::Faces:
            case TargetKind::Corners: {
                const ElementKind kind = opsElementKind(target);
                const uint32_t limit = kind == ElementKind::Vertex ? mesh.logicalCount() : (kind == ElementKind::Face ? mesh.triangleCount() : mesh.triangleCount() * 3);
                for (uint32_t id : target.ids) {
                    if (id >= limit) {
                        return "target.ids: " + std::to_string(id) + " is out of range; the target has " + std::to_string(limit) + " " + opsKindNoun(kind) +
                               (limit > 0 ? " (0.." + std::to_string(limit - 1) + ")" : "") + "; read valid ids with sample_mesh_colors elements";
                    }
                }
                return "";
            }
            case TargetKind::Sphere:
                if (!(target.radius > 0.0f)) return opsNumber("target.radius: must be > 0, got ", target.radius) + "; give the sphere's radius";
                return "";
            case TargetKind::Capsule:
                if (!(target.radius > 0.0f)) return opsNumber("target.radius: must be > 0, got ", target.radius) + "; give the capsule's radius";
                return "";
            case TargetKind::Box:
                if (!(target.halfExtents.x >= 0.0f && target.halfExtents.y >= 0.0f && target.halfExtents.z >= 0.0f))
                    return "target.half_extents: must be >= 0 on every axis; give half the box's size";
                return "";
            case TargetKind::Color:
                if (!(target.color.tolerance >= 0.0f)) return "target.tolerance: must be >= 0; 0.02 matches about 5 steps of 255";
                if (!(target.color.coverage >= 0.0f && target.color.coverage <= 1.0f)) return "target.coverage: must be from 0 to 1; 1 needs every corner of a face to match";
                return "";
            default: return "";
        }
    }

    std::string opsValidateOp(const Op& op, const MeshView& mesh) {
        if (!(op.strength >= 0.0f && op.strength <= 1.0f)) return opsNumber("strength: must be from 0 to 1, got ", op.strength) + "; 1 is the full change";
        if ((op.channels & CHANNELS_RGBA) == 0) return "channels: names no channel; give a subset of \"rgba\"";
        if (std::string message = opsValidateTarget(op.target, mesh); !message.empty()) return message;
        for (size_t i = 0; i < op.filters.size(); i++) {
            const Filter& filter = op.filters[i];
            const std::string where = "filter[" + std::to_string(i) + "].";
            if (filter.kind == FilterKind::Channel && (filter.channel < 0 || filter.channel > 3)) return where + "channel: must be one of \"r\", \"g\", \"b\", \"a\"";
            if (filter.kind == FilterKind::Facing && !(filter.angle >= 0.0f && filter.angle <= 180.0f)) return where + "angle: must be from 0 to 180 degrees";
            if (filter.kind == FilterKind::Facing && length3(filter.direction) <= 0.0f) return where + "direction: must not be zero; [0, 1, 0] faces up";
            if (filter.kind != FilterKind::Facing && filter.kind != FilterKind::Occluded && !(filter.min <= filter.max)) return where + "max: must be >= min";
        }
        switch (op.type) {
            case OpType::Stroke:
                if (op.points.empty()) return "points: needs at least one point; give [{p = [x, y, z]}, ...]";
                if (!(op.radius > 0.0f)) return opsNumber("radius: must be > 0, got ", op.radius) + "; give the brush radius in world units";
                if (!(op.hardness >= 0.0f && op.hardness <= 1.0f)) return "hardness: must be from 0 to 1";
                if (!(op.spacing >= 0.01f && op.spacing <= 10.0f)) return opsNumber("spacing: must be from 0.01 to 10, got ", op.spacing) + "; 0.25 stamps every quarter radius";
                if (op.stencil && !(op.stencilScale > 0.0f)) return "stencil_scale: must be > 0";
                if (op.falloff == FalloffPreset::Custom && op.falloffCurve.empty()) return "falloff_curve: needs at least one key for falloff \"custom\"";
                for (const StrokePoint& point : op.points) {
                    if (!std::isfinite(point.position.x) || !std::isfinite(point.position.y) || !std::isfinite(point.position.z)) return "points: every point must be finite";
                }
                return "";
            case OpType::Gradient:
                if (op.stops.size() < 2) return "stops: needs at least 2 stops; give [{t = 0, value = ...}, {t = 1, value = ...}]";
                if ((op.gradientType == GradientType::Linear || op.gradientType == GradientType::Radial) && length3(op.gradientTo - op.gradientFrom) <= 0.0f)
                    return "to: must differ from from";
                if (op.gradientType == GradientType::Axis && length3(op.axis) <= 0.0f) return "axis: must not be zero; [0, 1, 0] runs bottom to top";
                return "";
            case OpType::Smooth:
            case OpType::Sharpen:
                if (op.iterations < 1 || op.iterations > 100) return "iterations: must be from 1 to 100";
                return "";
            case OpType::BakeCurvature:
                if (op.iterations < 0 || op.iterations > 100) return "radius_edges: must be from 0 to 100";
                return "";
            case OpType::Noise:
                if (!(op.noiseScale > 0.0f)) return opsNumber("scale: must be > 0, got ", op.noiseScale) + "; give the feature size in units";
                if (op.octaves < 1 || op.octaves > 12) return "octaves: must be from 1 to 12";
                return "";
            case OpType::Adjust:
                if (op.adjustKind == AdjustKind::Levels && !(op.gamma > 0.0f)) return "levels.gamma: must be > 0; 1 leaves the midtones";
                if (op.adjustKind == AdjustKind::Levels && !(op.inWhite > op.inBlack)) return "levels.in: white must be above black";
                if (op.adjustKind == AdjustKind::Clamp && !(op.outBlack <= op.outWhite)) return "clamp: max must be >= min";
                return "";
            case OpType::Channel:
                if (op.sourceChannel < 0 || op.sourceChannel > 3 || op.destChannel < 0 || op.destChannel > 3) return "channel: channels are \"r\", \"g\", \"b\", \"a\"";
                for (int8_t s : op.packSource) {
                    if (s < -1 || s > 3) return "pack: sources are \"r\", \"g\", \"b\", \"a\" or a number";
                }
                return "";
            case OpType::FromTexture:
                if (!op.image) return "texture: no image; give texture or material_slot";
                if (op.samples < 1 || op.samples > 256) return "samples: must be from 1 to 256";
                if (op.uvSet < 0 || op.uvSet > 1) return "uv_set: must be 0 or 1";
                if ((op.uvSet == 0 ? mesh.uv0 : mesh.uv1).empty()) return "uv_set: the mesh has no UV set " + std::to_string(op.uvSet) + "; use the set it has";
                return "";
            case OpType::BakeAO:
            case OpType::BakeThickness:
                if (op.rays < 1 || op.rays > 1024) return "rays: must be from 1 to 1024";
                if (!(op.distance > 0.0f)) return "distance: must be > 0";
                return "";
            case OpType::Transfer:
                if (!op.source) return "source_entity: no source; name the entity to take colors from";
                if (!(op.maxDistance > 0.0f)) return "max_distance: must be > 0";
                if (!(op.normalAngle >= 0.0f && op.normalAngle <= 180.0f)) return "normal_angle: must be from 0 to 180 degrees";
                return "";
            case OpType::ByDirection:
                if (length3(op.direction) <= 0.0f) return "direction: must not be zero; [0, 1, 0] is top-down";
                if (!(op.rangeMin <= op.rangeMax)) return "range: max must be >= min";
                return "";
            default: return "";
        }
    }

}

const char* opName(OpType type) {
    for (const OpsNameEntry& entry : kOpsNames) {
        if (entry.type == type) return entry.name;
    }
    return "set";
}

bool opFromName(const std::string& name, OpType& out) {
    for (const OpsNameEntry& entry : kOpsNames) {
        if (name == entry.name) {
            out = entry.type;
            return true;
        }
    }
    return false;
}

void StrokeState::reset() {
    touched.clear();
    start.clear();
    weight.clear();
    slot.clear();
    cornerSlot.clear();
    hasLastPoint = false;
    lastPoint = Vector3(0.0f, 0.0f, 0.0f);
    lastPressure = 1.0f;
    carry = 0.0f;
    hasLastStamp = false;
    lastStamp = Vector3(0.0f, 0.0f, 0.0f);
    lastStampRadius = 0.0f;
    lastStampPressure = 1.0f;
}

Vector4 Image::sample(float u, float v) const {
    if (width <= 0 || height <= 0 || pixels.size() < (size_t)width * (size_t)height) return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    if (!std::isfinite(u)) u = 0.0f;
    if (!std::isfinite(v)) v = 0.0f;
    const double x = (double)u * width - 0.5;
    const double y = (double)v * height - 0.5;
    const double x0 = std::floor(x), y0 = std::floor(y);
    const float fx = (float)(x - x0), fy = (float)(y - y0);
    auto wrapIndex = [&](double i, int size) -> int {
        if (wrap) {
            double m = std::fmod(i, (double)size);
            if (m < 0.0) m += size;
            return std::min(size - 1, (int)m);
        }
        return i < 0.0 ? 0 : (i > size - 1 ? size - 1 : (int)i);
    };
    const int ix0 = wrapIndex(x0, width), ix1 = wrapIndex(x0 + 1.0, width);
    const int iy0 = wrapIndex(y0, height), iy1 = wrapIndex(y0 + 1.0, height);
    const Vector4& a = pixels[(size_t)iy0 * width + ix0];
    const Vector4& b = pixels[(size_t)iy0 * width + ix1];
    const Vector4& c = pixels[(size_t)iy1 * width + ix0];
    const Vector4& d = pixels[(size_t)iy1 * width + ix1];
    return opsLerp4(opsLerp4(a, b, fx), opsLerp4(c, d, fx), fy);
}

float srgbToLinear(float c) {
    return pixelart::engineSrgbToLinear(c);
}

float linearToSrgb(float c) {
    return pixelart::engineLinearToSrgb(c);
}

bool parseHex(const std::string& hex, Vector4& linear) {
    size_t start = !hex.empty() && hex[0] == '#' ? 1 : 0;
    const size_t digits = hex.size() - start;
    if (digits != 6 && digits != 8) return false;
    uint8_t bytes[4] = {0, 0, 0, 255};
    for (size_t i = 0; i < digits / 2; i++) {
        int value = 0;
        for (size_t k = 0; k < 2; k++) {
            const char ch = hex[start + i * 2 + k];
            int d;
            if (ch >= '0' && ch <= '9') d = ch - '0';
            else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
            else return false;
            value = value * 16 + d;
        }
        bytes[i] = (uint8_t)value;
    }
    // alpha is coverage, not a display encoding: it stays linear
    linear = Vector4(srgbToLinear(bytes[0] / 255.0f), srgbToLinear(bytes[1] / 255.0f), srgbToLinear(bytes[2] / 255.0f), bytes[3] / 255.0f);
    return true;
}

std::string toHex(const Vector4& linear, bool alpha) {
    const uint8_t r = opsByte(linearToSrgb(opsClamp01(linear.x)));
    const uint8_t g = opsByte(linearToSrgb(opsClamp01(linear.y)));
    const uint8_t b = opsByte(linearToSrgb(opsClamp01(linear.z)));
    char buffer[16];
    if (alpha) std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", r, g, b, opsByte(linear.w));
    else std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", r, g, b);
    return buffer;
}

Vector4 blendValue(const Vector4& dst, const Vector4& src, Blend blend, float strength, ChannelMask channels) {
    Vector4 out = dst;
    for (size_t c = 0; c < 4; c++) {
        if ((channels & (1u << c)) == 0) continue;
        const float d = dst[c];
        const float s = src[c];
        float r = s;
        switch (blend) {
            case Blend::Replace: r = s; break;
            case Blend::Add: r = d + s; break;
            case Blend::Subtract: r = d - s; break;
            case Blend::Multiply: r = d * s; break;
            case Blend::Screen: r = 1.0f - (1.0f - d) * (1.0f - s); break;
            case Blend::Overlay: r = d < 0.5f ? 2.0f * d * s : 1.0f - 2.0f * (1.0f - d) * (1.0f - s); break;
            case Blend::Min: r = std::min(d, s); break;
            case Blend::Max: r = std::max(d, s); break;
            // a mix by the value's own alpha (a translucent swatch over the paint)
            case Blend::Lerp: r = d + (s - d) * src.w; break;
        }
        out[c] = d + (r - d) * strength;
    }
    return out;
}

float deltaE(const Vector4& a, const Vector4& b) {
    const pixelart::Lab la = pixelart::linearToOklab(a.x, a.y, a.z);
    const pixelart::Lab lb = pixelart::linearToOklab(b.x, b.y, b.z);
    const float dl = la.l - lb.l, da = la.a - lb.a, db = la.b - lb.b;
    return 100.0f * std::sqrt(dl * dl + da * da + db * db);
}

void initializeTarget(TargetData& data, const MeshView& mesh, InitialValue initial, const Vector4& custom) {
    data.base.resize(mesh.vertexCount());
    for (uint32_t v = 0; v < mesh.vertexCount(); v++) {
        const Vector4* asset = v < mesh.assetColors.size() ? &mesh.assetColors[v] : nullptr;
        data.base[v] = initialValueFor(initial, custom, asset);
    }
    data.corners.clear();
}

ChannelStats computeStats(const TargetData& data, const MeshView& mesh) {
    ChannelStats stats;
    std::vector<Vector4> base, corners;
    detail::denseValues(data, mesh, base, corners);
    stats.count = (uint32_t)corners.size();
    if (corners.empty()) return stats;
    std::vector<float> values(corners.size());
    for (size_t c = 0; c < 4; c++) {
        double sum = 0.0;
        for (size_t i = 0; i < corners.size(); i++) {
            const float v = corners[i][c];
            values[i] = v;
            sum += v;
            const float bin = std::floor(v * 16.0f);
            stats.histogram[c][(size_t)(bin < 0.0f ? 0.0f : (bin > 15.0f ? 15.0f : bin))]++;
        }
        std::sort(values.begin(), values.end());
        stats.min[c] = values.front();
        stats.max[c] = values.back();
        stats.mean[c] = (float)(sum / (double)values.size());
        stats.p05[c] = opsPercentile(values, 0.05f);
        stats.p50[c] = opsPercentile(values, 0.5f);
        stats.p95[c] = opsPercentile(values, 0.95f);
    }
    return stats;
}

uint64_t hashTarget(const TargetData& data) {
    uint64_t hash = detail::kFnvOffset;
    auto value = [&](const Vector4& v) {
        for (size_t c = 0; c < 4; c++) hash = detail::fnvFloat(hash, v[c] == 0.0f ? 0.0f : v[c]);
    };
    hash = detail::fnvU32(hash, (uint32_t)data.base.size());
    for (const Vector4& v : data.base) value(v);
    hash = detail::fnvU32(hash, (uint32_t)data.corners.size());
    for (const CornerValue& entry : data.corners) {
        hash = detail::fnvU32(hash, entry.corner);
        value(entry.value);
    }
    return hash;
}

std::vector<PaletteEntry> palette(const TargetData& data, const MeshView& mesh, size_t maxEntries) {
    std::vector<Vector4> base, corners;
    detail::denseValues(data, mesh, base, corners);
    std::vector<uint32_t> keys(corners.size());
    for (size_t i = 0; i < corners.size(); i++) {
        const Vector4& v = corners[i];
        keys[i] = ((uint32_t)opsByte(v.x) << 24) | ((uint32_t)opsByte(v.y) << 16) | ((uint32_t)opsByte(v.z) << 8) | opsByte(v.w);
    }
    std::sort(keys.begin(), keys.end());
    struct Counted {
        uint32_t key;
        uint32_t count;
    };
    std::vector<Counted> counted;
    for (size_t i = 0; i < keys.size();) {
        size_t j = i;
        while (j < keys.size() && keys[j] == keys[i]) j++;
        counted.push_back(Counted{keys[i], (uint32_t)(j - i)});
        i = j;
    }
    if (counted.size() > maxEntries) {
        std::sort(counted.begin(), counted.end(), [](const Counted& a, const Counted& b) { return a.count != b.count ? a.count > b.count : a.key < b.key; });
        counted.resize(maxEntries);
    }
    struct Sorted {
        PaletteEntry entry;
        uint32_t key;
        float hue;
        float value;
    };
    std::vector<Sorted> sorted;
    for (const Counted& c : counted) {
        Sorted s;
        s.key = c.key;
        s.entry.value = Vector4(((c.key >> 24) & 255u) / 255.0f, ((c.key >> 16) & 255u) / 255.0f, ((c.key >> 8) & 255u) / 255.0f, (c.key & 255u) / 255.0f);
        s.entry.corners = c.count;
        const detail::Hsv hsv = detail::toHsv(s.entry.value);
        // grays have no hue: they lead, dark to light
        s.hue = hsv.s > 0.0f ? hsv.h : -1.0f;
        s.value = hsv.v;
        sorted.push_back(s);
    }
    std::sort(sorted.begin(), sorted.end(), [](const Sorted& a, const Sorted& b) {
        if (a.hue != b.hue) return a.hue < b.hue;
        if (a.value != b.value) return a.value < b.value;
        return a.key < b.key;
    });
    std::vector<PaletteEntry> out;
    out.reserve(sorted.size());
    for (const Sorted& s : sorted) out.push_back(s.entry);
    return out;
}

std::vector<uint32_t> resolveElements(const TargetData& data, const MeshView& mesh, const Target& target, std::span<const Filter> filters,
                                      const ApplyOptions& options, ElementKind& kind) {
    OpEngine engine(data, mesh, options);
    std::vector<uint32_t> ids;
    engine.resolveTarget(target, kind, ids);
    if (filters.empty()) return ids;
    size_t kept = 0;
    for (uint32_t id : ids) {
        if (engine.passesAll(filters, kind, id)) ids[kept++] = id;
    }
    ids.resize(kept);
    return ids;
}

namespace {

    OpsResult opsApply(TargetData& data, const MeshView& mesh, std::span<const Op> ops, const ApplyOptions& options, bool dryRun,
                       const bake::Field* injectedField) {
        OpsResult result;
        auto where = [&](size_t i) { return "ops[" + std::to_string(i + options.firstIndex) + "]"; };
        auto fail = [&](size_t i, const std::string& message) {
            result.ok = false;
            result.failedOp = i;
            result.ops.clear();
            if (message.rfind("ops[", 0) == 0) result.error = message;
            else if (!message.empty() && message.find(':') != std::string::npos && message.find(':') < message.find(' ')) result.error = where(i) + "." + message;
            else result.error = where(i) + ": " + message;
            result.hash = hashTarget(data);
            return result;
        };
        if (!mesh.built()) {
            result.error = "the mesh view is not built; call MeshView::build first";
            return result;
        }
        if (!data.base.empty() && data.base.size() != mesh.vertexCount()) {
            result.error = "the target has " + std::to_string(data.base.size()) + " values for a mesh of " + std::to_string(mesh.vertexCount()) +
                           " vertices (stale data); transfer it to the new mesh first";
            result.hash = hashTarget(data);
            return result;
        }
        for (const CornerValue& entry : data.corners) {
            if (entry.corner >= mesh.indices.size()) {
                result.error = "the target has a corner override for corner " + std::to_string(entry.corner) + " of a mesh with " +
                               std::to_string(mesh.indices.size()) + " corners (stale data); transfer it to the new mesh first";
                result.hash = hashTarget(data);
                return result;
            }
        }
        for (size_t i = 0; i < ops.size(); i++) {
            const std::string message = opsValidateOp(ops[i], mesh);
            if (!message.empty()) return fail(i, message);
        }

        OpEngine engine(data, mesh, options);
        engine.injectedField = injectedField;
        TargetData before;
        const TargetData* beforeData = &data;
        if (engine.fresh) {
            before = data;
            initializeTarget(before, mesh, options.initial, options.customInitial);
            beforeData = &before;
        }
        if (options.stats) result.before = computeStats(*beforeData, mesh);
        const bool hadOverrides = !engine.fresh && !data.corners.empty();

        StrokeState stroke;
        bool usesStroke = false;
        for (const Op& op : ops) usesStroke = usesStroke || op.type == OpType::Stroke;
        if (usesStroke && options.stroke) stroke = *options.stroke;

        result.ops.resize(ops.size());
        uint32_t splitTotal = 0;
        uint32_t underSampled = 0;
        for (size_t i = 0; i < ops.size(); i++) {
            if (options.cancel && options.cancel->load(std::memory_order_relaxed)) engine.cancelFlag = true;
            if (engine.cancelFlag) {
                fail(i, "cancelled");
                result.cancelled = true;
                return result;
            }
            const Op& op = ops[i];
            OpResult& opResult = result.ops[i];
            std::string error;
            bool ok = true;
            switch (op.type) {
                case OpType::Stroke:
                    // a stroke op without a caller state still caps within the op (R31)
                    if (!options.stroke) stroke.reset();
                    ok = engine.runStroke(op, opResult, stroke, error);
                    break;
                case OpType::Gradient: ok = engine.runGradient(op, opResult); break;
                case OpType::Noise: ok = engine.runNoise(op, opResult, error); break;
                case OpType::Smooth:
                case OpType::Sharpen: ok = engine.runSmooth(op, opResult); break;
                case OpType::FromTexture:
                case OpType::BakeAO:
                case OpType::BakeThickness:
                case OpType::BakeCurvature:
                case OpType::Transfer: ok = engine.runBake(op, opResult, error); break;
                default: ok = engine.runSimple(op, opResult); break;
            }
            if (!ok || engine.cancelFlag) {
                if (engine.cancelFlag) {
                    fail(i, "cancelled");
                    result.cancelled = true;
                    return result;
                }
                return fail(i, error.empty() ? std::string("failed") : error);
            }
            engine.finishOp(opResult);
            splitTotal += opResult.splitAdded;
            underSampled += opResult.underSampledFaces;
            if (options.progress) options.progress((float)(i + 1) / (float)ops.size());
        }

        TargetData out;
        engine.finish(out);
        result.ok = true;
        result.hash = hashTarget(out);
        if (options.stats) result.after = computeStats(out, mesh);
        if (splitTotal > 0 && !hadOverrides) {
            result.warnings.push_back(Warning{"split_vertices_added", std::to_string(splitTotal) +
                                              " corner overrides were created (hard edges, split vertices); expected for faces and corners targets, use a vertices target for smooth colors"});
        }
        if (underSampled > 0) {
            result.warnings.push_back(Warning{"low_density", std::to_string(underSampled) +
                                              " faces cover more texels than their corners can hold; import a denser mesh to keep the texture's detail"});
        }
        if (!dryRun) {
            data = std::move(out);
            if (usesStroke && options.stroke) *options.stroke = std::move(stroke);
        }
        return result;
    }

}

OpsResult applyOps(TargetData& data, const MeshView& mesh, std::span<const Op> ops, const ApplyOptions& options, bool dryRun) {
    return opsApply(data, mesh, ops, options, dryRun, nullptr);
}

OpsResult detail::applyOpsWithField(TargetData& data, const MeshView& mesh, std::span<const Op> ops, const ApplyOptions& options,
                                    const bake::Field& field, bool dryRun) {
    return opsApply(data, mesh, ops, options, dryRun, &field);
}

}
