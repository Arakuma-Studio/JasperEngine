// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Export as Model (docs/specs/V1-vertex-colors.md R61): one self-contained binary glTF with COLOR_0
// on every painted primitive (in the component's format) and hard color edges split into their own
// vertices, every other attribute and morph target following the split.
// - A Model's file is written again: the JSON is kept as it was read; every bufferView is moved into
//   the one GLB buffer (compressed ones decoded), a mesh several nodes use is copied for the painted
//   node, images keep their files (their URIs made relative to the new file). With Merge Static
//   Meshes, each merged submesh is the file primitive it was made of (the loader's order, checked
//   against the paint's topology).
// - A plain mesh (a built-in primitive) and an OBJ Model are written from their CPU geometry: one
//   node, a primitive per submesh, a metallic-roughness material per submesh from its material.
// Replace in Scene puts the new file in the Model's place (a plain mesh becomes a Model of it) and
// removes the component, in one undo step (the LatticeBakeCmd pattern).

#include "VertexColorEditing.h"
#include "VertexColorEditorInternal.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/CommandNames.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/LatticeBakeCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/RemoveComponentCmd.h"
#include "component/ModelComponent.h"
#include "component/VertexColorsComponent.h"
#include "io/AtomicFile.h"
#include "io/watch/FileWatchService.h"
#include "model/GltfDocument.h"
#include "subsystem/VertexColorScene.h"
#include "texture/Material.h"
#include "util/FbxImporter.h"
#include "util/FileUtils.h"
#include "vertexcolor/VertexColorSplit.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string_view>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::vertexcolor;
namespace vcd = jasperengine::editor::vertexcolor::detail;
using GltfJson = GltfDocument::Json;

namespace {

    void vcAlign4(std::vector<uint8_t>& bin) {
        while (bin.size() % 4 != 0) bin.push_back(0);
    }

