// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 4.6, internal to editor/vat/import)
// - Purpose: what the Houdini VAT 3 and OpenVAT importers share: double-precision vector and
//   quaternion helpers, IEEE half encoding, the raw mesh reader (ufbx / tinygltf, bypassing the
//   model import: no unit conversion, raw V, every UV set, tangents, no welding or reordering),
//   the payload builder and finishImport (transform baking, hemisphere continuity, precision,
//   bounds, layout, report).
// - Not a public API: the window, MCP tool and CLI use VatImport.h only.

#ifndef VAT_IMPORT_INTERNAL_H
#define VAT_IMPORT_INTERNAL_H

#include "vat/import/VatImport.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::vat::importer{

    // ---- math (double) ----

    struct D3{
        double x = 0, y = 0, z = 0;
        D3() = default;
        D3(double x_, double y_, double z_): x(x_), y(y_), z(z_) {}
        D3 operator+(const D3& o) const { return D3(x + o.x, y + o.y, z + o.z); }
        D3 operator-(const D3& o) const { return D3(x - o.x, y - o.y, z - o.z); }
        D3 operator*(double s) const { return D3(x * s, y * s, z * s); }
        D3 operator-() const { return D3(-x, -y, -z); }
        double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    };
    inline double dot(const D3& a, const D3& b){ return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline D3 cross(const D3& a, const D3& b){ return D3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
    inline double length(const D3& a){ return std::sqrt(dot(a, a)); }
    D3 normalize(const D3& a, const D3& fallback = D3(0, 1, 0));
    inline D3 lerp(const D3& a, const D3& b, double t){ return a + (b - a) * t; }

    // (x, y, z, w), Hamilton product, rotate(q, v) = v + 2 cross(q.xyz, cross(q.xyz, v) + w v)
    struct DQ{
        double x = 0, y = 0, z = 0, w = 1;
        DQ() = default;
        DQ(double x_, double y_, double z_, double w_): x(x_), y(y_), z(z_), w(w_) {}
    };
    inline double qdot(const DQ& a, const DQ& b){ return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
    inline DQ qneg(const DQ& q){ return DQ(-q.x, -q.y, -q.z, -q.w); }
    inline DQ qconj(const DQ& q){ return DQ(-q.x, -q.y, -q.z, q.w); }
    DQ qmul(const DQ& a, const DQ& b);
    DQ qnormalize(const DQ& q);
    D3 rotate(const DQ& q, const D3& v);
    // the rotation whose matrix has columns X, Y, Z (orthonormal, right-handed)
    DQ quatFromColumns(const D3& X, const D3& Y, const D3& Z);
    // the engine's tangent frame (spec 2.2): +X -> tangent, +Y -> bitangent = cross(N, T), +Z ->
    // normal; the tangent is made orthogonal to the normal (any perpendicular when degenerate)
    DQ tangentFrame(const D3& normal, const D3& tangent);

    inline double fract(double v){ return v - std::floor(v); }

    // ---- half floats ----

    uint16_t floatToHalf(float value);      // round to nearest even, overflow to infinity
    float halfToFloat(uint16_t half);

    // ---- the node transform of the source mesh (a similarity: rotation, uniform scale, translation) ----

    struct MeshXform{
        bool identity = true;
        DQ rotation;
        double scale = 1.0;
        D3 translation;

        D3 point(const D3& p) const { return identity ? p : rotate(rotation, p * scale) + translation; }
        D3 vector(const D3& v) const { return identity ? v : rotate(rotation, v * scale); }
        D3 direction(const D3& v) const { return identity ? v : rotate(rotation, v); }
    };

    // ---- the raw mesh ----

    struct SourceMesh{
        // triangles, 3 corners each; per corner the source point (FBX control point / glTF vertex)
        std::vector<uint32_t> point;
        std::vector<double> positions;          // 3 per point, the mesh's own space, raw
        std::vector<std::string> uvNames;       // FBX set names, glTF "TEXCOORD_n"
        std::vector<std::vector<double>> uvs;   // per set: 2 per corner, raw (V not flipped)
        std::vector<float> normals;             // 3 per corner or empty
        std::vector<float> tangents;            // 4 per corner (w = handedness) or empty
        std::vector<float> colors;              // 4 per corner or empty
        std::vector<int> triangleMaterial;      // per triangle, -1 = none
        std::vector<VatMaterialData> materials;
        MeshXform xform;                        // the mesh node's transform (to be baked)
        bool gltf = false;                      // UV origin top-left (glTF) or bottom-left (FBX)
        bool embeddedTextures = false;          // textures that only exist inside the file
        // the raw bounding box of every point (Houdini's guard vertices sit at its corners)
        D3 boundsMin, boundsMax;

        size_t corners() const { return point.size(); }
        size_t triangles() const { return point.size() / 3; }
        D3 position(size_t corner) const {
            const size_t p = point[corner] * 3;
            return D3(positions[p], positions[p + 1], positions[p + 2]);
        }
        int findUv(const std::string& name) const;  // case-insensitive, -1 when absent
    };

    // FBX (ufbx) or glTF/GLB (tinygltf) by extension; false with a sentence in `error`
    bool readSourceMesh(const std::string& path, SourceMesh& out, std::string& error);

    // ---- our mesh from the source's corners ----

    struct BuiltMesh{
        VatMeshData mesh;
        std::vector<size_t> sourceCorner;   // per mesh vertex: one source corner
    };
    // one vertex per distinct (point, element, material UV, normal, tangent, colour) of the kept
    // corners; `element` per corner becomes vatIndex (-1 = the corner's triangle is dropped);
    // material UVs (set `materialUv`, -1 = none) converted to glTF's V (FBX: 1 - v); one submesh
    // per material
    void buildMesh(const SourceMesh& src, const std::vector<int>& element, int materialUv, bool withNormals,
                   bool withColors, BuiltMesh& out, bool twoSided);
    // every submesh double-sided (a default "VAT" material when the mesh has none)
    void makeDoubleSided(VatMeshData& mesh);
    // a corner's rest tangent / normal in the mesh's own space ((1,0,0) / (0,1,0) when absent)
    D3 cornerTangent(const SourceMesh& src, size_t corner);
    D3 cornerNormal(const SourceMesh& src, size_t corner);

    // ---- presets (spec 2.4) ----

    struct PresetLimits{
        int width = 2048;
        uint64_t maxTexels = 2048ull * 2048ull;
        uint64_t memoryWarningBytes = 32ull * 1024ull * 1024ull;
    };
    bool presetLimits(const std::string& name, PresetLimits& out);

    // ---- the payload being built (float, the 2.2 layout) ----

    struct PayloadBuilder{
        jasperengine::vat::VatMode mode = jasperengine::vat::VatMode::Vertices;
        jasperengine::vat::VatLayout layout;    // vertices / bones / samples / normals / colors / width
        std::vector<float> texels;              // width * height * 4

        // sizes the texel array; false when it would not fit in memory
        bool allocate();
        uint64_t texelCount() const;
        float* at(int frame, int element, int k){
            return texels.data() + jasperengine::vat::vatTexelIndex(mode, layout, frame, element, k) * 4;
        }
        const float* at(int frame, int element, int k) const{
            return texels.data() + jasperengine::vat::vatTexelIndex(mode, layout, frame, element, k) * 4;
        }
        void set(int frame, int element, int k, double x, double y, double z, double w){
            float* t = at(frame, element, k);
            t[0] = (float)x; t[1] = (float)y; t[2] = (float)z; t[3] = (float)w;
        }
    };

    // What an importer hands to finishImport
    struct Assembled{
        PayloadBuilder payload;
        std::vector<jasperengine::vat::VatClip> clips;  // name, start, frames, fps, wrap, interpolate, restNone
        VatMeshData mesh;                       // rest positions etc. in the mesh's own space; vatIndex set
        MeshXform xform;                        // baked into the mesh and the payload by finishImport
        std::vector<D3> elementRest;            // Vertices: per payload vertex, its rest position (own space)
        std::string modeReason;
        float sourceFps = 0.0f;
        int sourceFrames = 0;
        int upsample = 1;
        float interpolationErrorMm = 0.0f;
    };

    // the texels a payload needs, and too_large (with fixes) when over the preset
    uint64_t payloadTexels(jasperengine::vat::VatMode mode, int samples, int elements, int perSample);
    bool checkPayloadSize(jasperengine::vat::VatMode mode, int samples, int elements, int perSample, const PresetLimits& limits,
                          const std::string& what, const std::vector<std::string>& fixes, VatImportResult& result);

    // bakes the node transform, enforces quaternion hemisphere continuity, picks the precision
    // (half when its error at every frame stays within the tolerance, unless forced), computes the
    // clip and asset bounds, fills result.data (mode, layout, clips, bounds, recipe left to the
    // caller), result.mesh, result.payload and the report's sizes
    bool finishImport(Assembled& in, const VatImportSpec& spec, VatImportResult& result);

    // ---- issues ----

    void addError(VatImportResult& result, const std::string& kind, const std::string& message, const std::string& fix);
    void addWarning(VatImportResult& result, const std::string& kind, const std::string& message, const std::string& fix);
    // a number for messages: shortest text that reads back ("0.1", "24", "1e-05")
    std::string num(double value);

    // ---- files ----

    std::string fileName(const std::string& path);
    bool fileExists(const std::string& path);
    bool readText(const std::string& path, std::string& out);
    // "sha1:<hex>" of a file's bytes ("" when unreadable)
    std::string fileSha1(const std::string& path);
    std::string lower(std::string text);

    // ---- the two importers ----

    void detectHoudini(const std::string& path, VatImportDetection& out);
    void detectOpenVat(const std::string& path, VatImportDetection& out);
    void importHoudini(const VatImportSpec& spec, const VatImportDetection& detection, VatImportResult& result);
    void importOpenVat(const VatImportSpec& spec, const VatImportDetection& detection, VatImportResult& result);

}

#endif //VAT_IMPORT_INTERNAL_H
