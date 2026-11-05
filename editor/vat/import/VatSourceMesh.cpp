// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The VAT importers' mesh reader. Unlike the model import (util/FbxImporter + GlbBuilder) it keeps
// the data exactly as stored: no unit conversion (FBX UnitScaleFactor is ignored), no V flip, every
// UV set, tangents (FBX tangent + bitangent -> handedness), no welding, reordering or recentring.
// The node transform is returned (as a similarity) for the importer to bake consistently with the
// animation data.

#include "vat/import/VatImportInternal.h"

#include "util/FileUtils.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/GltfNormalize.h"

#include "ufbx.h"
#include "tiny_gltf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;
using jasperengine::editor::FileUtils;

namespace jasperengine::editor::vat::importer{

int SourceMesh::findUv(const std::string& name) const{
    const std::string wanted = lower(name);
    for (size_t i = 0; i < uvNames.size(); i++){
        if (lower(uvNames[i]) == wanted) return (int)i;
    }
    return -1;
}

namespace{

    // a 3x4 column matrix (columns X, Y, Z, translation) as a similarity; false when it scales
    // unevenly, shears or mirrors
    bool similarityFrom(const double cols[4][3], MeshXform& out, std::string& error){
        D3 c0(cols[0][0], cols[0][1], cols[0][2]);
        D3 c1(cols[1][0], cols[1][1], cols[1][2]);
        D3 c2(cols[2][0], cols[2][1], cols[2][2]);
        const D3 t(cols[3][0], cols[3][1], cols[3][2]);
        const double s0 = length(c0), s1 = length(c1), s2 = length(c2);
        const double s = (s0 + s1 + s2) / 3.0;
        if (!(s > 1e-12)){
            error = "the mesh node's transform has a zero scale";
            return false;
        }
        const double tol = 1e-5 * s;
        if (std::fabs(s0 - s) > tol || std::fabs(s1 - s) > tol || std::fabs(s2 - s) > tol ||
            std::fabs(dot(c0, c1)) > tol * s || std::fabs(dot(c0, c2)) > tol * s || std::fabs(dot(c1, c2)) > tol * s ||
            dot(cross(c0, c1), c2) <= 0.0){
            error = "the mesh node's transform scales unevenly, shears or mirrors (scale " + num(s0) + ", " + num(s1) + ", " +
                    num(s2) + "): re-export with an identity transform (apply the transform to the mesh)";
            return false;
        }
        c0 = c0 * (1.0 / s0); c1 = c1 * (1.0 / s1); c2 = c2 * (1.0 / s2);
        out.rotation = quatFromColumns(c0, c1, c2);
        out.scale = s;
        out.translation = t;
        const bool identity = std::fabs(s - 1.0) < 1e-9 && length(t) < 1e-9 &&
                              std::fabs(std::fabs(out.rotation.w) - 1.0) < 1e-12;
        out.identity = identity;
        if (identity){
            out.rotation = DQ();
            out.scale = 1.0;
            out.translation = D3();
        }
        return true;
    }

    bool sameXform(const MeshXform& a, const MeshXform& b){
        if (a.identity != b.identity) return false;
        if (a.identity) return true;
        return std::fabs(std::fabs(qdot(a.rotation, b.rotation)) - 1.0) < 1e-9 && std::fabs(a.scale - b.scale) < 1e-9 * a.scale &&
               length(a.translation - b.translation) < 1e-9 * std::max(1.0, length(a.translation));
    }

    void finishBounds(SourceMesh& out){
        const double inf = std::numeric_limits<double>::infinity();
        out.boundsMin = D3(inf, inf, inf);
        out.boundsMax = D3(-inf, -inf, -inf);
        for (size_t i = 0; i + 2 < out.positions.size(); i += 3){
            out.boundsMin = D3(std::min(out.boundsMin.x, out.positions[i]), std::min(out.boundsMin.y, out.positions[i + 1]),
                               std::min(out.boundsMin.z, out.positions[i + 2]));
            out.boundsMax = D3(std::max(out.boundsMax.x, out.positions[i]), std::max(out.boundsMax.y, out.positions[i + 1]),
                               std::max(out.boundsMax.z, out.positions[i + 2]));
        }
        if (out.positions.empty()){
            out.boundsMin = D3();
            out.boundsMax = D3();
        }
    }