    // a new bufferView of `bytes` in the bin; its index
    size_t vcAddView(GltfJson& root, std::vector<uint8_t>& bin, const std::vector<uint8_t>& bytes, int target) {
        vcAlign4(bin);
        GltfJson view = {{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", bytes.size()}};
        if (target) view["target"] = target;
        bin.insert(bin.end(), bytes.begin(), bytes.end());
        root["bufferViews"].push_back(view);
        return root["bufferViews"].size() - 1;
    }

    size_t vcAddAccessor(GltfJson& root, GltfJson accessor) {
        root["accessors"].push_back(std::move(accessor));
        return root["accessors"].size() - 1;
    }

    // the accessor's elements picked by `source` (one output element per entry), dense, as a new accessor
    bool vcRemapAccessor(const GltfDocument& doc, GltfJson& root, std::vector<uint8_t>& bin, size_t accessor, const std::vector<uint32_t>& source,
                         size_t& out, std::string& error) {
        std::vector<uint8_t> raw;
        GltfAccessorLayout layout;
        if (!doc.readAccessorRaw(accessor, raw, layout, error)) return false;
        std::vector<uint8_t> bytes(source.size() * layout.elementSize);
        for (size_t i = 0; i < source.size(); i++) {
            if (source[i] >= layout.count) {
                error = "accessor " + std::to_string(accessor) + " has fewer elements than the paint's vertices";
                return false;
            }
            std::memcpy(bytes.data() + i * layout.elementSize, raw.data() + (size_t)source[i] * layout.elementSize, layout.elementSize);
        }
        const GltfJson& old = doc.json()["accessors"][accessor];
        GltfJson created = {{"bufferView", vcAddView(root, bin, bytes, gltf::ARRAY_BUFFER)}, {"componentType", layout.componentType},
                        {"count", source.size()}, {"type", old.value("type", std::string("SCALAR"))}};
        if (layout.normalized) created["normalized"] = true;
        // the same values: the bounds stay true
        if (old.contains("min")) created["min"] = old["min"];
        if (old.contains("max")) created["max"] = old["max"];
        out = vcAddAccessor(root, std::move(created));
        return true;
    }

    size_t vcColorAccessor(GltfJson& root, std::vector<uint8_t>& bin, const std::vector<Vector4>& values, vc::Format format) {
        std::vector<uint8_t> bytes;
        GltfJson accessor = {{"count", values.size()}, {"type", "VEC4"}};
        if (format == vc::Format::RGBA8) {
            // glTF's normalized bytes, linear as COLOR_0 is
            bytes.resize(values.size() * 4);
            for (size_t i = 0; i < values.size(); i++) {
                const float c[4] = {values[i].x, values[i].y, values[i].z, values[i].w};
                for (int k = 0; k < 4; k++) bytes[i * 4 + (size_t)k] = (uint8_t)(std::min(1.0f, std::max(0.0f, c[k])) * 255.0f + 0.5f);
            }
            accessor["componentType"] = gltf::UNSIGNED_BYTE;
            accessor["normalized"] = true;
        } else {
            // glTF has no half floats: High Precision exports as floats
            bytes.resize(values.size() * 16);
            for (size_t i = 0; i < values.size(); i++) {
                const float c[4] = {values[i].x, values[i].y, values[i].z, values[i].w};
                std::memcpy(bytes.data() + i * 16, c, 16);
            }
            accessor["componentType"] = gltf::FLOAT;
        }
        accessor["bufferView"] = vcAddView(root, bin, bytes, gltf::ARRAY_BUFFER);
        return vcAddAccessor(root, std::move(accessor));
    }

    size_t vcIndexAccessor(GltfJson& root, std::vector<uint8_t>& bin, const std::vector<uint32_t>& indices, size_t vertexCount) {
        std::vector<uint8_t> bytes;
        int type = gltf::UNSIGNED_INT;
        if (vertexCount <= 65535) {
            type = gltf::UNSIGNED_SHORT;
            bytes.resize(indices.size() * 2);
            for (size_t i = 0; i < indices.size(); i++) {
                const uint16_t value = (uint16_t)indices[i];
                std::memcpy(bytes.data() + i * 2, &value, 2);
            }
        } else {
            bytes.resize(indices.size() * 4);
            std::memcpy(bytes.data(), indices.data(), bytes.size());
        }
        return vcAddAccessor(root, {{"bufferView", vcAddView(root, bin, bytes, gltf::ELEMENT_ARRAY_BUFFER)}, {"componentType", type},
                                    {"count", indices.size()}, {"type", "SCALAR"}});
    }

    bool vcRemoveExtension(GltfJson& root, const char* list, const char* name) {
        if (!root.contains(list) || !root[list].is_array()) return false;
        GltfJson kept = GltfJson::array();
        for (const GltfJson& item : root[list]) {
            if (!(item.is_string() && item.get<std::string>() == name)) kept.push_back(item);
        }
        const bool removed = kept.size() != root[list].size();
        if (kept.empty()) root.erase(list);
        else root[list] = std::move(kept);
        return removed;
    }

    std::string vcSafeName(const std::string& entityName) {
        std::string name;
        for (const char c : entityName) name += (c == '/' || c == '\\' || c == ':') ? '_' : c;
        return name;
    }

    std::string vcDefaultPath(const std::string& modelPath, const std::string& entityName) {
        const fs::path model = FileUtils::pathFromUtf8(modelPath);
        return (model.parent_path() / FileUtils::pathFromUtf8(FileUtils::pathToUtf8(model.stem()) + " (" + vcSafeName(entityName) + ").glb")).generic_string();
    }

    // the floats of `count` elements of `components` each, as a new accessor (POSITION gets its bounds)
    size_t vcFloatAccessor(GltfJson& root, std::vector<uint8_t>& bin, const float* values, size_t count, int components, bool bounds) {
        std::vector<uint8_t> bytes(count * (size_t)components * 4);
        if (!bytes.empty()) std::memcpy(bytes.data(), values, bytes.size());
        GltfJson accessor = {{"bufferView", vcAddView(root, bin, bytes, gltf::ARRAY_BUFFER)}, {"componentType", gltf::FLOAT},
                         {"count", count}, {"type", gltf::typeName(components)}};
        if (bounds && count > 0) {
            GltfJson lo = GltfJson::array(), hi = GltfJson::array();
            for (int k = 0; k < components; k++) {
                float a = values[k], b = values[k];
                for (size_t i = 1; i < count; i++) {
                    a = std::min(a, values[i * (size_t)components + (size_t)k]);
                    b = std::max(b, values[i * (size_t)components + (size_t)k]);
                }
                lo.push_back(a);
                hi.push_back(b);
            }
            accessor["min"] = lo;
            accessor["max"] = hi;
        }
        return vcAddAccessor(root, std::move(accessor));
    }

    // the file primitive's triangles are the ones the target was painted on (R9 topology)
    bool vcFileTopologyMatches(const GltfDocument& doc, const GltfJson& primitive, const vc::TargetData& target, std::string& why) {
        size_t position = 0;
        GltfAccessorLayout layout;
        if (!GltfDocument::getIndex(primitive["attributes"], "POSITION", position) || !doc.accessorLayout(position, layout, why)) {
            if (why.empty()) why = "it has no POSITION";
            return false;
        }
        vc::MeshView probe;
        probe.positions.resize(layout.count);
        size_t indices = 0;
        if (GltfDocument::getIndex(primitive, "indices", indices)) {
            if (!doc.readIndices(indices, probe.indices, why)) return false;
        } else {
            probe.indices.resize(layout.count);
            for (uint32_t i = 0; i < (uint32_t)layout.count; i++) probe.indices[i] = i;
        }
        if (!probe.fingerprint().sameTopology(target.fingerprint) || layout.count != target.base.size()) {
            why = "it has " + std::to_string(layout.count) + " vertices and " + std::to_string(probe.indices.size() / 3) + " triangles in the file, not the " +
                  std::to_string(target.base.size()) + " vertices and " + std::to_string(target.fingerprint.indexCount / 3) + " triangles painted";
            return false;
        }
        return true;
    }

    // R2: with Merge Static Meshes the targets are the merged submeshes. The loader fills them node by
    // node (the nodes with a mesh, ascending) and primitive by primitive, then reverses the list.
    std::vector<vc::TargetKey> vcMergedFileKeys(const GltfJson& root) {
        std::vector<vc::TargetKey> flat;
        if (!root.contains("nodes") || !root.contains("meshes")) return flat;
        for (size_t n = 0; n < root["nodes"].size(); n++) {
            size_t mesh = 0;
            if (!GltfDocument::getIndex(root["nodes"][n], "mesh", mesh) || mesh >= root["meshes"].size()) continue;
            const GltfJson& primitives = root["meshes"][mesh]["primitives"];
            for (size_t p = 0; primitives.is_array() && p < primitives.size(); p++) {
                vc::TargetKey key;
                key.node = (int32_t)n;
                key.primitive = (int32_t)p;
                flat.push_back(key);
            }
        }
        std::reverse(flat.begin(), flat.end());
        return flat;
    }

    // ---- a Model's file written again

    bool vcRewriteModelFile(Project* project, Scene* scene, Entity entity, const ModelComponent& model, const vc::Data& data,
                            const ExportModelOptions& options, const fs::path& destination, std::vector<uint8_t>& glb, std::string& error) {
        const std::string name = scene->getEntityName(entity);
        fs::path source = vcd::absoluteAssetPath(*project, model.filename.path);
        if (FbxImporter::isFbxFile(source)) source = project->getImportedModelPath(source);
        GltfDocument doc;
        std::string why;
        if (!doc.load(source, why)) {
            error = "The model's file could not be read: " + why;
            return false;
        }
        for (const std::string& required : doc.requiredExtensions()) {
            if (required == "KHR_draco_mesh_compression") {
                error = "The model is Draco-compressed: export it without Draco from its tool, then paint and export again.";
                return false;
            }
        }

        GltfJson root = doc.json();
        std::vector<uint8_t> bin;
        // every existing bufferView into the one buffer, decoded
        if (root.contains("bufferViews") && root["bufferViews"].is_array()) {
            for (size_t v = 0; v < root["bufferViews"].size(); v++) {
                const uint8_t* bytes = nullptr;
                size_t size = 0, stride = 0;
                if (!doc.viewData(v, bytes, size, stride, why)) {
                    error = "The model's file could not be read: " + why;
                    return false;
                }
                vcAlign4(bin);
                GltfJson view = {{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", size}};
                if (stride) view["byteStride"] = stride;
                if (root["bufferViews"][v].contains("target")) view["target"] = root["bufferViews"][v]["target"];
                bin.insert(bin.end(), bytes, bytes + size);
                root["bufferViews"][v] = std::move(view);
            }
        } else {
            root["bufferViews"] = GltfJson::array();
        }
        if (!root.contains("accessors")) root["accessors"] = GltfJson::array();
        vcRemoveExtension(root, "extensionsUsed", "EXT_meshopt_compression");
        vcRemoveExtension(root, "extensionsRequired", "EXT_meshopt_compression");
        vcRemoveExtension(root, "extensionsUsed", "KHR_meshopt_compression");
        vcRemoveExtension(root, "extensionsRequired", "KHR_meshopt_compression");

        const std::vector<vc::TargetKey> merged = vcMergedFileKeys(root);
        // the meshes several nodes draw: a painted node gets its own copy
        std::map<size_t, int> meshUsers;
        if (root.contains("nodes")) {
            for (const GltfJson& node : root["nodes"]) {
                size_t mesh = 0;
                if (GltfDocument::getIndex(node, "mesh", mesh)) meshUsers[mesh]++;
            }
        }
        std::map<int, size_t> copiedFor;   // node -> its mesh copy
        for (const vc::TargetData& target : data.targets) {
            const std::string where = target.key.describe();
            vc::TargetKey key = target.key;
            if (key.submesh >= 0) {
                if ((size_t)key.submesh >= merged.size()) {
                    error = where + " is not a primitive of the model's file: the file changed since it was painted. Open and save the scene, then export.";
                    return false;
                }
                key = merged[(size_t)key.submesh];
            }
            if (key.node < 0 || !root.contains("nodes") || (size_t)key.node >= root["nodes"].size()) {
                error = where + " is not a node of the model's file: the file changed since it was painted. Open and save the scene, then export.";
                return false;
            }
            GltfJson& node = root["nodes"][(size_t)key.node];
            size_t mesh = 0;
            if (!GltfDocument::getIndex(node, "mesh", mesh) || mesh >= root["meshes"].size()) {
                error = where + " draws no mesh in the model's file.";
                return false;
            }
            if (meshUsers[mesh] > 1 && !copiedFor.count(key.node)) {
                root["meshes"].push_back(root["meshes"][mesh]);
                copiedFor[key.node] = root["meshes"].size() - 1;
                node["mesh"] = root["meshes"].size() - 1;
            }
            if (copiedFor.count(key.node)) mesh = copiedFor[key.node];
            GltfJson& primitives = root["meshes"][mesh]["primitives"];
            if (key.primitive < 0 || (size_t)key.primitive >= primitives.size()) {
                error = where + " is not a primitive of the model's file.";
                return false;
            }
            GltfJson& primitive = primitives[(size_t)key.primitive];

            const std::shared_ptr<const vc::MeshView> view = vc::sceneMeshView(scene, entity, target.key);
            if (!view || view->fingerprint() != target.fingerprint || view->vertexCount() != target.base.size()) {
                error = where + " of '" + name + "' was painted on another version of its mesh: open the scene and save it (the colors are transferred), then export.";
                return false;
            }
            if (!vcFileTopologyMatches(doc, primitive, target, why)) {
                error = where + " of '" + name + "' is not the file's " + key.describe() + " (" + why + "): the file changed since it was painted. Open and save the scene, then export.";
                return false;
            }

            const vc::SplitResult split = options.splitHardEdges && target.hasOverrides() ? vc::buildSplit(target, *view, {}) : vc::SplitResult();
            if (split.split) {
                GltfJson attributes = GltfJson::object();
                for (auto it = primitive["attributes"].begin(); it != primitive["attributes"].end(); ++it) {
                    if (it.key() == "COLOR_0") continue;
                    size_t created = 0;
                    if (!vcRemapAccessor(doc, root, bin, it.value().get<size_t>(), split.sourceVertex, created, why)) {
                        error = where + ": " + why;
                        return false;
                    }
                    attributes[it.key()] = created;
                }
                primitive["attributes"] = std::move(attributes);
                if (primitive.contains("targets") && primitive["targets"].is_array()) {
                    for (GltfJson& morph : primitive["targets"]) {
                        for (auto it = morph.begin(); it != morph.end(); ++it) {
                            size_t created = 0;
                            if (!vcRemapAccessor(doc, root, bin, it.value().get<size_t>(), split.sourceVertex, created, why)) {
                                error = where + ": " + why;
                                return false;
                            }
                            it.value() = created;
                        }
                    }
                }
                primitive["indices"] = vcIndexAccessor(root, bin, split.indices, split.values.size());
                // levels over the old vertices no longer fit
                if (primitive.contains("extensions") && primitive["extensions"].is_object()) primitive["extensions"].erase("JE_mesh_lods");
            }
            primitive["attributes"]["COLOR_0"] = vcColorAccessor(root, bin, split.split ? split.values : target.base, data.format);
        }

        // one buffer, in the GLB; images beside the old file stay reachable from the new one
        root["buffers"] = GltfJson::array({GltfJson{{"byteLength", bin.size()}}});
        if (root.contains("images") && root["images"].is_array()) {
            for (GltfJson& image : root["images"]) {
                if (!image.contains("uri") || !image["uri"].is_string()) continue;
                const std::string uri = image["uri"].get<std::string>();
                if (uri.rfind("data:", 0) == 0) continue;
                const fs::path file = (source.parent_path() / FileUtils::pathFromUtf8(uri)).lexically_normal();
                image["uri"] = file.lexically_relative(destination.parent_path()).generic_string();
            }
        }
        glb = GltfDocument::writeGlb(root, bin);
        return true;
    }

    // ---- a plain mesh (or an OBJ Model) written from its CPU geometry

    // a texture of the material as a glTF texture reference (null when it names no file)
    GltfJson vcTextureRef(Project* project, GltfJson& root, const Texture& texture, int texCoord, const fs::path& destination, std::map<std::string, size_t>& images) {
        if (texture.empty() || texture.isFramebuffer() || texture.getPath().empty()) return GltfJson();
        const std::string path = texture.getPath();
        auto found = images.find(path);
        if (found == images.end()) {
            fs::path file = FileUtils::pathFromUtf8(path);
            if (!file.is_absolute()) file = vcd::absoluteAssetPath(*project, path);
            root["images"].push_back({{"uri", file.lexically_normal().lexically_relative(destination.parent_path()).generic_string()}});
            root["textures"].push_back({{"source", root["images"].size() - 1}});
            found = images.emplace(path, root["textures"].size() - 1).first;
        }
        GltfJson ref = {{"index", found->second}};
        if (texCoord > 0) ref["texCoord"] = texCoord;
        return ref;
    }

    // the metallic-roughness part of the engine's material (factors, the five maps, alpha, culling)
    GltfJson vcMaterial(Project* project, GltfJson& root, const Material& material, bool faceCulling, const std::string& name, const fs::path& destination,
                    std::map<std::string, size_t>& images) {
        const Vector4& base = material.baseColorFactor;
        GltfJson pbr = {{"baseColorFactor", {base.x, base.y, base.z, base.w}}, {"metallicFactor", material.metallicFactor}, {"roughnessFactor", material.roughnessFactor}};
        GltfJson out = {{"name", name}};
        if (GltfJson ref = vcTextureRef(project, root, material.baseColorTexture, material.baseColorTexCoord, destination, images); !ref.is_null()) pbr["baseColorTexture"] = ref;
        if (GltfJson ref = vcTextureRef(project, root, material.metallicRoughnessTexture, material.metallicRoughnessTexCoord, destination, images); !ref.is_null())
            pbr["metallicRoughnessTexture"] = ref;
        out["pbrMetallicRoughness"] = pbr;
        if (GltfJson ref = vcTextureRef(project, root, material.normalTexture, material.normalTexCoord, destination, images); !ref.is_null()) out["normalTexture"] = ref;
        if (GltfJson ref = vcTextureRef(project, root, material.occlusionTexture, material.occlusionTexCoord, destination, images); !ref.is_null()) out["occlusionTexture"] = ref;
        if (GltfJson ref = vcTextureRef(project, root, material.emissiveTexture, material.emissiveTexCoord, destination, images); !ref.is_null()) out["emissiveTexture"] = ref;
        const Vector3& emissive = material.emissiveFactor;
        if (emissive.x != 0.0f || emissive.y != 0.0f || emissive.z != 0.0f) out["emissiveFactor"] = {emissive.x, emissive.y, emissive.z};
        if (material.alphaMode == MaterialAlphaMode::MASK) {
            out["alphaMode"] = "MASK";
            out["alphaCutoff"] = material.alphaCutoff;
        } else if (material.alphaMode == MaterialAlphaMode::BLEND) {
            out["alphaMode"] = "BLEND";
        }
        if (!faceCulling || material.cullMode == MaterialCullMode::OFF) out["doubleSided"] = true;
        return out;
    }

    bool vcBuildMeshFile(Project* project, Scene* scene, Entity entity, const vc::Data& data, const ExportModelOptions& options, const fs::path& destination,
                         std::vector<uint8_t>& glb, std::string& error) {
        const std::string name = scene->getEntityName(entity);
        GltfJson root = {{"asset", {{"version", "2.0"}, {"generator", "JasperEngine Export as Model"}}}, {"bufferViews", GltfJson::array()}, {"accessors", GltfJson::array()},
                     {"materials", GltfJson::array()}, {"images", GltfJson::array()}, {"textures", GltfJson::array()}};
        std::vector<uint8_t> bin;
        std::map<std::string, size_t> images;
        GltfJson primitives = GltfJson::array();
        for (const vc::SceneTarget& info : vc::sceneTargets(scene, entity)) {
            const std::string where = info.key.describe();
            const std::shared_ptr<const vc::MeshView> view = vc::sceneMeshView(scene, entity, info.key);
            const Material* material = vc::sceneTargetMaterial(scene, info);
            if (!view || !material) {
                error = where + " of '" + name + "' has no geometry loaded: wait for it to load, then export.";
                return false;
            }
            const vc::TargetData* target = data.find(info.key);
            if (target && (view->fingerprint() != target->fingerprint || view->vertexCount() != target->base.size())) {
                error = where + " of '" + name + "' was painted on another version of its mesh: open the scene and save it (the colors are transferred), then export.";
                return false;
            }
            const vc::SplitResult split = target && options.splitHardEdges && target->hasOverrides() ? vc::buildSplit(*target, *view, {}) : vc::SplitResult();
            std::vector<uint32_t> identity;
            if (!split.split) {
                identity.resize(view->vertexCount());
                for (uint32_t v = 0; v < view->vertexCount(); v++) identity[v] = v;
            }
            const std::vector<uint32_t>& source = split.split ? split.sourceVertex : identity;
            auto gather = [&](const auto& values, auto&& write) {
                for (uint32_t v : source) write(values[v]);
            };
            std::vector<float> positions, normals, uv0, uv1;
            gather(view->positions, [&](const Vector3& p) { positions.insert(positions.end(), {p.x, p.y, p.z}); });
            GltfJson attributes = {{"POSITION", vcFloatAccessor(root, bin, positions.data(), source.size(), 3, true)}};
            if (view->normals.size() == view->vertexCount()) {
                gather(view->normals, [&](const Vector3& n) { normals.insert(normals.end(), {n.x, n.y, n.z}); });
                attributes["NORMAL"] = vcFloatAccessor(root, bin, normals.data(), source.size(), 3, false);
            }
            if (view->uv0.size() == view->vertexCount()) {
                gather(view->uv0, [&](const Vector2& t) { uv0.insert(uv0.end(), {t.x, t.y}); });
                attributes["TEXCOORD_0"] = vcFloatAccessor(root, bin, uv0.data(), source.size(), 2, false);
            }
            if (view->uv1.size() == view->vertexCount()) {
                gather(view->uv1, [&](const Vector2& t) { uv1.insert(uv1.end(), {t.x, t.y}); });
                attributes["TEXCOORD_1"] = vcFloatAccessor(root, bin, uv1.data(), source.size(), 2, false);
            }
            // the paint, else the mesh's own colors
            if (target) {
                attributes["COLOR_0"] = vcColorAccessor(root, bin, split.split ? split.values : target->base, data.format);
            } else if (view->assetColors.size() == view->vertexCount()) {
                attributes["COLOR_0"] = vcColorAccessor(root, bin, view->assetColors, vc::Format::RGBA16F);
            }
            const std::string materialName = info.material.empty() ? "Submesh " + std::to_string(info.submesh) : info.material;
            root["materials"].push_back(vcMaterial(project, root, *material, info.faceCulling, materialName, destination, images));
            primitives.push_back({{"attributes", attributes},
                                  {"indices", vcIndexAccessor(root, bin, split.split ? split.indices : view->indices, source.size())},
                                  {"material", root["materials"].size() - 1}});
        }
        if (primitives.empty()) {
            error = "'" + name + "' has no mesh loaded to export: wait for it to load, then export.";
            return false;
        }
        // the loader reverses a node's primitives into submeshes: written last to first, submesh i
        // of the new Model is submesh i of this mesh
        std::reverse(primitives.begin(), primitives.end());
        root["meshes"] = GltfJson::array({GltfJson{{"name", name}, {"primitives", primitives}}});
        root["nodes"] = GltfJson::array({GltfJson{{"name", name}, {"mesh", 0}}});
        root["scenes"] = GltfJson::array({GltfJson{{"nodes", {0}}}});
        root["scene"] = 0;
        if (root["images"].empty()) {
            root.erase("images");
            root.erase("textures");
        }
        root["buffers"] = GltfJson::array({GltfJson{{"byteLength", bin.size()}}});
        glb = GltfDocument::writeGlb(root, bin);
        return true;
    }

}

bool editor::vertexcolor::exportAsModel(Project* project, uint32_t sceneId, Entity entity, const ExportModelOptions& options, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    Scene* scene = vcd::sceneOf(project, sceneId);
    if (!scene || !scene->isEntityCreated(entity)) return fail("The GameObject does not exist.");
    const ModelComponent* model = scene->findComponent<ModelComponent>(entity);
    const std::string name = CommandNames::entity(project, sceneId, entity);
    std::string why;
    if (!canPaint(project, sceneId, entity, &why)) return fail(why);
    const std::shared_ptr<vc::Data> data = vcd::resolve(project, sceneId, entity, error);
    if (!data) return error && !error->empty() ? false : fail(name + " has no vertex colors to export: paint it first.");
    if (data->targets.empty()) return fail(name + " has no painted mesh to export: paint it first.");

    // a Model's glTF is written again; a plain mesh, or a Model of an OBJ, from its geometry
    const bool modelFile = model && !model->filename.path.empty() && FileUtils::pathFromUtf8(model->filename.path).extension() != ".obj";
    const VertexColorsComponent* component = vcd::componentOf(project, sceneId, entity);
    std::string relative = options.path;
    if (relative.empty()) {
        relative = model && !model->filename.path.empty()
                       ? vcDefaultPath(model->filename.path, scene->getEntityName(entity))
                       : (FileUtils::pathFromUtf8(component->data.path).parent_path() / FileUtils::pathFromUtf8(vcSafeName(scene->getEntityName(entity)) + ".glb")).generic_string();
    }
    if (FileUtils::pathFromUtf8(relative).extension() != ".glb") return fail("path: Export as Model writes a binary glTF; end the name with .glb.");
    const fs::path destination = vcd::absoluteAssetPath(*project, relative);
    if (!project->isInsideAssetsPath(destination)) return fail("path: '" + relative + "' is outside the project's assets.");
    std::error_code ec;
    if (fs::exists(destination, ec)) return fail("path: '" + relative + "' exists already; choose another name.");

    std::vector<uint8_t> glb;
    const bool built = modelFile ? vcRewriteModelFile(project, scene, entity, *model, *data, options, destination, glb, why)
                                 : vcBuildMeshFile(project, scene, entity, *data, options, destination, glb, why);
    if (!built) return fail(why);
    fs::create_directories(destination.parent_path(), ec);
    {
        ScopedExpectedWrite expected(destination);
        const io::WriteResult written = io::writeFileAtomic(destination, std::string_view(reinterpret_cast<const char*>(glb.data()), glb.size()));
        if (!written.ok()) return fail("Could not write '" + relative + "': " + io::describeWriteError(written));
    }
    AssetDatabase& database = project->getAssetDatabase();
    if (database.getAssetGuid(destination).isNull()) project->onAssetFilesChanged(database.importAssets(destination));
    if (!options.replaceInScene) return true;

    // the new file in the Model's place and the paint it carries off the GameObject: one step
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t mark = history->mark();
    auto rollback = [&](const std::string& why) {
        while (history->position() > mark && history->canUndo() && history->undo()) {}
        return fail("'" + relative + "' was written, but " + why + "; it stays on disk.");
    };
    if (modelFile) {
        LatticeBakeCmd::ModelChange change;
        change.entity = entity;
        change.filename = relative;
        if (!history->addCommandNoMerge(new LatticeBakeCmd(project, sceneId, {change}, {}))) return rollback("the Model could not load it");
        if (!history->addCommandNoMerge(new RemoveComponentCmd(project, sceneId, entity, ComponentType::VertexColorsComponent)))
            return rollback("the Vertex Colors component could not be removed");
    } else {
        // the plain mesh leaves with its geometry (an undo puts it back) and a Model of the file comes
        if (!history->addCommandNoMerge(new RemoveComponentCmd(project, sceneId, entity, ComponentType::VertexColorsComponent)))
            return rollback("the Vertex Colors component could not be removed");
        if (!history->addCommandNoMerge(new RemoveComponentCmd(project, sceneId, entity, ComponentType::MeshComponent)))
            return rollback("the Mesh could not be removed");
        if (!history->addCommandNoMerge(new AddComponentCmd(project, sceneId, entity, ComponentType::MeshComponent)) ||
            !history->addCommandNoMerge(new AddComponentCmd(project, sceneId, entity, ComponentType::ModelComponent)))
            return rollback("a Model could not be added");
        if (!history->addCommandNoMerge(new ModelLoadCmd(project, sceneId, entity, relative))) return rollback("the Model could not load it");
    }
    history->groupSince(mark, "Export as Model (" + name + ")");
    return true;
}
