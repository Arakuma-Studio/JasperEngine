// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Synthetic Houdini Labs VAT 3 exports for the importer tests (vertex animation spec 4.6.1),
// shared by VatImportTest (decoded against the ground truth) and VatEndToEndTest (imported for
// the GPU render checks of engine/tests/htmlui/TestVatEndToEnd.cpp): writers of every file an
// export holds, from KNOWN ground truth with the exporter's documented encodings (ASCII FBX
// meshes with every UV set, normals, tangents and binormals; EXR textures through FloatImage's
// writeExr; 8-bit PNGs through stb; Unity .mat YAML; the legacy data JSON), the soft-body fixture
// (writeSoft: a waving 3 x 3 grid) and the rigid-body one (writeRigid: tetrahedra spinning about
// their pivots), plus helpers that read an import's payload back (texel, animated). The math is
// the tests' own, in double precision, independent of the importer's.

#ifndef VAT_HOUDINI_FIXTURES_H
#define VAT_HOUDINI_FIXTURES_H

#include <gtest/gtest.h>

#include "vat/import/VatImport.h"

#include "util/FloatImage.h"

#include "json.hpp"
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace vathoudini {

    namespace fs = std::filesystem;
    namespace ev = jasperengine::editor::vat;
    namespace vat = jasperengine::vat;
    using Json = nlohmann::json;

    constexpr double PI = 3.14159265358979323846;

    // ---- small math (independent of the importer's) ----

    struct V3 {
        double x = 0, y = 0, z = 0;
        V3() = default;
        V3(double a, double b, double c) : x(a), y(b), z(c) {}
        V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
        V3 operator*(double s) const { return {x * s, y * s, z * s}; }
    };
    inline double dot3(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline V3 cross3(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    inline double len3(const V3& a) { return std::sqrt(dot3(a, a)); }
    inline V3 unit(const V3& a) { return a * (1.0 / len3(a)); }

    struct Q4 {
        double x = 0, y = 0, z = 0, w = 1;
    };
    inline Q4 axisAngle(const V3& axis, double radians) {
        const V3 a = unit(axis);
        const double s = std::sin(radians / 2);
        return {a.x * s, a.y * s, a.z * s, std::cos(radians / 2)};
    }
    inline V3 rot(const Q4& q, const V3& v) {
        const V3 u(q.x, q.y, q.z);
        const V3 c = cross3(u, v) + v * q.w;
        return v + cross3(u, c) * 2.0;
    }
    inline Q4 mul(const Q4& a, const Q4& b) {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }
    // the rotation with matrix columns X, Y, Z
    inline Q4 fromColumns(const V3& X, const V3& Y, const V3& Z) {
        const double tr = X.x + Y.y + Z.z;
        Q4 q;
        if (tr > 0) {
            const double s = std::sqrt(tr + 1.0) * 2;
            q = {(Y.z - Z.y) / s, (Z.x - X.z) / s, (X.y - Y.x) / s, 0.25 * s};
        } else if (X.x > Y.y && X.x > Z.z) {
            const double s = std::sqrt(1.0 + X.x - Y.y - Z.z) * 2;
            q = {0.25 * s, (Y.x + X.y) / s, (Z.x + X.z) / s, (Y.z - Z.y) / s};
        } else if (Y.y > Z.z) {
            const double s = std::sqrt(1.0 + Y.y - X.x - Z.z) * 2;
            q = {(Y.x + X.y) / s, 0.25 * s, (Z.y + Y.z) / s, (Z.x - X.z) / s};
        } else {
            const double s = std::sqrt(1.0 + Z.z - X.x - Y.y) * 2;
            q = {(Z.x + X.z) / s, (Z.y + Y.z) / s, 0.25 * s, (X.y - Y.x) / s};
        }
        const double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return {q.x / l, q.y / l, q.z / l, q.w / l};
    }

    inline float half2float(uint16_t h) {
        const int e = (h >> 10) & 31, m = h & 1023;
        const double sign = (h & 0x8000) ? -1.0 : 1.0;
        if (e == 0) return (float)(sign * m * std::ldexp(1.0, -24));
        if (e == 31) return (float)(m ? NAN : sign * INFINITY);
        return (float)(sign * (1024 + m) * std::ldexp(1.0, e - 25));
    }

    // a payload texel through vatTexelIndex
    inline std::array<double, 4> texel(const ev::VatImportResult& r, int frame, int element, int k) {
        const uint64_t t = vat::vatTexelIndex(r.data.mode, r.data.layout, frame, element, k);
        std::array<double, 4> out{};
        for (int c = 0; c < 4; c++) {
            if (r.payload.precision == vat::VatPrecision::Float) {
                float f;
                std::memcpy(&f, r.payload.bytes.data() + (t * 4 + c) * 4, 4);
                out[c] = f;
            } else {
                uint16_t h;
                std::memcpy(&h, r.payload.bytes.data() + (t * 4 + c) * 2, 2);
                out[c] = half2float(h);
            }
        }
        return out;
    }
    inline Q4 texelQuat(const ev::VatImportResult& r, int frame, int element, int k) {
        const auto t = texel(r, frame, element, k);
        return {t[0], t[1], t[2], t[3]};
    }

    inline V3 meshPosition(const ev::VatMeshData& m, size_t v) { return {m.positions[v * 3], m.positions[v * 3 + 1], m.positions[v * 3 + 2]}; }

    // a mesh vertex's position at payload frame `frame` (Vertices: rest + offset or absolute; Bones: skinned)
    inline V3 animated(const ev::VatImportResult& r, size_t v, int frame) {
        const ev::VatMeshData& m = r.mesh;
        if (r.data.mode == vat::VatMode::Bones) {
            const int bone = m.joints[v * 4];
            const auto q = texelQuat(r, frame, bone, 0);
            const auto t = texel(r, frame, bone, 1);
            return rot(q, meshPosition(m, v) * t[3]) + V3(t[0], t[1], t[2]);
        }
        const int e = (int)m.vatIndex[v];
        const auto o = texel(r, frame, e, 0);
        bool absolute = false;
        for (const vat::VatClip& c : r.data.clips)
            if (frame >= c.start && frame < c.start + c.frames) absolute = c.restNone;
        return (absolute ? V3() : meshPosition(m, v)) + V3(o[0], o[1], o[2]);
    }

    inline std::string issues(const ev::VatImportResult& r) { return ev::vatImportIssuesText(r.errors); }

    inline bool hasWarning(const ev::VatImportResult& r, const std::string& kind) {
        for (const ev::VatImportIssue& w : r.report.warnings)
            if (w.kind == kind) return true;
        return false;
    }
    inline bool hasError(const ev::VatImportResult& r, const std::string& kind) {
        for (const ev::VatImportIssue& e : r.errors)
            if (e.kind == kind) return true;
        return false;
    }

    // ---- fixture writers ----

    inline fs::path freshDir(const std::string& name) {
        const fs::path dir = fs::temp_directory_path() / "jasperengine-vat-import" / name;
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        return dir;
    }

    inline void writeText(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << text;
    }

    inline std::string g17(double v) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.17g", v);
        return b;
    }
    // Unity writes float32 values with their shortest text
    inline std::string unityFloat(double v) {
        const float f = (float)v;
        char b[64];
        for (int digits = 1; digits <= 9; digits++) {
            std::snprintf(b, sizeof(b), "%.*g", digits, (double)f);
            if ((float)std::strtod(b, nullptr) == f) break;
        }
        return b;
    }

    struct FbxMesh {
        std::vector<V3> points;
        std::vector<int> corners;                       // 3 per triangle
        std::vector<std::string> uvNames;
        std::vector<std::vector<std::array<double, 2>>> uvs;  // per set, per corner
        std::vector<V3> normals, tangents, binormals;   // per corner (or empty)
        // a node transform (translation, Euler XYZ degrees, uniform scale)
        V3 translation, rotationDeg;
        double scale = 1.0;
    };

    // a minimal ASCII FBX 7.4 (what ufbx reads; Houdini's exporter writes binary FBX with the same layers)
    inline void writeFbx(const fs::path& path, const FbxMesh& mesh) {
        std::ostringstream o;
        o << "; FBX 7.4.0 project file\nFBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n\tCreator: \"JasperEngine VatImportTest\"\n}\n";
        o << "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n"
             "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
             "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
             "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
             "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n\t}\n}\n";
        o << "Objects:  {\n\tGeometry: 1000, \"Geometry::MESH\", \"Mesh\" {\n";
        o << "\t\tVertices: *" << mesh.points.size() * 3 << " {\n\t\t\ta: ";
        for (size_t i = 0; i < mesh.points.size(); i++) {
            if (i) o << ",";
            o << g17(mesh.points[i].x) << "," << g17(mesh.points[i].y) << "," << g17(mesh.points[i].z);
        }
        o << "\n\t\t}\n\t\tPolygonVertexIndex: *" << mesh.corners.size() << " {\n\t\t\ta: ";
        for (size_t i = 0; i < mesh.corners.size(); i++) {
            if (i) o << ",";
            o << ((i % 3 == 2) ? -mesh.corners[i] - 1 : mesh.corners[i]);
        }
        o << "\n\t\t}\n\t\tGeometryVersion: 124\n";
        auto vectors = [&](const char* element, const char* name, const std::vector<V3>& values) {
            if (values.empty()) return;
            o << "\t\t" << element << ": 0 {\n\t\t\tVersion: 102\n\t\t\tName: \"\"\n\t\t\tMappingInformationType: \"ByPolygonVertex\"\n"
                 "\t\t\tReferenceInformationType: \"Direct\"\n\t\t\t" << name << ": *" << values.size() * 3 << " {\n\t\t\t\ta: ";
            for (size_t i = 0; i < values.size(); i++) {
                if (i) o << ",";
                o << g17(values[i].x) << "," << g17(values[i].y) << "," << g17(values[i].z);
            }
            o << "\n\t\t\t}\n\t\t}\n";
        };
        vectors("LayerElementNormal", "Normals", mesh.normals);
        vectors("LayerElementTangent", "Tangents", mesh.tangents);
        vectors("LayerElementBinormal", "Binormals", mesh.binormals);
        for (size_t s = 0; s < mesh.uvs.size(); s++) {
            o << "\t\tLayerElementUV: " << s << " {\n\t\t\tVersion: 101\n\t\t\tName: \"" << mesh.uvNames[s]
              << "\"\n\t\t\tMappingInformationType: \"ByPolygonVertex\"\n\t\t\tReferenceInformationType: \"Direct\"\n\t\t\tUV: *"
              << mesh.uvs[s].size() * 2 << " {\n\t\t\t\ta: ";
            for (size_t i = 0; i < mesh.uvs[s].size(); i++) {
                if (i) o << ",";
                o << g17(mesh.uvs[s][i][0]) << "," << g17(mesh.uvs[s][i][1]);
            }
            o << "\n\t\t\t}\n\t\t}\n";
        }
        o << "\t\tLayer: 0 {\n\t\t\tVersion: 100\n";
        if (!mesh.normals.empty()) o << "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementNormal\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
        if (!mesh.tangents.empty()) o << "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementTangent\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
        if (!mesh.binormals.empty()) o << "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementBinormal\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
        if (!mesh.uvs.empty()) o << "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementUV\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
        o << "\t\t}\n";
        for (size_t s = 1; s < mesh.uvs.size(); s++) {
            o << "\t\tLayer: " << s << " {\n\t\t\tVersion: 100\n\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementUV\"\n\t\t\t\tTypedIndex: " << s
              << "\n\t\t\t}\n\t\t}\n";
        }
        o << "\t}\n\tModel: 2000, \"Model::MESH\", \"Mesh\" {\n\t\tVersion: 232\n\t\tProperties70:  {\n";
        o << "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\"," << g17(mesh.translation.x) << "," << g17(mesh.translation.y) << ","
          << g17(mesh.translation.z) << "\n";
        o << "\t\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\"," << g17(mesh.rotationDeg.x) << "," << g17(mesh.rotationDeg.y) << ","
          << g17(mesh.rotationDeg.z) << "\n";
        o << "\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\"," << g17(mesh.scale) << "," << g17(mesh.scale) << "," << g17(mesh.scale) << "\n";
        o << "\t\t}\n\t}\n}\nConnections:  {\n\tC: \"OO\",2000,0\n\tC: \"OO\",1000,2000\n}\n";
        writeText(path, o.str());
    }

    struct Image {
        int width = 0, height = 0;
        std::vector<double> rgba;   // top row first
        Image() = default;
        Image(int w, int h) : width(w), height(h), rgba((size_t)w * h * 4, 0.0) {}
        double* at(int col, int row) { return &rgba[((size_t)row * width + col) * 4]; }
        void set(int col, int row, double r, double g, double b, double a) {
            double* t = at(col, row);
            t[0] = r; t[1] = g; t[2] = b; t[3] = a;
        }
    };

    inline void writeExrImage(const fs::path& path, const Image& image, int channels = 4) {
        jasperengine::editor::FloatImage out;
        out.width = image.width;
        out.height = image.height;
        out.sourceChannels = channels;
        out.rgba.resize(image.rgba.size());
        for (size_t i = 0; i < image.rgba.size(); i++) out.rgba[i] = (float)image.rgba[i];
        fs::create_directories(path.parent_path());
        std::string error;
        ASSERT_TRUE(jasperengine::editor::writeExr(path.string(), out, false, &error)) << error;
    }

    // 8-bit PNG of values in 0..1 (rounded) or of raw bytes
    inline void writePng8(const fs::path& path, const Image& image, bool rawBytes = false) {
        std::vector<uint8_t> bytes(image.rgba.size());
        for (size_t i = 0; i < bytes.size(); i++) {
            const double v = rawBytes ? image.rgba[i] : std::round(std::clamp(image.rgba[i], 0.0, 1.0) * 255.0);
            bytes[i] = (uint8_t)std::clamp(v, 0.0, 255.0);
        }
        fs::create_directories(path.parent_path());
        ASSERT_TRUE(stbi_write_png(path.string().c_str(), image.width, image.height, 4, bytes.data(), image.width * 4) != 0);
    }
    inline double q8(double v) { return std::round(std::clamp(v, 0.0, 1.0) * 255.0) / 255.0; }

    // ---- Houdini bounds with flags (spec 4.6.1) ----

    struct HBounds {
        double mn[3], mx[3];
    };

    // target-space bounds covering [lo, hi] with the flag digits: HDR, active ratios, position
    // alpha scale, the min-X digit (rigid LDR rotation range / fluid lookup depth)
    inline HBounds flaggedBounds(V3 lo, V3 hi, bool hdr, double ur, double vr, double alphaScale, double minXDigit) {
        HBounds b;
        b.mx[0] = (std::ceil(hi.x * 10) + 1 - (1.0 - vr)) / 10.0;
        b.mx[1] = (std::ceil(hi.y * 10) + 1 + (1.0 - 1.0 / alphaScale)) / 10.0;
        b.mx[2] = (std::ceil(hi.z * 10) + 1 + (hdr ? 0.7 : 0.2)) / 10.0;
        b.mn[0] = -(std::ceil(-lo.x * 10) + 1 + minXDigit) / 10.0;
        b.mn[1] = (std::floor(lo.y * 10) - 1) / 10.0;
        b.mn[2] = (std::floor(lo.z * 10) - 1 - (ur < 1.0 ? 1.0 - ur : 0.0)) / 10.0;
        return b;
    }

    // the target's axes: target = F W houdini * scale
    struct Target {
        std::string name = "unity";
        V3 point(const V3& h) const {
            if (name == "unity") return {-h.x, h.y, h.z};
            if (name == "unreal") return V3(h.x, h.z, h.y) * 100.0;
            return h;
        }
        V3 back(const V3& t) const {
            if (name == "unity") return {-t.x, t.y, t.z};
            if (name == "unreal") return V3(t.x, t.z, t.y) * 0.01;
            return t;
        }
        // rotations: the axis is a pseudovector (det = -1 for both mirrors)
        Q4 quat(const Q4& q) const {
            if (name == "unity") return {q.x, -q.y, -q.z, q.w};
            if (name == "unreal") return {-q.x, -q.z, -q.y, q.w};
            return q;
        }
        // pivots, compressed normals, accelerations: W space (no position invert)
        V3 wspace(const V3& h) const {
            if (name == "unreal") return V3(h.x, h.z, h.y);
            return h;
        }
        double scale() const { return name == "unreal" ? 100.0 : 1.0; }
    };

    // the Unity .mat the ROP writes (shader GUID per mode, m_Floats, keywords)
    inline std::string unityMaterial(const std::string& guid, const HBounds& b, int frames, double fps, const std::map<std::string, double>& flags,
                              const std::string& keywords) {
        std::ostringstream o;
        o << "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n--- !u!21 &2100000\nMaterial:\n  serializedVersion: 6\n  m_Name: test_mat\n"
             "  m_Shader: {fileID: -6465566751694194690, guid: " << guid << ",\n    type: 3}\n  m_ShaderKeywords: " << keywords << "\n"
             "  m_SavedProperties:\n    serializedVersion: 3\n    m_TexEnvs:\n    - _posTexture:\n        m_Texture: {fileID: 2800000, guid: 0123, type: 3}\n"
             "        m_Scale: {x: 1, y: 1}\n    m_Ints: []\n    m_Floats:\n";
        o << "    - _AlphaClip: 0\n";
        for (const auto& f : flags) o << "    - " << f.first << ": " << unityFloat(f.second) << "\n";
        const char* names[6] = {"_boundMaxX", "_boundMaxY", "_boundMaxZ", "_boundMinX", "_boundMinY", "_boundMinZ"};
        const double values[6] = {b.mx[0], b.mx[1], b.mx[2], b.mn[0], b.mn[1], b.mn[2]};
        for (int i = 0; i < 6; i++) o << "    - " << names[i] << ": " << unityFloat(values[i]) << "\n";
        o << "    - _frameCount: " << frames << "\n    - _houdiniFPS: " << unityFloat(fps) << "\n";
        o << "    m_Colors:\n    - _BaseColor: {r: 1, g: 1, b: 1, a: 1}\n  m_BuildTextureStacks: []\n";
        return o.str();
    }

    inline constexpr const char* SOFT_GUID = "987a09a96b5f17d46a88a7c429d130d2";
    inline constexpr const char* RIGID_GUID = "085ef3ec3a5a70d4790c0957d30c9f63";
    inline constexpr const char* FLUID_GUID = "19b419010e1004644affea18d0fc8dff";
    inline constexpr const char* SPRITE_GUID = "8bf13a6837d559049924774e12033384";

    // ---- the soft-body fixture ----

    struct SoftOptions {
        std::string metadata = "material";  // "material" | "json" | "bounds"
        std::string target = "unity";
        bool hdr = true;
        bool twoPositions = false;
        bool compressed = false;            // normals in position alpha, no rotation texture
        bool colors = true;
        double ur = 1.0, vr = 1.0;
        bool float32Trap = false;           // bounds as float32 values whose tenths aren't whole
        int frames = 6;
        bool nodeTransform = false;     // the mesh node: translated, turned 90 degrees about Y, scaled 2
        double fps = 24.0;
    };

    struct SoftTruth {
        std::vector<V3> rest;                       // Houdini space
        std::vector<std::vector<V3>> pos, nrm, tan; // [frame][point]
        std::vector<std::vector<std::array<double, 4>>> col;
        HBounds bounds;
    };

    // decodes a compressed (spheremap) normal code the way the Unity graph does, in Houdini's axes
    inline V3 decodeSpheremap(double a) {
        const double x = a * 1024.0;
        const double hi = std::floor(x / 32.0);
        const double ex = hi / 31.5 * 4.0 - 2.0, ey = (x - hi * 32.0) / 31.5 * 4.0 - 2.0;
        const double e2 = ex * ex + ey * ey;
        const double g = std::sqrt(std::max(0.0, 1.0 - e2 / 4.0));
        return unit(V3(std::clamp(ex * g, -1.0, 1.0), std::clamp(1.0 - e2 / 2.0, -1.0, 1.0), std::clamp(ey * g, -1.0, 1.0)));
    }
    inline double encodeSpheremap(const V3& n) {
        const double s = 2.0 * (1.0 - n.y);
        const double g = std::sqrt(std::max(1e-12, 1.0 - s / 4.0));
        const double ex = n.x / g, ey = n.z / g;
        const double hi = std::clamp(std::round((ex + 2.0) / 4.0 * 31.5), 0.0, 31.0);
        const double lo = std::clamp(std::round((ey + 2.0) / 4.0 * 31.5), 0.0, 31.0);
        return (hi * 32.0 + lo) / 1024.0;
    }

    // a 3 x 3 grid of points (8 triangles) waving and turning, plus the guard triangle
    inline SoftTruth writeSoft(const fs::path& root, const std::string& asset, const SoftOptions& opt) {
        SoftTruth truth;
        const Target target{opt.target};
        const int P = 9, N = opt.frames;
        for (int i = 0; i < P; i++) truth.rest.push_back(V3((i % 3) * 0.5 - 0.5, 1.0 + 0.1 * (i / 3), (i / 3) * 0.5 - 0.5));
        truth.pos.assign(N, {});
        truth.nrm.assign(N, {});
        truth.tan.assign(N, {});
        truth.col.assign(N, {});
        for (int f = 0; f < N; f++) {
            for (int i = 0; i < P; i++) {
                const V3 off = f == 0 ? V3() : V3(0.3 * std::sin(f * 0.7 + i), 0.2 * f, -0.1 * std::cos(f + 2.0 * i) + 0.1 * std::cos(2.0 * i));
                truth.pos[f].push_back(truth.rest[i] + off);
                const Q4 r = axisAngle(V3(1, 0.5, 0.2), 0.4 * f + 0.1 * i);
                truth.nrm[f].push_back(rot(r, V3(0, 1, 0)));
                truth.tan[f].push_back(rot(r, V3(1, 0, 0)));
                truth.col[f].push_back({0.1 * i, 0.05 * f, 0.5, 1.0 - 0.01 * i});
            }
        }
        // target-space extent of the stored offsets and the rest mesh
        V3 lo(1e9, 1e9, 1e9), hi(-1e9, -1e9, -1e9);
        auto grow = [&](const V3& v) {
            lo = V3(std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z));
            hi = V3(std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z));
        };
        for (int f = 0; f < N; f++)
            for (int i = 0; i < P; i++) grow(target.point(truth.pos[f][i] - truth.rest[i]));
        for (int i = 0; i < P; i++) grow(target.point(truth.rest[i]));
        HBounds b = flaggedBounds(lo, hi, opt.hdr, opt.ur, opt.vr, 1.0, 0.0);
        if (opt.float32Trap) {
            // 8.1 as float32 (8.10000038): its tenths are 81.0000038
            b.mx[0] = (double)(float)(std::ceil(hi.x) + 8.1);
            for (int k = 0; k < 3; k++) {
                b.mn[k] = (double)(float)b.mn[k];
                b.mx[k] = (double)(float)b.mx[k];
            }
        }
        truth.bounds = b;
        const double range[3] = {b.mx[0] - b.mn[0], b.mx[1] - b.mn[1], b.mx[2] - b.mn[2]};

        // texture layout: A active columns, R rows per frame
        const int A = 4, R = (P + A - 1) / A;
        const int W = (int)std::lround(A / opt.ur);
        const int activeRows = R * N;
        const int H = (int)std::lround(activeRows / opt.vr);
        Image pos(W, H), pos2(W, H), rotTex(W, H), col(W, H);
        for (int f = 0; f < N; f++) {
            for (int i = 0; i < P; i++) {
                const int c = i % A, r = i / A + f * R;
                const V3 o = target.point(truth.pos[f][i] - truth.rest[i]);
                double stored[3] = {o.x, o.y, o.z};
                double alpha = 1.0;
                if (opt.compressed) {
                    alpha = encodeSpheremap(target.wspace(truth.nrm[f][i]));
                    truth.nrm[f][i] = decodeSpheremap(alpha);   // what the code can hold
                    if (opt.target == "unreal") truth.nrm[f][i] = unit(V3(truth.nrm[f][i].x, truth.nrm[f][i].z, truth.nrm[f][i].y));
                }
                if (!opt.hdr) {
                    // LDR: normalised to the bounds; split: coarse + fine / 100
                    const double mnv[3] = {b.mn[0], b.mn[1], b.mn[2]};
                    double norm[3], coarse[3], fine[3];
                    for (int k = 0; k < 3; k++) {
                        norm[k] = (stored[k] - mnv[k]) / range[k];
                        if (opt.twoPositions) {
                            coarse[k] = std::floor(norm[k] * 255.0) / 255.0;
                            fine[k] = q8((norm[k] - coarse[k]) * 100.0);
                            norm[k] = coarse[k] + fine[k] * 0.01;
                        } else {
                            coarse[k] = q8(norm[k]);
                            fine[k] = 0;
                            norm[k] = coarse[k];
                        }
                        stored[k] = norm[k] * range[k] + mnv[k];
                    }
                    pos.set(c, r, coarse[0], coarse[1], coarse[2], alpha);
                    if (opt.twoPositions) pos2.set(c, r, fine[0], fine[1], fine[2], 1.0);
                } else if (opt.twoPositions) {
                    double coarse[3], fine[3];
                    for (int k = 0; k < 3; k++) {
                        coarse[k] = std::round(stored[k] * 16.0) / 16.0;
                        fine[k] = (stored[k] - coarse[k]) * 100.0;
                    }
                    pos.set(c, r, coarse[0], coarse[1], coarse[2], alpha);
                    pos2.set(c, r, fine[0], fine[1], fine[2], 1.0);
                } else {
                    pos.set(c, r, stored[0], stored[1], stored[2], alpha);
                }
                // the decoded truth in Houdini space
                truth.pos[f][i] = truth.rest[i] + target.back(V3(stored[0], stored[1], stored[2]));

                if (!opt.compressed) {
                    // q maps (1,0,0) -> tangent, (0,1,0) -> normal in Houdini space; target-space q stored
                    const V3 n = truth.nrm[f][i], t = truth.tan[f][i];
                    Q4 qt = target.quat(fromColumns(t, n, cross3(t, n)));
                    double qv[4] = {qt.x, qt.y, qt.z, qt.w};
                    if (!opt.hdr) {
                        for (double& v : qv) v = q8((v + 1.0) / 2.0);
                        rotTex.set(c, r, qv[0], qv[1], qv[2], qv[3]);
                        for (double& v : qv) v = (v - 0.5) * 2.0;
                    } else {
                        rotTex.set(c, r, qv[0], qv[1], qv[2], qv[3]);
                    }
                    // what the shader shows: N = rotate(q, (0,1,0)), T = rotate(q, (-1,0,0)) (Unity), normalised
                    const Q4 q{qv[0], qv[1], qv[2], qv[3]};
                    const V3 nt = unit(rot(q, target.point(V3(0, 1, 0)) * (1.0 / target.scale())));
                    const V3 tt = unit(rot(q, target.point(V3(1, 0, 0)) * (1.0 / target.scale())));
                    truth.nrm[f][i] = unit(target.back(nt) * target.scale());
                    truth.tan[f][i] = unit(target.back(tt) * target.scale());
                }
                auto& cc = truth.col[f][i];
                if (!opt.hdr) for (double& v : cc) v = q8(v);
                col.set(c, r, cc[0], cc[1], cc[2], cc[3]);
            }
        }
        const std::string ext = opt.hdr ? ".exr" : ".png";
        auto save = [&](const std::string& kind, const Image& image) {
            const fs::path path = root / "tex" / (asset + "_" + kind + ext);
            if (opt.hdr) writeExrImage(path, image);
            else writePng8(path, image);
        };
        save("pos", pos);
        if (opt.twoPositions) save("pos2", pos2);
        if (!opt.compressed) save("rot", rotTex);
        if (opt.colors) save("col", col);

        // the mesh: the grid (rest, Houdini space) and the guard triangle at the bound corners
        FbxMesh mesh;
        mesh.points = truth.rest;
        const V3 tmin = V3(b.mn[0], b.mn[1], b.mn[2]), tmax = V3(b.mx[0], b.mx[1], b.mx[2]);
        V3 g0 = target.back(tmin), g1 = target.back(tmax);
        if (opt.float32Trap) {
            g0 = V3((float)g0.x, (float)g0.y, (float)g0.z);
            g1 = V3((float)g1.x, (float)g1.y, (float)g1.z);
        }
        mesh.points.push_back(g0);
        mesh.points.push_back(g1);
        mesh.points.push_back(V3(g0.x, g1.y, g0.z));
        mesh.uvNames = {"uv", "uv2"};
        mesh.uvs.assign(2, {});
        const int tris[8][3] = {{0, 1, 4}, {0, 4, 3}, {1, 2, 5}, {1, 5, 4}, {3, 4, 7}, {3, 7, 6}, {4, 5, 8}, {4, 8, 7}};
        for (const auto& t : tris) {
            for (int k = 0; k < 3; k++) {
                const int i = t[k];
                mesh.corners.push_back(i);
                mesh.uvs[0].push_back({(i % 3) / 2.0, (i / 3) / 2.0});
                mesh.uvs[1].push_back({((i % A) + 0.5) / A, 1.0 - ((i / A) + 0.5) / activeRows});
                mesh.normals.push_back(truth.nrm[0][i]);
                mesh.tangents.push_back(truth.tan[0][i]);
                mesh.binormals.push_back(cross3(truth.nrm[0][i], truth.tan[0][i]));
            }
        }
        for (int k = 0; k < 3; k++) {
            mesh.corners.push_back(P + k);
            mesh.uvs[0].push_back({0, 0});
            mesh.uvs[1].push_back({0.0, 0.0});
            mesh.normals.push_back(V3(0, 1, 0));
            mesh.tangents.push_back(V3(1, 0, 0));
            mesh.binormals.push_back(V3(0, 0, -1));
        }
        if (opt.nodeTransform) {
            mesh.translation = V3(1, 2, 3);
            mesh.rotationDeg = V3(0, 90, 0);
            mesh.scale = 2.0;
        }
        writeFbx(root / "geo" / (asset + "_mesh.fbx"), mesh);

        if (opt.metadata == "material") {
            std::map<std::string, double> flags = {{"_B_LOAD_COL_TEX", opt.colors ? 1 : 0}, {"_B_LOAD_POS_TWO_TEX", opt.twoPositions ? 1 : 0},
                                                   {"_B_UNLOAD_ROT_TEX", opt.compressed ? 1 : 0}, {"_B_twoSidedNorms", 1}, {"_B_interpolate", 0}};
            std::string keywords = opt.colors ? "_B_LOAD_COL_TEX" : "";
            if (opt.twoPositions) keywords += " _B_LOAD_POS_TWO_TEX";
            std::string text = unityMaterial(SOFT_GUID, b, N, opt.fps, flags, keywords);
            if (opt.float32Trap) {
                // the float32 value with all its digits, as a hand-edited or re-saved material may hold
                const std::string shortest = "_boundMaxX: " + unityFloat(b.mx[0]);
                text.replace(text.find(shortest), shortest.size(), "_boundMaxX: " + g17(b.mx[0]));
            }
            writeText(root / "unity" / (asset + "_mat.mat"), text);
        } else if (opt.metadata == "json") {
            Json j = {{"Bound Min X", b.mn[0]}, {"Bound Min Y", b.mn[1]}, {"Bound Min Z", b.mn[2]}, {"Bound Max X", b.mx[0]},
                      {"Bound Max Y", b.mx[1]}, {"Bound Max Z", b.mx[2]}, {"Houdini FPS", opt.fps}, {"Two Position Textures", opt.twoPositions},
                      {"Axis System", opt.target == "unreal" ? "Z-X-Y Clockwise (Left-Handed Z-Up)" : "Y-Z-X Clockwise (Left-Handed Y-Up)"},
                      {"Frame Count", N}};
            writeText(root / "data" / (asset + "_data.json"), Json::array({j}).dump(2));
        }
        return truth;
    }


    struct RigidPiece {
        V3 pivot;
        V3 axis;
        double degreesPerFrame = 0;
        V3 velocity;
        double scale = 1.0;
        V3 acceleration;    // smooth trajectories
    };

    struct RigidTruth {
        std::vector<RigidPiece> pieces;
        std::vector<V3> rest;           // mesh points (Houdini space)
        std::vector<int> pointPiece;
        int frames = 0;
        double fps = 24;
        bool smooth = false;

        double timeOf(double frame) const { return frame / fps; }
        V3 position(int b, double frame) const {
            const RigidPiece& p = pieces[b];
            const double t = timeOf(frame);
            return p.pivot + p.velocity * t + p.acceleration * (0.5 * t * t);
        }
        Q4 rotation(int b, double frame) const { return axisAngle(pieces[b].axis, pieces[b].degreesPerFrame * frame * PI / 180.0); }
        // where Houdini's shader puts a rest point at `frame` (exact motion between frames)
        V3 point(int i, double frame) const {
            const int b = pointPiece[i];
            return position(b, frame) + rot(rotation(b, frame), (rest[i] - pieces[b].pivot) * pieces[b].scale);
        }
    };

    // smallest-three encoding as the ROP writes it: rgb = three components, index = the dropped one
    // (0 w, 1 x, 2 y, 3 z) with its sign made what the decoder rebuilds; returns the stored sign
    inline double encodeRotation(const Q4& q, double rgb[3], int& index) {
        const double c[4] = {std::fabs(q.w), std::fabs(q.x), std::fabs(q.y), std::fabs(q.z)};
        index = 0;
        for (int i = 1; i < 4; i++)
            if (c[i] > c[index] + 1e-9) index = i;
        Q4 s = q;
        double sign = 1.0;
        // the decoder rebuilds the dropped component as +sqrt (index 0, 1) or -sqrt (index 2, 3)
        const double dropped = index == 0 ? q.w : index == 1 ? q.x : index == 2 ? q.y : q.z;
        if ((index <= 1 && dropped < 0) || (index >= 2 && dropped > 0)) {
            s = {-q.x, -q.y, -q.z, -q.w};
            sign = -1.0;
        }
        switch (index) {
        case 1: rgb[0] = s.w; rgb[1] = s.y; rgb[2] = s.z; break;    // q = (w', y, z, x)
        case 2: rgb[0] = s.x; rgb[1] = -s.w; rgb[2] = s.z; break;   // q = (x, -w', z, -y)
        case 3: rgb[0] = s.x; rgb[1] = s.y; rgb[2] = -s.w; break;   // q = (x, y, -w', -z)
        default: rgb[0] = s.x; rgb[1] = s.y; rgb[2] = s.z; break;
        }
        return sign;
    }

    // tetrahedra around each pivot; spins chosen so the dropped index runs through 0..3
    inline RigidTruth writeRigid(const fs::path& root, const std::string& asset, bool smooth, int frames = 6) {
        RigidTruth truth;
        truth.frames = frames;
        truth.smooth = smooth;
        if (smooth) {
            truth.pieces.push_back({V3(0.5, 2, 0), V3(0, 1, 0), 10, V3(1.0, 4.0, 0.0), 1.0, V3(0, -9.8, 0.5)});
            truth.pieces.push_back({V3(-1, 1, 1), V3(1, 0, 0), 5, V3(0.0, 2.0, -1.0), 1.0, V3(0.2, -9.8, 0)});
        } else {
            truth.pieces.push_back({V3(2, 1, 0), V3(0, 0, 1), 140, V3(0.5, 0, 0), 1.0, V3()});    // fast spin: needs upsampling
            truth.pieces.push_back({V3(-1, 0.5, 3), V3(1, 0, 0), 100, V3(0, 0.3, 0), 1.0, V3()});
            truth.pieces.push_back({V3(0, 2, -2), V3(0, 1, 0), 110, V3(0, 0, -0.4), 0.5, V3()});   // half scale
        }
        const Target target{"unity"};
        const int B = (int)truth.pieces.size();
        const V3 tet[4] = {V3(0.2, 0, 0), V3(-0.1, 0.2, 0), V3(-0.1, -0.1, 0.2), V3(0, -0.1, -0.2)};
        for (int b = 0; b < B; b++) {
            for (const V3& c : tet) {
                truth.rest.push_back(truth.pieces[b].pivot + c);
                truth.pointPiece.push_back(b);
            }
        }
        const int N = frames;
        Image pos(B, N), rotTex(B, N), col(B, N);
        V3 lo(1e9, 1e9, 1e9), hi(-1e9, -1e9, -1e9);
        auto grow = [&](const V3& v) {
            lo = V3(std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z));
            hi = V3(std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z));
        };
        for (int f = 0; f < N; f++) {
            for (int b = 0; b < B; b++) {
                const V3 p = target.point(truth.position(b, f));
                grow(p);
                const Q4 qh = truth.rotation(b, f);
                const Q4 qt = target.quat(qh);
                double rgb[3];
                int index = 0;
                const double sign0 = encodeRotation(qt, rgb, index);
                // the angle to the next frame, in turns, negative when the next stored q must flip
                double angle = 0.0;
                if (f + 1 < N) {
                    double rgb1[3];
                    int index1 = 0;
                    const double sign1 = encodeRotation(target.quat(truth.rotation(b, f + 1)), rgb1, index1);
                    angle = truth.pieces[b].degreesPerFrame / 360.0 * sign0 * sign1;
                }
                const double alpha = (index + 1.0 - truth.pieces[b].scale) / 4.0;
                pos.set(b, f, p.x, p.y, p.z, alpha);
                rotTex.set(b, f, rgb[0], rgb[1], rgb[2], angle);
                const V3 acc = target.wspace(truth.pieces[b].acceleration);
                if (smooth) col.set(b, f, acc.x, acc.y, acc.z, 1.0);
                else col.set(b, f, 0.2 * b, 0.5, 1.0, 1.0);
            }
        }
        for (const V3& r : truth.rest) grow(target.point(r));
        const HBounds bounds = flaggedBounds(lo, hi, true, 1.0, 1.0, 1.0, 0.0);
        writeExrImage(root / "tex" / (asset + "_pos.exr"), pos);
        writeExrImage(root / "tex" / (asset + "_rot.exr"), rotTex);
        writeExrImage(root / "tex" / (asset + "_col.exr"), col);

        FbxMesh mesh;
        mesh.points = truth.rest;
        mesh.uvNames = {"uv", "uv2", "uv3", "uv4"};
        mesh.uvs.assign(4, {});
        const int faces[4][3] = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}};
        for (int b = 0; b < B; b++) {
            const V3 pivotW = target.wspace(truth.pieces[b].pivot);
            for (const auto& face : faces) {
                const V3 n = unit(cross3(truth.rest[b * 4 + face[1]] - truth.rest[b * 4 + face[0]], truth.rest[b * 4 + face[2]] - truth.rest[b * 4 + face[0]]));
                const V3 t = unit(cross3(V3(0.3, 1, 0.2), n));
                for (int k = 0; k < 3; k++) {
                    mesh.corners.push_back(b * 4 + face[k]);
                    mesh.uvs[0].push_back({0.25 * k, 0.5});
                    mesh.uvs[1].push_back({(b + 0.5) / B, 1.0 - 0.5 / N});
                    mesh.uvs[2].push_back({pivotW.x, 1.0});
                    mesh.uvs[3].push_back({pivotW.y, 1.0 - pivotW.z});
                    mesh.normals.push_back(n);
                    mesh.tangents.push_back(t);
                    mesh.binormals.push_back(cross3(n, t));
                }
            }
        }
        // guard triangle
        const int g = (int)mesh.points.size();
        mesh.points.push_back(target.back(V3(bounds.mn[0], bounds.mn[1], bounds.mn[2])));
        mesh.points.push_back(target.back(V3(bounds.mx[0], bounds.mx[1], bounds.mx[2])));
        mesh.points.push_back(V3(0, 0, 0));
        for (int k = 0; k < 3; k++) {
            mesh.corners.push_back(g + k);
            for (int s = 0; s < 4; s++) mesh.uvs[s].push_back({0, 0});
            mesh.normals.push_back(V3(0, 1, 0));
            mesh.tangents.push_back(V3(1, 0, 0));
            mesh.binormals.push_back(V3(0, 0, -1));
        }
        writeFbx(root / "geo" / (asset + "_mesh.fbx"), mesh);
        std::map<std::string, double> flags = {{"_B_LOAD_COL_TEX", 1}, {"_B_SMOOTH_TRAJECTORIES", smooth ? 1 : 0}, {"_B_pscaleAreInPosA", 1},
                                               {"_B_stretchByVel", 0}, {"_globalPscaleMul", 1}, {"_B_interpolate", 1}};
        writeText(root / "unity" / (asset + "_mat.mat"),
                  unityMaterial(RIGID_GUID, bounds, N, truth.fps, flags, smooth ? "_B_LOAD_COL_TEX _B_SMOOTH_TRAJECTORIES" : "_B_LOAD_COL_TEX"));
        return truth;
    }

}

#endif //VAT_HOUDINI_FIXTURES_H
