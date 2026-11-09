// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleMeshes.h"

#include "component/MeshComponent.h"
#include "io/Data.h"
#include "io/File.h"
#include "io/FileData.h"
#include "io/ResourcePack.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/GltfNormalize.h"

#include "tiny_gltf.h"
#include "tiny_obj_loader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    constexpr float kPi = 3.14159265358979f;

    void vertex(ParticleMeshData& m, float x, float y, float z, float nx, float ny, float nz, float u, float v){
        m.vertices.insert(m.vertices.end(), {x, y, z, nx, ny, nz, u, v});
    }

    void quadIndices(ParticleMeshData& m, uint32_t a, uint32_t b, uint32_t c, uint32_t d){
        m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
    }

    // a surface of revolution around Y: rings of (radius, y, normal radial, normal y) from
    // bottom to top, `slices` around
    struct Ring { float radius, y, nr, ny, v; };
    void revolve(ParticleMeshData& m, const std::vector<Ring>& rings, int slices){
        const uint32_t base = m.vertexCount();
        for (const Ring& r : rings){
            for (int s = 0; s <= slices; s++){
                const float a = 2.0f * kPi * (float)s / (float)slices;
                const float c = std::cos(a), sn = std::sin(a);
                vertex(m, r.radius * c, r.y, -r.radius * sn, r.nr * c, r.ny, -r.nr * sn, (float)s / (float)slices, r.v);
            }
        }
        const uint32_t row = (uint32_t)slices + 1;
        for (uint32_t k = 0; k + 1 < rings.size(); k++){
            for (uint32_t s = 0; s < (uint32_t)slices; s++){
                const uint32_t a = base + k * row + s, b = a + 1, c = a + row + 1, d = a + row;
                quadIndices(m, a, b, c, d);
            }
        }
    }

    void disc(ParticleMeshData& m, float y, float radius, bool up, int slices){
        const uint32_t center = m.vertexCount();
        const float ny = up ? 1.0f : -1.0f;
        vertex(m, 0.0f, y, 0.0f, 0.0f, ny, 0.0f, 0.5f, 0.5f);
        for (int s = 0; s <= slices; s++){
            const float a = 2.0f * kPi * (float)s / (float)slices;
            const float c = std::cos(a), sn = std::sin(a);
            vertex(m, radius * c, y, -radius * sn, 0.0f, ny, 0.0f, 0.5f + 0.5f * c, 0.5f - 0.5f * sn);
        }
        for (int s = 0; s < slices; s++){
            const uint32_t a = center + 1 + (uint32_t)s, b = a + 1;
            if (up) m.indices.insert(m.indices.end(), {center, a, b});
            else m.indices.insert(m.indices.end(), {center, b, a});
        }
    }

    // normals for triangles that came without them: the area-weighted mean of the faces
    void computeNormals(ParticleMeshData& m){
        const uint32_t n = m.vertexCount();
        std::vector<float> acc(n * 3, 0.0f);
        float* v = m.vertices.data();
        constexpr uint32_t F = ParticleMeshData::kFloatsPerVertex;
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3){
            const uint32_t i0 = m.indices[t], i1 = m.indices[t + 1], i2 = m.indices[t + 2];
            const float e1[3] = {v[i1 * F] - v[i0 * F], v[i1 * F + 1] - v[i0 * F + 1], v[i1 * F + 2] - v[i0 * F + 2]};
            const float e2[3] = {v[i2 * F] - v[i0 * F], v[i2 * F + 1] - v[i0 * F + 1], v[i2 * F + 2] - v[i0 * F + 2]};
            const float c[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            for (uint32_t i : {i0, i1, i2})
                for (int k = 0; k < 3; k++) acc[i * 3 + k] += c[k];
        }
        for (uint32_t i = 0; i < n; i++){
            const float len = std::sqrt(acc[i * 3] * acc[i * 3] + acc[i * 3 + 1] * acc[i * 3 + 1] + acc[i * 3 + 2] * acc[i * 3 + 2]);
            for (int k = 0; k < 3; k++) v[i * F + 3 + k] = len > 0.0f ? acc[i * 3 + k] / len : (k == 1 ? 1.0f : 0.0f);
        }
    }

    // ---------------------------------------------------------------- files

    bool fileExists(const std::string& path, void*){
        if (ResourcePack::contains(path))
            return true;
        File file;
        return file.open(path.c_str()) == FileErrors::FILEDATA_OK;
    }

    bool readWholeFile(std::vector<unsigned char>* out, std::string* err, const std::string& path, void*){
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK){
            if (err) *err += "cannot read " + path + "\n";
            return false;
        }
        data.seek(0);
        const std::string bytes = data.readString(data.length());
        out->assign(bytes.begin(), bytes.end());
        return true;
    }

    bool fileSize(size_t* size, std::string* err, const std::string& path, void*){
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK){
            if (err) *err += "cannot read " + path + "\n";
            return false;
        }
        *size = data.length();
        return true;
    }

    std::string readFileToString(const char* path){
        Data data;
        if (data.open(path) != FileErrors::FILEDATA_OK)
            return std::string();
        data.seek(0);
        return data.readString(data.length());
    }

    // `components` floats per element of a glTF accessor (normalized integers decoded)
    bool readAccessor(const tinygltf::Model& model, int index, int components, std::vector<float>& out){
        if (index < 0 || index >= (int)model.accessors.size())
            return false;
        const tinygltf::Accessor& a = model.accessors[index];
        if (a.bufferView < 0 || a.bufferView >= (int)model.bufferViews.size())
            return false;
        const tinygltf::BufferView& view = model.bufferViews[a.bufferView];
        if (view.buffer < 0 || view.buffer >= (int)model.buffers.size())
            return false;
        const tinygltf::Buffer& buffer = model.buffers[view.buffer];
        const int stride = a.ByteStride(view);
        if (stride <= 0)
            return false;
        const size_t base = view.byteOffset + a.byteOffset;
        out.resize(a.count * (size_t)components);
        for (size_t e = 0; e < a.count; e++){
            const unsigned char* p = buffer.data.data() + base + e * (size_t)stride;
            if (base + e * (size_t)stride + (size_t)components * 4 > buffer.data.size() && a.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
                return false;
            for (int c = 0; c < components; c++){
                float value = 0.0f;
                switch (a.componentType){
                    case TINYGLTF_COMPONENT_TYPE_FLOAT: std::memcpy(&value, p + c * 4, 4); break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: value = p[c] / 255.0f; break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: { uint16_t s; std::memcpy(&s, p + c * 2, 2); value = s / 65535.0f; break; }
                    case TINYGLTF_COMPONENT_TYPE_BYTE: value = std::max((int8_t)p[c] / 127.0f, -1.0f); break;
                    case TINYGLTF_COMPONENT_TYPE_SHORT: { int16_t s; std::memcpy(&s, p + c * 2, 2); value = std::max(s / 32767.0f, -1.0f); break; }
                    default: return false;
                }
                out[e * (size_t)components + c] = value;
            }
        }
        return true;
    }

    bool readIndices(const tinygltf::Model& model, int index, std::vector<uint32_t>& out){
        const tinygltf::Accessor& a = model.accessors[index];
        if (a.bufferView < 0)
            return false;
        const tinygltf::BufferView& view = model.bufferViews[a.bufferView];
        const tinygltf::Buffer& buffer = model.buffers[view.buffer];
        const int stride = a.ByteStride(view);
        const size_t base = view.byteOffset + a.byteOffset;
        out.resize(a.count);
        for (size_t e = 0; e < a.count; e++){
            const unsigned char* p = buffer.data.data() + base + e * (size_t)stride;
            switch (a.componentType){
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: out[e] = p[0]; break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: { uint16_t s; std::memcpy(&s, p, 2); out[e] = s; break; }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: { uint32_t s; std::memcpy(&s, p, 4); out[e] = s; break; }
                default: return false;
            }
        }
        return true;
    }

    // a quantized mesh's vertices through its node's transform (scale, rotation, offset; a
    // matrix node as its columns), its normals through the rotation and the inverse scale
    void dequantize(ParticleMeshData& out, const tinygltf::Node& node){
        double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (node.matrix.size() == 16){
            std::copy(node.matrix.begin(), node.matrix.end(), m);
        }else{
            const double t[3] = {node.translation.size() == 3 ? node.translation[0] : 0.0, node.translation.size() == 3 ? node.translation[1] : 0.0,
                                 node.translation.size() == 3 ? node.translation[2] : 0.0};
            const double sc[3] = {node.scale.size() == 3 ? node.scale[0] : 1.0, node.scale.size() == 3 ? node.scale[1] : 1.0,
                                  node.scale.size() == 3 ? node.scale[2] : 1.0};
            double x = 0, y = 0, z = 0, w = 1;
            if (node.rotation.size() == 4){ x = node.rotation[0]; y = node.rotation[1]; z = node.rotation[2]; w = node.rotation[3]; }
            const double r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                                 2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                                 2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
            for (int c = 0; c < 3; c++){
                for (int row = 0; row < 3; row++)
                    m[c * 4 + row] = r[c * 3 + row] * sc[c];
            }
            m[12] = t[0]; m[13] = t[1]; m[14] = t[2];
        }
        // the normal matrix: columns of the linear part divided by their squared lengths
        double n[9];
        for (int c = 0; c < 3; c++){
            const double length2 = m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2];
            for (int row = 0; row < 3; row++)
                n[c * 3 + row] = length2 > 0.0 ? m[c * 4 + row] / length2 : 0.0;
        }
        for (size_t v = 0; v + ParticleMeshData::kFloatsPerVertex <= out.vertices.size(); v += ParticleMeshData::kFloatsPerVertex){
            float* p = &out.vertices[v];
            const double px = p[0], py = p[1], pz = p[2], nx = p[3], ny = p[4], nz = p[5];
            for (int row = 0; row < 3; row++){
                p[row] = (float)(m[row] * px + m[4 + row] * py + m[8 + row] * pz + m[12 + row]);
                p[3 + row] = (float)(n[row] * nx + n[3 + row] * ny + n[6 + row] * nz);
            }
            const float length = std::sqrt(p[3] * p[3] + p[4] * p[4] + p[5] * p[5]);
            if (length > 0.0f){
                p[3] /= length; p[4] /= length; p[5] /= length;
            }
        }
    }

    bool gltfMesh(const std::string& path, int index, ParticleMeshData& out, std::string* error){
        tinygltf::Model model;
        tinygltf::TinyGLTF loader;
        loader.SetFsCallbacks({&fileExists, &tinygltf::ExpandFilePath, &readWholeFile, &tinygltf::WriteWholeFile, &fileSize});
        // the shapes take geometry only: images stay undecoded (a KTX2 or WebP one too)
        loader.SetImagesAsIs(true);
        std::string err, warn;
        // through the models' one redirect (a cooked .glb, an FBX's import: S53 R12)
        const bool ok = MeshSystem::readGLTFFile(loader, &model, &err, &warn, path);
        if (!ok){
            if (error) *error = err.empty() ? "cannot read " + path : err;
            return false;
        }
        // the meshes stored as integers (KHR_mesh_quantization): their scale and offset are the
        // transform of the node that draws them, so the shape takes it (the others keep the mesh's
        // own space, as before)
        std::vector<int> quantizedNodes(model.meshes.size(), -1);
        for (size_t n = 0; n < model.nodes.size(); n++){
            const int mesh = model.nodes[n].mesh;
            if (mesh < 0 || mesh >= (int)model.meshes.size() || quantizedNodes[mesh] >= 0 || model.meshes[mesh].primitives.empty())
                continue;
            auto position = model.meshes[mesh].primitives[0].attributes.find("POSITION");
            if (position != model.meshes[mesh].primitives[0].attributes.end() && position->second >= 0 &&
                    position->second < (int)model.accessors.size() &&
                    model.accessors[position->second].componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
                quantizedNodes[mesh] = (int)n;
        }
        // compressed and quantized streams as core glTF (docs/specs/S53-model-import-cook.md R1)
        GltfNormalizeResult normalized;
        if (!GltfNormalize::run(model, path, normalized)){
            if (error) *error = normalized.firstError();
            return false;
        }
        int seen = 0;
        for (size_t meshIndex = 0; meshIndex < model.meshes.size(); meshIndex++){
            const tinygltf::Mesh& mesh = model.meshes[meshIndex];
            for (const tinygltf::Primitive& primitive : mesh.primitives){
                if (seen++ != index)
                    continue;
                if (primitive.mode != TINYGLTF_MODE_TRIANGLES && primitive.mode != -1){
                    if (error) *error = "the mesh is not made of triangles";
                    return false;
                }
                std::vector<float> positions, normals, uvs;
                auto attribute = [&](const char* name){
                    auto it = primitive.attributes.find(name);
                    return it == primitive.attributes.end() ? -1 : it->second;
                };
                if (!readAccessor(model, attribute("POSITION"), 3, positions)){
                    if (error) *error = "the mesh has no positions";
                    return false;
                }
                const size_t count = positions.size() / 3;
                const bool hasNormals = readAccessor(model, attribute("NORMAL"), 3, normals) && normals.size() == count * 3;
                const bool hasUVs = readAccessor(model, attribute("TEXCOORD_0"), 2, uvs) && uvs.size() == count * 2;
                out.vertices.clear();
                out.vertices.reserve(count * ParticleMeshData::kFloatsPerVertex);
                for (size_t v = 0; v < count; v++){
                    vertex(out, positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2],
                           hasNormals ? normals[v * 3] : 0.0f, hasNormals ? normals[v * 3 + 1] : 1.0f, hasNormals ? normals[v * 3 + 2] : 0.0f,
                           hasUVs ? uvs[v * 2] : 0.0f, hasUVs ? uvs[v * 2 + 1] : 0.0f);
                }
                out.indices.clear();
                if (primitive.indices >= 0){
                    if (!readIndices(model, primitive.indices, out.indices)){
                        if (error) *error = "the mesh's indices cannot be read";
                        return false;
                    }
                }else{
                    for (uint32_t v = 0; v < (uint32_t)count; v++) out.indices.push_back(v);
                }
                for (uint32_t i : out.indices){
                    if (i >= count){
                        if (error) *error = "the mesh's indices point past its vertices";
                        return false;
                    }
                }
                if (quantizedNodes[meshIndex] >= 0)
                    dequantize(out, model.nodes[quantizedNodes[meshIndex]]);
                if (!hasNormals)
                    computeNormals(out);
                return true;
            }
        }
        if (error) *error = "the model has no mesh " + std::to_string(index);
        return false;
    }

    bool objMesh(const std::string& path, int index, ParticleMeshData& out, std::string* error){
        tinyobj::attrib_t attrib;
        std::vector<tinyobj::shape_t> shapes;
        std::vector<tinyobj::material_t> materials;
        std::string warn, err;
        tinyobj::FileReader::externalFunc = readFileToString;
        const std::string text = readFileToString(path.c_str());
        if (text.empty()){
            if (error) *error = "cannot read " + path;
            return false;
        }
        std::istringstream stream(text);
        tinyobj::MaterialFileReader materialReader(FileData::getBaseDir(path));
        if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, &stream, &materialReader)){
            if (error) *error = err;
            return false;
        }
        if (index < 0 || index >= (int)shapes.size()){
            if (error) *error = "the model has no mesh " + std::to_string(index);
            return false;
        }
        const tinyobj::mesh_t& mesh = shapes[index].mesh;
        out.vertices.clear();
        out.indices.clear();
        bool hasNormals = true;
        size_t offset = 0;
        for (size_t f = 0; f < mesh.num_face_vertices.size(); f++){
            const int corners = mesh.num_face_vertices[f];
            const uint32_t first = out.vertexCount();
            for (int c = 0; c < corners; c++){
                const tinyobj::index_t& idx = mesh.indices[offset + c];
                const float* p = &attrib.vertices[3 * (size_t)idx.vertex_index];
                float n[3] = {0.0f, 1.0f, 0.0f}, t[2] = {0.0f, 0.0f};
                if (idx.normal_index >= 0){
                    std::memcpy(n, &attrib.normals[3 * (size_t)idx.normal_index], sizeof(n));
                }else{
                    hasNormals = false;
                }
                if (idx.texcoord_index >= 0){
                    t[0] = attrib.texcoords[2 * (size_t)idx.texcoord_index];
                    // OBJ's v grows upwards; the engine's textures have v down
                    t[1] = 1.0f - attrib.texcoords[2 * (size_t)idx.texcoord_index + 1];
                }
                vertex(out, p[0], p[1], p[2], n[0], n[1], n[2], t[0], t[1]);
            }
            // a fan over the face's corners
            for (int c = 1; c + 1 < corners; c++)
                out.indices.insert(out.indices.end(), {first, first + (uint32_t)c, first + (uint32_t)c + 1});
            offset += (size_t)corners;
        }
        if (!hasNormals)
            computeNormals(out);
        return !out.indices.empty();
    }

}

