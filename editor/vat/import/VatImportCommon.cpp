// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/import/VatImportInternal.h"

#include "util/FileUtils.h"
#include "htmlui/compat/Hash.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::vat;
using jasperengine::editor::FileUtils;

namespace jasperengine::editor::vat::importer{

// ---- math ----

D3 normalize(const D3& a, const D3& fallback){
    const double l = length(a);
    if (!(l > 1e-20) || !std::isfinite(l)) return fallback;
    return a * (1.0 / l);
}

DQ qmul(const DQ& a, const DQ& b){
    return DQ(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
              a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
              a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}

DQ qnormalize(const DQ& q){
    const double l = std::sqrt(qdot(q, q));
    if (!(l > 1e-20) || !std::isfinite(l)) return DQ();
    return DQ(q.x / l, q.y / l, q.z / l, q.w / l);
}

D3 rotate(const DQ& q, const D3& v){
    const D3 u(q.x, q.y, q.z);
    const D3 c = cross(u, v) + v * q.w;
    return v + cross(u, c) * 2.0;
}

DQ quatFromColumns(const D3& X, const D3& Y, const D3& Z){
    // m[row][col] with columns X, Y, Z
    const double m00 = X.x, m10 = X.y, m20 = X.z;
    const double m01 = Y.x, m11 = Y.y, m21 = Y.z;
    const double m02 = Z.x, m12 = Z.y, m22 = Z.z;
    const double trace = m00 + m11 + m22;
    DQ q;
    if (trace > 0.0){
        const double s = std::sqrt(trace + 1.0) * 2.0;
        q = DQ((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s);
    }else if (m00 > m11 && m00 > m22){
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        q = DQ(0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s);
    }else if (m11 > m22){
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        q = DQ((m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s);
    }else{
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        q = DQ((m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s);
    }
    return qnormalize(q);
}

DQ tangentFrame(const D3& normal, const D3& tangent){
    const D3 n = normalize(normal);
    D3 t = tangent - n * dot(n, tangent);
    if (length(t) < 1e-6){
        // any perpendicular: the axis least aligned with the normal
        const D3 axis = std::fabs(n.x) < 0.9 ? D3(1, 0, 0) : D3(0, 1, 0);
        t = axis - n * dot(n, axis);
    }
    t = normalize(t, D3(1, 0, 0));
    const D3 b = cross(n, t);
    return quatFromColumns(t, b, n);
}

// ---- half ----

uint16_t floatToHalf(float value){
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent == 0xFFu){
        return (uint16_t)(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
    }
    const int e = (int)exponent - 127 + 15;
    if (e >= 31) return (uint16_t)(sign | 0x7C00u);
    if (e <= 0){
        if (e < -10) return (uint16_t)sign;
        mantissa |= 0x800000u;
        const int shift = 14 - e;
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        if (rest > halfway || (rest == halfway && (half & 1u))) half++;
        return (uint16_t)(sign | half);
    }
    uint32_t half = ((uint32_t)e << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) half++;   // may carry into the exponent (to infinity)
    return (uint16_t)(sign | half);
}

float halfToFloat(uint16_t half){
    const uint32_t sign = (uint32_t)(half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1Fu;
    uint32_t mantissa = half & 0x3FFu;
    uint32_t bits;
    if (exponent == 0){
        if (mantissa == 0){
            bits = sign;
        }else{
            int e = -1;
            do{ e++; mantissa <<= 1; }while ((mantissa & 0x400u) == 0);
            bits = sign | ((uint32_t)(127 - 15 - e) << 23) | ((mantissa & 0x3FFu) << 13);
        }
    }else if (exponent == 31){
        bits = sign | 0x7F800000u | (mantissa << 13);
    }else{
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
}

// ---- presets ----

bool presetLimits(const std::string& name, PresetLimits& out){
    if (name == "mobile_web" || name.empty()){
        out.width = 2048;
        out.maxTexels = 2048ull * 2048ull;
        out.memoryWarningBytes = 32ull * 1024ull * 1024ull;
        return true;
    }
    if (name == "desktop"){
        out.width = 4096;
        out.maxTexels = 4096ull * 8192ull;
        out.memoryWarningBytes = 256ull * 1024ull * 1024ull;
        return true;
    }
    return false;
}

// ---- payload ----

uint64_t PayloadBuilder::texelCount() const{
    return payloadTexels(mode, layout.samples, mode == VatMode::Bones ? layout.bones : layout.vertices, layout.texelsPerSample(mode));
}

bool PayloadBuilder::allocate(){
    const uint64_t texelsNeeded = texelCount();
    const uint64_t rows = (texelsNeeded + (uint64_t)layout.width - 1) / (uint64_t)layout.width;
    layout.height = (int)std::max<uint64_t>(rows, 1);
    try{
        texels.assign((size_t)((uint64_t)layout.width * (uint64_t)layout.height * 4), 0.0f);
    }catch (const std::bad_alloc&){
        texels.clear();
        return false;
    }
    return true;
}

uint64_t payloadTexels(VatMode mode, int samples, int elements, int perSample){
    (void)mode;
    return (uint64_t)std::max(samples, 0) * (uint64_t)std::max(elements, 0) * (uint64_t)std::max(perSample, 0);
}

bool checkPayloadSize(VatMode mode, int samples, int elements, int perSample, const PresetLimits& limits,
                      const std::string& what, const std::vector<std::string>& fixes, VatImportResult& result){
    const uint64_t texels = payloadTexels(mode, samples, elements, perSample);
    if (texels <= limits.maxTexels) return true;
    std::string fix;
    for (size_t i = 0; i < fixes.size(); i++){
        if (i > 0) fix += "; or ";
        fix += fixes[i];
    }
    addError(result, "too_large",
             what + " need " + std::to_string(texels) + " texels (" + std::to_string(samples) + " frames x " +
             std::to_string(elements) + (mode == VatMode::Bones ? " bones x " : " vertices x ") + std::to_string(perSample) +
             " texels), over the preset's " + std::to_string(limits.maxTexels),
             fix.empty() ? "use the desktop preset (preset: desktop)" : fix);
    return false;
}

namespace{

    // the clip each global frame belongs to (-1 between clips: none expected)
    std::vector<int> frameClips(const std::vector<VatClip>& clips, int samples){
        std::vector<int> out((size_t)std::max(samples, 0), -1);
        for (size_t c = 0; c < clips.size(); c++){
            for (int f = clips[c].start; f < clips[c].start + clips[c].frames && f < samples; f++){
                if (f >= 0) out[(size_t)f] = (int)c;
            }
        }
        return out;
    }

    void bakeTransform(Assembled& in){
        const MeshXform& m = in.xform;
        if (m.identity) return;
        PayloadBuilder& p = in.payload;
        const VatLayout& layout = p.layout;
        const std::vector<int> owner = frameClips(in.clips, layout.samples);
        if (p.mode == VatMode::Vertices){
            for (int f = 0; f < layout.samples; f++){
                const bool absolute = owner[(size_t)f] >= 0 && in.clips[(size_t)owner[(size_t)f]].restNone;
                for (int e = 0; e < layout.vertices; e++){
                    float* t = p.at(f, e, 0);
                    const D3 v(t[0], t[1], t[2]);
                    const D3 r = absolute ? m.point(v) : m.vector(v);
                    t[0] = (float)r.x; t[1] = (float)r.y; t[2] = (float)r.z;
                    if (layout.normals){
                        float* q = p.at(f, e, 1);
                        const DQ out = qnormalize(qmul(m.rotation, DQ(q[0], q[1], q[2], q[3])));
                        q[0] = (float)out.x; q[1] = (float)out.y; q[2] = (float)out.z; q[3] = (float)out.w;
                    }
                }
            }
            for (D3& rest : in.elementRest) rest = m.point(rest);
        }else{
            const DQ inv = qconj(m.rotation);
            for (int f = 0; f < layout.samples; f++){
                for (int b = 0; b < layout.bones; b++){
                    float* q = p.at(f, b, 0);
                    float* t = p.at(f, b, 1);
                    const DQ rb(q[0], q[1], q[2], q[3]);
                    const DQ r = qnormalize(qmul(qmul(m.rotation, rb), inv));
                    const double s = t[3];
                    const D3 tb(t[0], t[1], t[2]);
                    const D3 out = m.translation + rotate(m.rotation, tb * m.scale) - rotate(r, m.translation * s);
                    q[0] = (float)r.x; q[1] = (float)r.y; q[2] = (float)r.z; q[3] = (float)r.w;
                    t[0] = (float)out.x; t[1] = (float)out.y; t[2] = (float)out.z;
                }
            }
        }
        VatMeshData& mesh = in.mesh;
        for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3){
            const D3 v = m.point(D3(mesh.positions[i], mesh.positions[i + 1], mesh.positions[i + 2]));
            mesh.positions[i] = (float)v.x; mesh.positions[i + 1] = (float)v.y; mesh.positions[i + 2] = (float)v.z;
        }
        for (size_t i = 0; i + 2 < mesh.normals.size(); i += 3){
            const D3 v = normalize(m.direction(D3(mesh.normals[i], mesh.normals[i + 1], mesh.normals[i + 2])));
            mesh.normals[i] = (float)v.x; mesh.normals[i + 1] = (float)v.y; mesh.normals[i + 2] = (float)v.z;
        }
        for (size_t i = 0; i + 3 < mesh.tangents.size(); i += 4){
            const D3 v = normalize(m.direction(D3(mesh.tangents[i], mesh.tangents[i + 1], mesh.tangents[i + 2])), D3(1, 0, 0));
            mesh.tangents[i] = (float)v.x; mesh.tangents[i + 1] = (float)v.y; mesh.tangents[i + 2] = (float)v.z;
        }
    }

    void enforceContinuity(PayloadBuilder& p){
        const VatLayout& layout = p.layout;
        int k = -1;
        int elements = 0;
        if (p.mode == VatMode::Vertices){
            if (!layout.normals) return;
            k = 1;
            elements = layout.vertices;
        }else{
            k = 0;
            elements = layout.bones;
        }
        for (int e = 0; e < elements; e++){
            for (int f = 1; f < layout.samples; f++){
                const float* a = p.at(f - 1, e, k);
                float* b = p.at(f, e, k);
                const double d = (double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2] + (double)a[3] * b[3];
                if (d < 0.0){
                    b[0] = -b[0]; b[1] = -b[1]; b[2] = -b[2]; b[3] = -b[3];
                }
            }
        }
    }

}

bool finishImport(Assembled& in, const VatImportSpec& spec, VatImportResult& result){
    PresetLimits limits;
    presetLimits(spec.preset, limits);
    PayloadBuilder& p = in.payload;
    VatLayout& layout = p.layout;

    if (in.mesh.vertexCount() > (size_t)VAT_MAX_VERTICES){
        addError(result, "vertex_limit", "the mesh has " + std::to_string(in.mesh.vertexCount()) + " vertices, at most " +
                 std::to_string(VAT_MAX_VERTICES), "simplify the mesh before exporting");
        return false;
    }
    if (!in.xform.identity){
        addWarning(result, "node_transform_baked", "the mesh node has a transform: it was applied to the vertices and the animation",
                   "nothing to do (export with an identity transform to keep the mesh space)");
    }
    bakeTransform(in);
    enforceContinuity(p);

    // bounds of every clip (exact values, before any half rounding)
    const std::vector<int> owner = frameClips(in.clips, layout.samples);
    for (VatClip& clip : in.clips) clip.bounds = VatBounds();
    if (p.mode == VatMode::Vertices){
        for (int f = 0; f < layout.samples; f++){
            if (owner[(size_t)f] < 0) continue;
            VatClip& clip = in.clips[(size_t)owner[(size_t)f]];
            for (int e = 0; e < layout.vertices; e++){
                const float* t = p.at(f, e, 0);
                D3 pos(t[0], t[1], t[2]);
                if (!clip.restNone) pos = pos + in.elementRest[(size_t)e];
                clip.bounds.extend(Vector3((float)pos.x, (float)pos.y, (float)pos.z));
            }
        }
    }else{
        const VatMeshData& mesh = in.mesh;
        const size_t vertices = mesh.vertexCount();
        for (int f = 0; f < layout.samples; f++){
            if (owner[(size_t)f] < 0) continue;
            VatClip& clip = in.clips[(size_t)owner[(size_t)f]];
            for (size_t v = 0; v < vertices; v++){
                const int bone = mesh.joints[v * 4];
                const float* q = p.at(f, bone, 0);
                const float* t = p.at(f, bone, 1);
                const D3 rest(mesh.positions[v * 3], mesh.positions[v * 3 + 1], mesh.positions[v * 3 + 2]);
                const D3 pos = rotate(DQ(q[0], q[1], q[2], q[3]), rest * t[3]) + D3(t[0], t[1], t[2]);
                clip.bounds.extend(Vector3((float)pos.x, (float)pos.y, (float)pos.z));
            }
        }
    }
    VatBounds total;
    for (const VatClip& clip : in.clips) total.extend(clip.bounds);
    if (!total.valid) total.extend(Vector3(0, 0, 0));
    const Vector3 diagonal = total.max - total.min;
    const double diagonalMm = std::sqrt((double)diagonal.x * diagonal.x + (double)diagonal.y * diagonal.y + (double)diagonal.z * diagonal.z) * 1000.0;
    const double tolerance = std::max((double)spec.toleranceMm, diagonalMm * 0.0005);

    // precision
    double maxError = 0.0, sumError = 0.0;
    uint64_t counted = 0;
    if (p.mode == VatMode::Vertices){
        for (int f = 0; f < layout.samples; f++){
            for (int e = 0; e < layout.vertices; e++){
                const float* t = p.at(f, e, 0);
                double error2 = 0.0;
                for (int c = 0; c < 3; c++){
                    const double d = (double)halfToFloat(floatToHalf(t[c])) - (double)t[c];
                    error2 += d * d;
                }
                const double error = std::sqrt(error2) * 1000.0;
                maxError = std::max(maxError, error);
                sumError += error;
                counted++;
            }
        }
    }
    const double meanError = counted ? sumError / (double)counted : 0.0;
    VatPrecision precision = VatPrecision::Float;
    if (p.mode == VatMode::Vertices){
        if (spec.precision == "half"){
            precision = VatPrecision::Half;
        }else if (spec.precision == "float"){
            precision = VatPrecision::Float;
        }else if (maxError <= tolerance){
            precision = VatPrecision::Half;
        }else{
            precision = VatPrecision::Float;
            addWarning(result, "precision_switched",
                       "half precision would move vertices up to " + num(maxError) + " mm (tolerance " + num(tolerance) +
                       " mm): the payload is float (twice the memory)",
                       "accept it, or force precision: half and live with the error");
        }
    }
    layout.precision = precision;

    // the payload bytes
    VatPayload& payload = result.payload;
    payload.precision = precision;
    payload.width = layout.width;
    payload.height = layout.height;
    const size_t values = p.texels.size();
    try{
        if (precision == VatPrecision::Float){
            payload.bytes.resize(values * 4);
            for (size_t i = 0; i < values; i++){
                uint32_t bits;
                std::memcpy(&bits, &p.texels[i], 4);
                payload.bytes[i * 4] = (uint8_t)(bits & 0xFF);
                payload.bytes[i * 4 + 1] = (uint8_t)((bits >> 8) & 0xFF);
                payload.bytes[i * 4 + 2] = (uint8_t)((bits >> 16) & 0xFF);
                payload.bytes[i * 4 + 3] = (uint8_t)((bits >> 24) & 0xFF);
            }
        }else{
            payload.bytes.resize(values * 2);
            for (size_t i = 0; i < values; i++){
                const uint16_t h = floatToHalf(p.texels[i]);
                payload.bytes[i * 2] = (uint8_t)(h & 0xFF);
                payload.bytes[i * 2 + 1] = (uint8_t)(h >> 8);
            }
        }
    }catch (const std::bad_alloc&){
        addError(result, "too_large", "not enough memory for the payload (" + std::to_string(values / 4) + " texels)",
                 "lower the frame count or the resolution of the export");
        return false;
    }
    std::vector<float>().swap(p.texels);

    VatData& data = result.data;
    data.mode = p.mode;
    data.layout = layout;
    data.clips = in.clips;
    data.bounds = total;
    data.stats.bytes = payload.bytes.size();
    if (precision == VatPrecision::Half){
        data.stats.maxErrorMm = (float)maxError;
        data.stats.meanErrorMm = (float)meanError;
    }
    result.mesh = std::move(in.mesh);

    VatImportReport& report = result.report;
    report.mode = p.mode;
    report.modeReason = in.modeReason;
    report.vertices = p.mode == VatMode::Vertices ? layout.vertices : (int)result.mesh.vertexCount();
    report.meshVertices = (int)result.mesh.vertexCount();
    report.bones = p.mode == VatMode::Bones ? layout.bones : 0;
    report.sourceFrames = in.sourceFrames;
    report.frames = layout.samples;
    report.upsample = in.upsample;
    report.fps = in.sourceFps;
    report.width = layout.width;
    report.height = layout.height;
    report.precision = precision;
    report.bytes = payload.bytes.size();
    report.maxErrorMm = precision == VatPrecision::Half ? (float)maxError : 0.0f;
    report.meanErrorMm = precision == VatPrecision::Half ? (float)meanError : 0.0f;
    report.quantizationErrorMm = report.maxErrorMm;
    report.toleranceMm = (float)tolerance;
    report.interpolationErrorMm = in.interpolationErrorMm;
    report.fetchesPerVertex = p.mode == VatMode::Bones ? 4 : 2 * layout.texelsPerSample(VatMode::Vertices);
    const uint64_t texelBytes = precision == VatPrecision::Float ? 16 : 8;
    const uint64_t perFrame = (uint64_t)(p.mode == VatMode::Bones ? layout.bones : layout.vertices) *
                              (uint64_t)layout.texelsPerSample(p.mode) * texelBytes;
    report.clips.clear();
    for (const VatClip& clip : in.clips){
        VatImportClipReport row;
        row.name = clip.name;
        row.frames = clip.frames;
        row.fps = clip.fps;
        row.bytes = perFrame * (uint64_t)clip.frames;
        report.clips.push_back(row);
    }
    if (report.bytes > limits.memoryWarningBytes){
        addWarning(result, "memory_high",
                   "the payload takes " + num((double)report.bytes / (1024.0 * 1024.0)) + " MB of GPU memory (over the preset's " +
                   num((double)limits.memoryWarningBytes / (1024.0 * 1024.0)) + " MB guideline)",
                   p.mode == VatMode::Vertices ? "turn off normals or colours, or export fewer frames"
                                               : "export fewer frames, or lower the upsampling (upsample)");
    }
    return true;
}

// ---- mesh building ----

namespace{

    struct CornerKey{
        uint32_t point;
        int element;
        float data[13];
        bool operator==(const CornerKey& o) const{
            return point == o.point && element == o.element && std::memcmp(data, o.data, sizeof(data)) == 0;
        }
    };
    struct CornerKeyHash{
        size_t operator()(const CornerKey& k) const{
            uint64_t h = 1469598103934665603ull;
            auto mix = [&](const void* p, size_t n){
                const unsigned char* b = (const unsigned char*)p;
                for (size_t i = 0; i < n; i++){ h ^= b[i]; h *= 1099511628211ull; }
            };
            mix(&k.point, sizeof(k.point));
            mix(&k.element, sizeof(k.element));
            mix(k.data, sizeof(k.data));
            return (size_t)h;
        }
    };

}

void buildMesh(const SourceMesh& src, const std::vector<int>& element, int materialUv, bool withNormals,
               bool withColors, BuiltMesh& out, bool twoSided){
    std::unordered_map<CornerKey, uint32_t, CornerKeyHash> unique;
    std::map<int, std::vector<uint32_t>> byMaterial;
    VatMeshData& mesh = out.mesh;
    const bool hasUv = materialUv >= 0 && materialUv < (int)src.uvs.size();
    const bool hasNormals = withNormals && !src.normals.empty();
    const bool hasTangents = withNormals && !src.tangents.empty() && !src.normals.empty();
    const bool hasColors = withColors && !src.colors.empty();
    for (size_t t = 0; t < src.triangles(); t++){
        bool keep = true;
        for (int c = 0; c < 3; c++) keep = keep && element[t * 3 + (size_t)c] >= 0;
        if (!keep) continue;
        for (int c = 0; c < 3; c++){
            const size_t corner = t * 3 + (size_t)c;
            CornerKey key;
            std::memset(&key, 0, sizeof(key));
            key.point = src.point[corner];
            key.element = element[corner];
            if (hasUv){
                key.data[0] = (float)src.uvs[(size_t)materialUv][corner * 2];
                key.data[1] = (float)src.uvs[(size_t)materialUv][corner * 2 + 1];
            }
            if (hasNormals) for (int k = 0; k < 3; k++) key.data[2 + k] = src.normals[corner * 3 + (size_t)k];
            if (hasTangents) for (int k = 0; k < 4; k++) key.data[5 + k] = src.tangents[corner * 4 + (size_t)k];
            if (hasColors) for (int k = 0; k < 4; k++) key.data[9 + k] = src.colors[corner * 4 + (size_t)k];
            auto found = unique.find(key);
            uint32_t index;
            if (found == unique.end()){
                index = (uint32_t)mesh.vertexCount();
                unique.emplace(key, index);
                const D3 p = src.position(corner);
                mesh.positions.push_back((float)p.x);
                mesh.positions.push_back((float)p.y);
                mesh.positions.push_back((float)p.z);
                if (hasUv){
                    mesh.uv0.push_back(key.data[0]);
                    mesh.uv0.push_back(src.gltf ? key.data[1] : 1.0f - key.data[1]);
                }
                if (hasNormals) for (int k = 0; k < 3; k++) mesh.normals.push_back(key.data[2 + k]);
                if (hasTangents) for (int k = 0; k < 4; k++) mesh.tangents.push_back(key.data[5 + k]);
                if (hasColors) for (int k = 0; k < 4; k++) mesh.colors.push_back(key.data[9 + k]);
                mesh.vatIndex.push_back((float)key.element);
                out.sourceCorner.push_back(corner);
            }else{
                index = found->second;
            }
            byMaterial[src.triangleMaterial[t]].push_back(index);
        }
    }
    mesh.materials = src.materials;
    for (auto& entry : byMaterial){
        VatSubmesh submesh;
        submesh.indices = std::move(entry.second);
        submesh.material = entry.first;
        mesh.submeshes.push_back(std::move(submesh));
    }
    if (twoSided) makeDoubleSided(mesh);
}

void makeDoubleSided(VatMeshData& mesh){
    if (mesh.materials.empty()){
        VatMaterialData material;
        material.name = "VAT";
        mesh.materials.push_back(material);
    }
    for (VatSubmesh& submesh : mesh.submeshes) if (submesh.material < 0) submesh.material = 0;
    for (VatMaterialData& material : mesh.materials) material.doubleSided = true;
}

D3 cornerTangent(const SourceMesh& src, size_t corner){
    if (!src.tangents.empty()) return D3(src.tangents[corner * 4], src.tangents[corner * 4 + 1], src.tangents[corner * 4 + 2]);
    return D3(1, 0, 0);
}
D3 cornerNormal(const SourceMesh& src, size_t corner){
    if (!src.normals.empty()) return D3(src.normals[corner * 3], src.normals[corner * 3 + 1], src.normals[corner * 3 + 2]);
    return D3(0, 1, 0);
}

// ---- issues ----

void addError(VatImportResult& result, const std::string& kind, const std::string& message, const std::string& fix){
    VatImportIssue issue;
    issue.kind = kind;
    issue.message = message;
    issue.fix = fix;
    issue.error = true;
    result.errors.push_back(issue);
}

void addWarning(VatImportResult& result, const std::string& kind, const std::string& message, const std::string& fix){
    for (const VatImportIssue& existing : result.report.warnings){
        if (existing.kind == kind && existing.message == message) return;
    }
    VatImportIssue issue;
    issue.kind = kind;
    issue.message = message;
    issue.fix = fix;
    result.report.warnings.push_back(issue);
}

std::string num(double value){
    if (!std::isfinite(value)) return value != value ? "nan" : (value > 0 ? "inf" : "-inf");
    char text[64];
    for (int digits = 1; digits <= 17; digits++){
        std::snprintf(text, sizeof(text), "%.*g", digits, value);
        if (std::strtod(text, nullptr) == value || digits >= 6) break;
    }
    return text;
}

// ---- files ----

std::string fileName(const std::string& path){
    return FileUtils::pathToUtf8(FileUtils::pathFromUtf8(path).filename());
}

bool fileExists(const std::string& path){
    if (path.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(FileUtils::pathFromUtf8(path), ec);
}

bool readText(const std::string& path, std::string& out){
    std::ifstream file(FileUtils::pathFromUtf8(path), std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

std::string fileSha1(const std::string& path){
    std::string bytes;
    if (!readText(path, bytes)) return {};
    return "sha1:" + jasperengine::htmlui::compat::sha1Hex(bytes);
}

std::string lower(std::string text){
    for (char& c : text) c = (char)std::tolower((unsigned char)c);
    return text;
}

}