    // ---- FBX ----

    std::string ufbxText(const ufbx_string& s){
        return std::string(s.data, s.length);
    }

    // a texture's file on disk ("" when it only exists inside the FBX or is missing)
    std::string fbxTextureFile(const ufbx_texture* texture, const fs::path& dir, bool& embedded){
        if (!texture) return {};
        const std::string candidates[] = {ufbxText(texture->filename), ufbxText(texture->absolute_filename),
                                          ufbxText(texture->relative_filename)};
        for (const std::string& candidate : candidates){
            if (candidate.empty()) continue;
            fs::path path = FileUtils::pathFromUtf8(candidate);
            if (path.is_relative()) path = dir / path;
            if (fileExists(FileUtils::pathToUtf8(path))) return FileUtils::pathToUtf8(fs::absolute(path).lexically_normal());
            const fs::path beside = dir / FileUtils::pathFromUtf8(candidate).filename();
            if (fileExists(FileUtils::pathToUtf8(beside))) return FileUtils::pathToUtf8(fs::absolute(beside).lexically_normal());
        }
        if (texture->content.size > 0) embedded = true;
        return {};
    }

    VatMaterialData fbxMaterial(const ufbx_material* material, const fs::path& dir, bool& embedded){
        VatMaterialData out;
        out.name = ufbxText(material->name);
        const ufbx_material_map& base = material->pbr.base_color.has_value ? material->pbr.base_color : material->fbx.diffuse_color;
        if (base.has_value){
            out.baseColor[0] = (float)base.value_vec4.x;
            out.baseColor[1] = (float)base.value_vec4.y;
            out.baseColor[2] = (float)base.value_vec4.z;
            out.baseColor[3] = base.value_components >= 4 ? (float)base.value_vec4.w : 1.0f;
        }
        out.baseColorTexture = fbxTextureFile(material->pbr.base_color.texture ? material->pbr.base_color.texture
                                                                                : material->fbx.diffuse_color.texture, dir, embedded);
        out.normalTexture = fbxTextureFile(material->pbr.normal_map.texture ? material->pbr.normal_map.texture
                                                                             : material->fbx.normal_map.texture, dir, embedded);
        if (material->pbr.metalness.has_value) out.metallic = (float)material->pbr.metalness.value_real;
        if (material->pbr.roughness.has_value) out.roughness = (float)material->pbr.roughness.value_real;
        return out;
    }

    bool readFbx(const std::string& path, SourceMesh& out, std::string& error){
        ufbx_load_opts opts = {};
        opts.ignore_animation = true;
        opts.load_external_files = false;
        ufbx_error ufbxError;
        ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &ufbxError);
        if (!scene){
            error = "cannot read " + fileName(path) + ": " + std::string(ufbxError.description.data, ufbxError.description.length);
            return false;
        }
        const fs::path dir = FileUtils::pathFromUtf8(path).parent_path();
        std::map<const ufbx_material*, int> materialIndex;
        bool haveXform = false;
        bool ok = true;
        std::vector<uint32_t> triangleIndices;

        // the UV set names across every mesh, in first-seen order
        for (size_t n = 0; n < scene->nodes.count && ok; n++){
            const ufbx_node* node = scene->nodes.data[n];
            if (!node->mesh) continue;
            const ufbx_mesh* mesh = node->mesh;
            for (size_t u = 0; u < mesh->uv_sets.count; u++){
                const std::string name = ufbxText(mesh->uv_sets.data[u].name);
                if (std::find(out.uvNames.begin(), out.uvNames.end(), name) == out.uvNames.end()) out.uvNames.push_back(name);
            }
        }
        out.uvs.assign(out.uvNames.size(), {});