void ParticleMeshes::ring(float outerRadius, float innerRadius, float arcDegrees, int segments, ParticleMeshData& out){
    out.vertices.clear();
    out.indices.clear();
    const float arc = std::clamp(arcDegrees, 0.0f, 360.0f) / 360.0f;
    const int around = std::clamp(segments, 3, (int)kMaxRingSegments);
    // the arc's share of the circle's segments, one at least
    const int count = std::max(1, (int)std::ceil((float)around * arc - 1.0e-4f));
    const float span = arc * 6.28318530718f;
    for (int k = 0; k <= count; k++){
        const float u = (float)k / (float)count;
        const float c = std::cos(span * u), s = std::sin(span * u);
        vertex(out, c * outerRadius, s * outerRadius, 0.0f, 0, 0, 1, u, 0.0f);
        vertex(out, c * innerRadius, s * innerRadius, 0.0f, 0, 0, 1, u, 1.0f);
    }
    for (int k = 0; k < count; k++){
        const uint32_t outer = (uint32_t)k * 2, inner = outer + 1;
        // counter-clockwise seen from +Z
        quadIndices(out, inner, outer, outer + 2, inner + 2);
    }
}

void ParticleMeshes::primitive(PrimitiveMesh primitive, ParticleMeshData& out){
    out.vertices.clear();
    out.indices.clear();
    constexpr int kSlices = 24;
    switch (primitive){
        case PrimitiveMesh::Quad:
            vertex(out, -0.5f, -0.5f, 0.0f, 0, 0, 1, 0, 1);
            vertex(out, 0.5f, -0.5f, 0.0f, 0, 0, 1, 1, 1);
            vertex(out, 0.5f, 0.5f, 0.0f, 0, 0, 1, 1, 0);
            vertex(out, -0.5f, 0.5f, 0.0f, 0, 0, 1, 0, 0);
            quadIndices(out, 0, 1, 2, 3);
            break;
        case PrimitiveMesh::Plane:
            vertex(out, -0.5f, 0.0f, 0.5f, 0, 1, 0, 0, 1);
            vertex(out, 0.5f, 0.0f, 0.5f, 0, 1, 0, 1, 1);
            vertex(out, 0.5f, 0.0f, -0.5f, 0, 1, 0, 1, 0);
            vertex(out, -0.5f, 0.0f, -0.5f, 0, 1, 0, 0, 0);
            quadIndices(out, 0, 1, 2, 3);
            break;
        case PrimitiveMesh::Sphere: {
            std::vector<Ring> rings;
            const int stacks = 12;
            for (int k = 0; k <= stacks; k++){
                const float phi = kPi * (float)k / (float)stacks - 0.5f * kPi;
                rings.push_back({0.5f * std::cos(phi), 0.5f * std::sin(phi), std::cos(phi), std::sin(phi), 1.0f - (float)k / (float)stacks});
            }
            revolve(out, rings, kSlices);
            break;
        }
        case PrimitiveMesh::Cylinder:
            revolve(out, {{0.5f, -0.5f, 1.0f, 0.0f, 1.0f}, {0.5f, 0.5f, 1.0f, 0.0f, 0.0f}}, kSlices);
            disc(out, 0.5f, 0.5f, true, kSlices);
            disc(out, -0.5f, 0.5f, false, kSlices);
            break;
        case PrimitiveMesh::Capsule: {
            // radius 0.25, 1 tall: a cylinder of 0.5 between two half spheres
            std::vector<Ring> rings;
            const int half = 6;
            for (int k = 0; k <= half; k++){
                const float phi = -0.5f * kPi + 0.5f * kPi * (float)k / (float)half;
                rings.push_back({0.25f * std::cos(phi), -0.25f + 0.25f * std::sin(phi), std::cos(phi), std::sin(phi), 0.0f});
            }
            for (int k = 0; k <= half; k++){
                const float phi = 0.5f * kPi * (float)k / (float)half;
                rings.push_back({0.25f * std::cos(phi), 0.25f + 0.25f * std::sin(phi), std::cos(phi), std::sin(phi), 0.0f});
            }
            for (size_t k = 0; k < rings.size(); k++) rings[k].v = 1.0f - (rings[k].y + 0.5f);
            revolve(out, rings, kSlices);
            break;
        }
        case PrimitiveMesh::Torus: {
            // ring radius 0.35, tube radius 0.15
            const int sides = 12;
            const uint32_t base = out.vertexCount();
            for (int s = 0; s <= kSlices; s++){
                const float a = 2.0f * kPi * (float)s / (float)kSlices;
                const float ca = std::cos(a), sa = std::sin(a);
                for (int t = 0; t <= sides; t++){
                    const float b = 2.0f * kPi * (float)t / (float)sides;
                    const float cb = std::cos(b), sb = std::sin(b);
                    const float r = 0.35f + 0.15f * cb;
                    vertex(out, r * ca, 0.15f * sb, -r * sa, cb * ca, sb, -cb * sa, (float)s / (float)kSlices, (float)t / (float)sides);
                }
            }
            const uint32_t row = (uint32_t)sides + 1;
            for (uint32_t s = 0; s < (uint32_t)kSlices; s++){
                for (uint32_t t = 0; t < (uint32_t)sides; t++){
                    const uint32_t a = base + s * row + t, b = a + row, c = b + 1, d = a + 1;
                    quadIndices(out, a, b, c, d);
                }
            }
            break;
        }
        case PrimitiveMesh::Cube:
        default: {
            // six faces, each with its own normal
            const float faces[6][3][3] = {
                {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
                {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
                {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}};
            for (const auto& f : faces){
                const uint32_t base = out.vertexCount();
                const float* n = f[0];
                const float* u = f[1];
                const float* v = f[2];
                const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
                for (const auto& c : corners){
                    vertex(out, 0.5f * (n[0] + c[0] * u[0] + c[1] * v[0]), 0.5f * (n[1] + c[0] * u[1] + c[1] * v[1]),
                           0.5f * (n[2] + c[0] * u[2] + c[1] * v[2]), n[0], n[1], n[2], 0.5f + 0.5f * c[0], 0.5f - 0.5f * c[1]);
                }
                quadIndices(out, base, base + 1, base + 2, base + 3);
            }
            break;
        }
    }
}

bool ParticleMeshes::model(const std::string& path, int index, ParticleMeshData& out, std::string* error){
    const std::string ext = FileData::getFilePathExtension(path);
    if (ext == "gltf" || ext == "glb")
        return gltfMesh(path, index, out, error);
    if (ext == "obj")
        return objMesh(path, index, out, error);
    if (error) *error = "not a glTF or OBJ model: " + path;
    return false;
}

bool ParticleMeshes::meshComponent(MeshComponent& mesh, int index, ParticleMeshData& out){
    if (index < 0 || index >= (int)mesh.numSubmeshes)
        return false;
    std::map<std::string, Buffer*> buffers;
    if (mesh.buffer.getSize() > 0) buffers["vertices"] = &mesh.buffer;
    if (mesh.indices.getSize() > 0) buffers["indices"] = &mesh.indices;
    for (unsigned int i = 0; i < mesh.numExternalBuffers; i++) buffers[mesh.eBuffers[i].getName()] = &mesh.eBuffers[i];
    Submesh& submesh = mesh.submeshes[index];
    auto find = [&](AttributeType type, Buffer*& buffer) -> Attribute* {
        auto it = submesh.attributes.find(type);
        if (it != submesh.attributes.end()){
            auto b = buffers.find(it->second.getBufferName());
            if (b == buffers.end())
                return nullptr;
            buffer = b->second;
            return &it->second;
        }
        for (auto& b : buffers){
            if (b.second->isRenderAttributes() && b.second->getAttribute(type)){
                buffer = b.second;
                return b.second->getAttribute(type);
            }
        }
        return nullptr;
    };
    Buffer* posBuffer = nullptr;
    Buffer* normalBuffer = nullptr;
    Buffer* uvBuffer = nullptr;
    Buffer* indexBuffer = nullptr;
    Attribute* pos = find(AttributeType::POSITION, posBuffer);
    if (!pos || pos->getElements() != 3 || pos->getDataType() != AttributeDataType::FLOAT)
        return false;
    Attribute* normal = find(AttributeType::NORMAL, normalBuffer);
    Attribute* uv = find(AttributeType::TEXCOORD1, uvBuffer);
    Attribute* idx = find(AttributeType::INDEX, indexBuffer);
    const unsigned int count = pos->getCount();
    if (normal && (normal->getCount() != count || normal->getDataType() != AttributeDataType::FLOAT)) normal = nullptr;
    if (uv && (uv->getCount() != count || uv->getDataType() != AttributeDataType::FLOAT)) uv = nullptr;
    out.vertices.clear();
    out.indices.clear();
    for (unsigned int v = 0; v < count; v++){
        const Vector3 p = posBuffer->getVector3(pos, v);
        const Vector3 n = normal ? normalBuffer->getVector3(normal, v) : Vector3(0, 1, 0);
        const Vector2 t = uv ? uvBuffer->getVector2(uv, v) : Vector2(0, 0);
        vertex(out, p.x, p.y, p.z, n.x, n.y, n.z, t.x, t.y);
    }
    if (idx){
        const unsigned int indexCount = idx->getCount();
        for (unsigned int k = 0; k < indexCount; k++){
            const uint32_t value = idx->getDataType() == AttributeDataType::UNSIGNED_INT ? indexBuffer->getUInt32(idx, k) : indexBuffer->getUInt16(idx, k);
            if (value >= count)
                return false;
            out.indices.push_back(value);
        }
    }else{
        for (uint32_t v = 0; v + 2 < count; v += 3) out.indices.insert(out.indices.end(), {v, v + 1, v + 2});
    }
    if (!normal)
        computeNormals(out);
    return !out.indices.empty();
}

void ParticleMeshes::geometry(const ParticleMeshData& mesh, MeshGeometry& out){
    const uint32_t n = mesh.vertexCount();
    out.positions.resize((size_t)n * 3);
    out.normals.resize((size_t)n * 3);
    for (uint32_t v = 0; v < n; v++){
        for (int k = 0; k < 3; k++){
            out.positions[v * 3 + k] = mesh.vertices[v * ParticleMeshData::kFloatsPerVertex + k];
            out.normals[v * 3 + k] = mesh.vertices[v * ParticleMeshData::kFloatsPerVertex + 3 + k];
        }
    }
    out.indices = mesh.indices;
}