        bool anyNormals = false, anyTangents = false, anyColors = false;
        for (size_t n = 0; n < scene->nodes.count; n++){
            const ufbx_node* node = scene->nodes.data[n];
            if (!node->mesh) continue;
            anyNormals = anyNormals || node->mesh->vertex_normal.exists;
            anyTangents = anyTangents || (node->mesh->vertex_tangent.exists && node->mesh->vertex_normal.exists);
            anyColors = anyColors || node->mesh->vertex_color.exists;
        }

        for (size_t n = 0; n < scene->nodes.count && ok; n++){
            const ufbx_node* node = scene->nodes.data[n];
            if (!node->mesh) continue;
            const ufbx_mesh* mesh = node->mesh;

            double cols[4][3];
            for (int c = 0; c < 4; c++){
                cols[c][0] = node->geometry_to_world.cols[c].x;
                cols[c][1] = node->geometry_to_world.cols[c].y;
                cols[c][2] = node->geometry_to_world.cols[c].z;
            }
            MeshXform xform;
            if (!similarityFrom(cols, xform, error)){
                ok = false;
                break;
            }
            if (!haveXform){
                out.xform = xform;
                haveXform = true;
            }else if (!sameXform(out.xform, xform)){
                error = "the FBX has meshes with different node transforms: merge them into one object before exporting";
                ok = false;
                break;
            }

            const uint32_t pointOffset = (uint32_t)(out.positions.size() / 3);
            for (size_t v = 0; v < mesh->num_vertices; v++){
                out.positions.push_back(mesh->vertices.data[v].x);
                out.positions.push_back(mesh->vertices.data[v].y);
                out.positions.push_back(mesh->vertices.data[v].z);
            }
            std::vector<int> uvSlot(mesh->uv_sets.count, -1);
            for (size_t u = 0; u < mesh->uv_sets.count; u++){
                const std::string name = ufbxText(mesh->uv_sets.data[u].name);
                uvSlot[u] = (int)(std::find(out.uvNames.begin(), out.uvNames.end(), name) - out.uvNames.begin());
            }
            std::vector<int> faceMaterial(mesh->materials.count, -1);
            for (size_t m = 0; m < mesh->materials.count; m++){
                const ufbx_material* material = mesh->materials.data[m];
                auto found = materialIndex.find(material);
                if (found == materialIndex.end()){
                    out.materials.push_back(fbxMaterial(material, dir, out.embeddedTextures));
                    found = materialIndex.emplace(material, (int)out.materials.size() - 1).first;
                }
                faceMaterial[m] = found->second;
            }

            triangleIndices.resize(mesh->max_face_triangles * 3);
            for (size_t f = 0; f < mesh->num_faces; f++){
                const ufbx_face face = mesh->faces.data[f];
                const uint32_t triangles = ufbx_triangulate_face(triangleIndices.data(), triangleIndices.size(), mesh, face);
                int material = -1;
                if (mesh->face_material.count > f && mesh->face_material.data[f] < faceMaterial.size()){
                    material = faceMaterial[mesh->face_material.data[f]];
                }
                for (uint32_t t = 0; t < triangles; t++){
                    out.triangleMaterial.push_back(material);
                    for (int c = 0; c < 3; c++){
                        const uint32_t ix = triangleIndices[t * 3 + c];
                        out.point.push_back(pointOffset + mesh->vertex_indices.data[ix]);
                        for (size_t u = 0; u < out.uvs.size(); u++){
                            int set = -1;
                            for (size_t k = 0; k < uvSlot.size(); k++) if (uvSlot[k] == (int)u) set = (int)k;
                            if (set >= 0){
                                const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->uv_sets.data[set].vertex_uv, ix);
                                out.uvs[u].push_back(uv.x);
                                out.uvs[u].push_back(uv.y);
                            }else{
                                out.uvs[u].push_back(0.0);
                                out.uvs[u].push_back(0.0);
                            }
                        }
                        D3 normal(0, 1, 0);
                        if (anyNormals){
                            if (mesh->vertex_normal.exists){
                                const ufbx_vec3 nv = ufbx_get_vertex_vec3(&mesh->vertex_normal, ix);
                                normal = D3(nv.x, nv.y, nv.z);
                            }
                            out.normals.push_back((float)normal.x);
                            out.normals.push_back((float)normal.y);
                            out.normals.push_back((float)normal.z);
                        }
                        if (anyTangents){
                            D3 tangent(1, 0, 0);
                            double sign = 1.0;
                            if (mesh->vertex_tangent.exists){
                                const ufbx_vec3 tv = ufbx_get_vertex_vec3(&mesh->vertex_tangent, ix);
                                tangent = D3(tv.x, tv.y, tv.z);
                                if (mesh->vertex_bitangent.exists){
                                    const ufbx_vec3 bv = ufbx_get_vertex_vec3(&mesh->vertex_bitangent, ix);
                                    sign = dot(cross(normal, tangent), D3(bv.x, bv.y, bv.z)) < 0.0 ? -1.0 : 1.0;
                                }
                            }
                            out.tangents.push_back((float)tangent.x);
                            out.tangents.push_back((float)tangent.y);
                            out.tangents.push_back((float)tangent.z);
                            out.tangents.push_back((float)sign);
                        }
                        if (anyColors){
                            ufbx_vec4 color = {{{1, 1, 1, 1}}};
                            if (mesh->vertex_color.exists) color = ufbx_get_vertex_vec4(&mesh->vertex_color, ix);
                            out.colors.push_back((float)color.x);
                            out.colors.push_back((float)color.y);
                            out.colors.push_back((float)color.z);
                            out.colors.push_back((float)color.w);
                        }
                    }
                }
            }
        }
        ufbx_free_scene(scene);
        if (!ok) return false;
        out.gltf = false;
        return true;
    }

    // ---- glTF ----

    // an accessor without a bufferView is all zeros; its count is bounded (the vertex limit is far lower)
    constexpr size_t MAX_ZERO_ACCESSOR_ELEMENTS = (size_t)1 << 24;
    constexpr int MAX_UV_SETS = 32;

    // an accessor as doubles (normalized integers mapped to 0..1 / -1..1), `components` per element
    bool readAccessor(const tinygltf::Model& model, int index, std::vector<double>& values, int& components, std::string& error){
        if (index < 0 || index >= (int)model.accessors.size()){
            error = "accessor " + std::to_string(index) + " doesn't exist";
            return false;
        }
        const tinygltf::Accessor& accessor = model.accessors[(size_t)index];
        components = tinygltf::GetNumComponentsInType((uint32_t)accessor.type);
        const int componentSize = tinygltf::GetComponentSizeInBytes((uint32_t)accessor.componentType);
        if (components <= 0 || componentSize <= 0){
            error = "accessor " + std::to_string(index) + " has an unsupported type";
            return false;
        }
        if (accessor.sparse.isSparse){
            error = "accessor " + std::to_string(index) + " is sparse (not supported): export without sparse accessors";
            return false;
        }
        // every size below is checked before it is used: a crafted file can claim any count, stride,
        // offset or index (64-bit products overflow, indices point past the arrays)
        const std::string name = "accessor " + std::to_string(index);
        const uint64_t elementBytes = (uint64_t)components * (uint64_t)componentSize;
        if (accessor.bufferView < 0){
            // all zeros: no data backs the count, so it is bounded explicitly
            if (accessor.count > MAX_ZERO_ACCESSOR_ELEMENTS){
                error = name + " claims " + std::to_string(accessor.count) + " elements without any data";
                return false;
            }
            values.assign(accessor.count * (size_t)components, 0.0);
            return true;
        }
        if (accessor.bufferView >= (int)model.bufferViews.size()){
            error = name + " uses bufferView " + std::to_string(accessor.bufferView) + ", which doesn't exist";
            return false;
        }
        const tinygltf::BufferView& view = model.bufferViews[(size_t)accessor.bufferView];
        if (view.buffer < 0 || view.buffer >= (int)model.buffers.size()){
            error = name + "'s bufferView uses buffer " + std::to_string(view.buffer) + ", which doesn't exist";
            return false;
        }
        const tinygltf::Buffer& buffer = model.buffers[(size_t)view.buffer];
        const uint64_t stride = view.byteStride ? (uint64_t)view.byteStride : elementBytes;
        if (stride < elementBytes){
            error = name + "'s bufferView has a byteStride (" + std::to_string(view.byteStride) + ") smaller than an element (" +
                    std::to_string(elementBytes) + " bytes)";
            return false;
        }
        const uint64_t bufferSize = buffer.data.size();
        const uint64_t viewOffset = view.byteOffset, accessorOffset = accessor.byteOffset, count = accessor.count;
        // start + stride * (count - 1) + elementBytes <= bufferSize, without overflowing
        bool fits = viewOffset <= bufferSize && accessorOffset <= bufferSize - viewOffset;
        if (fits && count > 0){
            const uint64_t start = viewOffset + accessorOffset;
            const uint64_t room = bufferSize - start;
            fits = elementBytes <= room && (count - 1 == 0 || stride <= (room - elementBytes) / (count - 1));
        }
        if (!fits){
            error = name + " reads past its buffer";
            return false;
        }
        const size_t start = (size_t)(viewOffset + accessorOffset);
        values.assign(accessor.count * (size_t)components, 0.0);
        for (size_t i = 0; i < accessor.count; i++){
            const unsigned char* element = buffer.data.data() + start + (size_t)stride * i;
            for (int c = 0; c < components; c++){
                const unsigned char* p = element + c * componentSize;
                double v = 0.0;
                switch (accessor.componentType){
                    case TINYGLTF_COMPONENT_TYPE_FLOAT:{ float f; std::memcpy(&f, p, 4); v = f; break; }
                    case TINYGLTF_COMPONENT_TYPE_DOUBLE:{ double d; std::memcpy(&d, p, 8); v = d; break; }
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:{ v = *p; if (accessor.normalized) v /= 255.0; break; }
                    case TINYGLTF_COMPONENT_TYPE_BYTE:{ v = (int8_t)*p; if (accessor.normalized) v = std::max(v / 127.0, -1.0); break; }
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:{ uint16_t s; std::memcpy(&s, p, 2); v = s; if (accessor.normalized) v /= 65535.0; break; }
                    case TINYGLTF_COMPONENT_TYPE_SHORT:{ int16_t s; std::memcpy(&s, p, 2); v = s; if (accessor.normalized) v = std::max(v / 32767.0, -1.0); break; }
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:{ uint32_t s; std::memcpy(&s, p, 4); v = s; break; }
                    case TINYGLTF_COMPONENT_TYPE_INT:{ int32_t s; std::memcpy(&s, p, 4); v = s; break; }
                    default: error = "accessor " + std::to_string(index) + " has an unknown component type"; return false;
                }
                values[i * (size_t)components + (size_t)c] = v;
            }
        }
        return true;
    }

    // column-major 4x4 of a node's local transform
    void nodeMatrix(const tinygltf::Node& node, double m[16]){
        if (node.matrix.size() == 16){
            for (int i = 0; i < 16; i++) m[i] = node.matrix[(size_t)i];
            return;
        }
        D3 t, s(1, 1, 1);
        DQ r;
        if (node.translation.size() == 3) t = D3(node.translation[0], node.translation[1], node.translation[2]);
        if (node.scale.size() == 3) s = D3(node.scale[0], node.scale[1], node.scale[2]);
        if (node.rotation.size() == 4) r = qnormalize(DQ(node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]));
        const D3 x = rotate(r, D3(1, 0, 0)) * s.x, y = rotate(r, D3(0, 1, 0)) * s.y, z = rotate(r, D3(0, 0, 1)) * s.z;
        const double values[16] = {x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, t.x, t.y, t.z, 1};
        for (int i = 0; i < 16; i++) m[i] = values[i];
    }

    void multiply(const double a[16], const double b[16], double out[16]){
        double r[16];
        for (int c = 0; c < 4; c++){
            for (int row = 0; row < 4; row++){
                double v = 0.0;
                for (int k = 0; k < 4; k++) v += a[k * 4 + row] * b[c * 4 + k];
                r[c * 4 + row] = v;
            }
        }
        for (int i = 0; i < 16; i++) out[i] = r[i];
    }

    std::string gltfImageFile(const tinygltf::Model& model, int textureIndex, const fs::path& dir, bool& embedded){
        if (textureIndex < 0 || textureIndex >= (int)model.textures.size()) return {};
        const int source = model.textures[(size_t)textureIndex].source;
        if (source < 0 || source >= (int)model.images.size()) return {};
        const tinygltf::Image& image = model.images[(size_t)source];
        if (image.uri.empty() || image.uri.rfind("data:", 0) == 0){
            embedded = true;
            return {};
        }
        std::string uri;
        if (!tinygltf::URIDecode(image.uri, &uri, nullptr)) uri = image.uri;
        const fs::path path = dir / FileUtils::pathFromUtf8(uri);
        if (!fileExists(FileUtils::pathToUtf8(path))) return {};
        return FileUtils::pathToUtf8(fs::absolute(path).lexically_normal());
    }

    VatMaterialData gltfMaterial(const tinygltf::Model& model, const tinygltf::Material& material, const fs::path& dir, bool& embedded){
        VatMaterialData out;
        out.name = material.name;
        const auto& pbr = material.pbrMetallicRoughness;
        for (int i = 0; i < 4 && i < (int)pbr.baseColorFactor.size(); i++) out.baseColor[i] = (float)pbr.baseColorFactor[(size_t)i];
        out.metallic = (float)pbr.metallicFactor;
        out.roughness = (float)pbr.roughnessFactor;
        for (int i = 0; i < 3 && i < (int)material.emissiveFactor.size(); i++) out.emissive[i] = (float)material.emissiveFactor[(size_t)i];
        out.alphaMode = material.alphaMode.empty() ? "OPAQUE" : material.alphaMode;
        out.alphaCutoff = (float)material.alphaCutoff;
        out.doubleSided = material.doubleSided;
        // what MeshSystem reads from a glTF material besides the core slots
        out.unlit = material.extensions.count("KHR_materials_unlit") != 0;
        if (material.extras.IsObject() && material.extras.Has("jasperengine_foliage_transmission")){
            const tinygltf::Value& value = material.extras.Get("jasperengine_foliage_transmission");
            if (value.IsNumber() && std::isfinite(value.GetNumberAsDouble()))
                out.foliageTransmission = std::clamp((float)value.GetNumberAsDouble(), 0.0f, 1.0f);
        }
        out.baseColorTexture = gltfImageFile(model, pbr.baseColorTexture.index, dir, embedded);
        out.metallicRoughnessTexture = gltfImageFile(model, pbr.metallicRoughnessTexture.index, dir, embedded);
        out.normalTexture = gltfImageFile(model, material.normalTexture.index, dir, embedded);
        out.occlusionTexture = gltfImageFile(model, material.occlusionTexture.index, dir, embedded);
        out.emissiveTexture = gltfImageFile(model, material.emissiveTexture.index, dir, embedded);
        return out;
    }

    bool readGltf(const std::string& path, SourceMesh& out, std::string& error){
        tinygltf::TinyGLTF loader;
        tinygltf::Model model;
        std::string err, warn;
        // images are not decoded: only their URIs are needed
        loader.SetImageLoader([](tinygltf::Image*, const int, std::string*, std::string*, int, int, const unsigned char*, int, void*){
            return true;
        }, nullptr);
        // through the models' one redirect (a cooked .glb, an FBX's import: S53 R12)
        const bool loaded = MeshSystem::readGLTFFile(loader, &model, &err, &warn, path);
        if (!loaded){
            error = "cannot read " + fileName(path) + ": " + (err.empty() ? std::string("not a glTF file") : err);
            return false;
        }
        // compressed and quantized streams as core glTF (docs/specs/S53-model-import-cook.md R1)
        GltfNormalizeResult normalized;
        if (!GltfNormalize::run(model, path, normalized)){
            error = "cannot read " + fileName(path) + ": " + normalized.firstError();
            return false;
        }
        const fs::path dir = FileUtils::pathFromUtf8(path).parent_path();
        for (const tinygltf::Material& material : model.materials){
            out.materials.push_back(gltfMaterial(model, material, dir, out.embeddedTextures));
        }

        // nodes with meshes, with their world matrices
        std::vector<std::pair<int, std::vector<double>>> instances;
        std::vector<int> roots;
        const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : (model.scenes.empty() ? -1 : 0);
        if (sceneIndex >= 0 && sceneIndex < (int)model.scenes.size()){
            roots = model.scenes[(size_t)sceneIndex].nodes;
        }else{
            for (size_t i = 0; i < model.nodes.size(); i++) roots.push_back((int)i);
        }
        std::vector<std::pair<int, std::vector<double>>> stack;
        const std::vector<double> identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (int root : roots) stack.push_back({root, identity});
        size_t guard = 0;
        while (!stack.empty() && guard++ < 100000){
            auto [index, parent] = stack.back();
            stack.pop_back();
            if (index < 0 || index >= (int)model.nodes.size()) continue;
            const tinygltf::Node& node = model.nodes[(size_t)index];
            double local[16], world[16];
            nodeMatrix(node, local);
            multiply(parent.data(), local, world);
            std::vector<double> w(world, world + 16);
            if (node.mesh >= 0 && node.mesh < (int)model.meshes.size()) instances.push_back({node.mesh, w});
            for (int child : node.children) stack.push_back({child, w});
        }

        // TEXCOORD sets: the highest index used by any primitive
        int uvSets = 0;
        bool anyNormals = false, anyTangents = false, anyColors = false;
        for (const auto& instance : instances){
            for (const tinygltf::Primitive& primitive : model.meshes[(size_t)instance.first].primitives){
                for (const auto& attribute : primitive.attributes){
                    if (attribute.first.rfind("TEXCOORD_", 0) == 0){
                        const int set = std::atoi(attribute.first.c_str() + 9);
                        if (set < 0 || set >= MAX_UV_SETS){
                            error = "the glTF uses " + attribute.first + ": at most " + std::to_string(MAX_UV_SETS) + " UV sets are read";
                            return false;
                        }
                        uvSets = std::max(uvSets, set + 1);
                    }
                    if (attribute.first == "NORMAL") anyNormals = true;
                    if (attribute.first == "TANGENT") anyTangents = true;
                    if (attribute.first == "COLOR_0") anyColors = true;
                }
            }
        }
        for (int u = 0; u < uvSets; u++) out.uvNames.push_back("TEXCOORD_" + std::to_string(u));
        out.uvs.assign((size_t)uvSets, {});

        bool haveXform = false;
        for (const auto& instance : instances){
            double cols[4][3];
            for (int c = 0; c < 4; c++) for (int r = 0; r < 3; r++) cols[c][r] = instance.second[(size_t)(c * 4 + r)];
            MeshXform xform;
            if (!similarityFrom(cols, xform, error)) return false;
            if (!haveXform){
                out.xform = xform;
                haveXform = true;
            }else if (!sameXform(out.xform, xform)){
                error = "the glTF has meshes with different node transforms: merge them into one object before exporting";
                return false;
            }
            for (const tinygltf::Primitive& primitive : model.meshes[(size_t)instance.first].primitives){
                if (primitive.mode != -1 && primitive.mode != TINYGLTF_MODE_TRIANGLES) continue;
                auto position = primitive.attributes.find("POSITION");
                if (position == primitive.attributes.end()) continue;
                std::vector<double> positions;
                int components = 0;
                if (!readAccessor(model, position->second, positions, components, error)) return false;
                if (components != 3){
                    error = "POSITION is not a vec3";
                    return false;
                }
                const size_t count = positions.size() / 3;
                const uint32_t offset = (uint32_t)(out.positions.size() / 3);
                out.positions.insert(out.positions.end(), positions.begin(), positions.end());

                std::vector<double> indices;
                if (primitive.indices >= 0){
                    int indexComponents = 0;
                    if (!readAccessor(model, primitive.indices, indices, indexComponents, error)) return false;
                }else{
                    indices.resize(count);
                    for (size_t i = 0; i < count; i++) indices[i] = (double)i;
                }
                // every attribute has one element per vertex (glTF requires it; a shorter one would be
                // read past its end)
                auto attribute = [&](const std::string& name, std::vector<double>& values, int& comps) -> bool {
                    auto found = primitive.attributes.find(name);
                    values.clear();
                    comps = 0;
                    if (found == primitive.attributes.end()) return true;
                    if (!readAccessor(model, found->second, values, comps, error)) return false;
                    if (values.size() / (size_t)comps != count){
                        error = name + " has " + std::to_string(values.size() / (size_t)comps) + " elements but POSITION has " +
                                std::to_string(count) + ": every attribute of a primitive needs one per vertex";
                        values.clear();
                        return false;
                    }
                    return true;
                };
                std::vector<double> normals, tangents, colors;
                int normalComps = 0, tangentComps = 0, colorComps = 0;
                if (!attribute("NORMAL", normals, normalComps) || !attribute("TANGENT", tangents, tangentComps) ||
                    !attribute("COLOR_0", colors, colorComps)){
                    return false;
                }
                std::vector<std::vector<double>> uvs((size_t)uvSets);
                for (int u = 0; u < uvSets; u++){
                    int comps = 0;
                    if (!attribute("TEXCOORD_" + std::to_string(u), uvs[(size_t)u], comps)) return false;
                }
                const size_t triangles = indices.size() / 3;
                for (size_t t = 0; t < triangles; t++){
                    out.triangleMaterial.push_back(primitive.material >= 0 && primitive.material < (int)out.materials.size() ? primitive.material : -1);
                    for (int c = 0; c < 3; c++){
                        const double index = indices[t * 3 + (size_t)c];
                        if (!(index >= 0.0 && index < (double)count)){
                            error = "an index (" + num(index) + ") is past the " + std::to_string(count) + " vertices";
                            return false;
                        }
                        const size_t v = (size_t)index;
                        out.point.push_back(offset + (uint32_t)v);
                        for (int u = 0; u < uvSets; u++){
                            const std::vector<double>& set = uvs[(size_t)u];
                            out.uvs[(size_t)u].push_back(set.size() >= (v + 1) * 2 ? set[v * 2] : 0.0);
                            out.uvs[(size_t)u].push_back(set.size() >= (v + 1) * 2 ? set[v * 2 + 1] : 0.0);
                        }
                        if (anyNormals){
                            for (int k = 0; k < 3; k++) out.normals.push_back(normalComps == 3 ? (float)normals[v * 3 + (size_t)k] : (k == 1 ? 1.0f : 0.0f));
                        }
                        if (anyTangents){
                            for (int k = 0; k < 4; k++){
                                out.tangents.push_back(tangentComps == 4 ? (float)tangents[v * 4 + (size_t)k] : (k == 0 || k == 3 ? 1.0f : 0.0f));
                            }
                        }
                        if (anyColors){
                            for (int k = 0; k < 4; k++){
                                float value = 1.0f;
                                if (colorComps >= 3 && k < colorComps) value = (float)colors[v * (size_t)colorComps + (size_t)k];
                                out.colors.push_back(value);
                            }
                        }
                    }
                }
            }
        }
        out.gltf = true;
        return true;
    }

}

bool readSourceMesh(const std::string& path, SourceMesh& out, std::string& error){
    out = SourceMesh();
    if (!fileExists(path)){
        error = "the mesh " + path + " doesn't exist";
        return false;
    }
    const std::string ext = lower(FileUtils::pathToUtf8(FileUtils::pathFromUtf8(path).extension()));
    bool ok = false;
    if (ext == ".fbx"){
        ok = readFbx(path, out, error);
    }else if (ext == ".gltf" || ext == ".glb"){
        ok = readGltf(path, out, error);
    }else{
        error = fileName(path) + " is not an FBX or glTF mesh";
        return false;
    }
    if (!ok) return false;
    if (out.point.empty()){
        error = fileName(path) + " has no triangles";
        return false;
    }
    finishBounds(out);
    return true;
}

}
