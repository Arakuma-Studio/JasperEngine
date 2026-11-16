// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Export as Model (docs/specs/V1-vertex-colors.md R61): the Model's file written again as one
// self-contained binary glTF with COLOR_0 on every painted primitive (in the component's format) and
// hard color edges split into their own vertices, every other attribute and morph target following
// the split. The JSON is kept as it was read; every bufferView is moved into the one GLB buffer
// (compressed ones decoded), a mesh several nodes use is copied for the painted node, images keep
// their files (their URIs made relative to the new file). Replace in Scene swaps the Model's file and
// removes the component in one undo step (the LatticeBakeCmd pattern).

#include "VertexColorEditing.h"
#include "VertexColorEditorInternal.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/CommandNames.h"
#include "command/type/LatticeBakeCmd.h"
#include "command/type/RemoveComponentCmd.h"
#include "component/ModelComponent.h"
#include "component/VertexColorsComponent.h"
#include "io/AtomicFile.h"
#include "io/watch/FileWatchService.h"
#include "model/GltfDocument.h"
#include "subsystem/VertexColorScene.h"
#include "util/FbxImporter.h"
#include "util/FileUtils.h"
#include "vertexcolor/VertexColorSplit.h"

#include <cstring>
#include <map>
#include <string_view>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::vertexcolor;
namespace vcd = jasperengine::editor::vertexcolor::detail;
using Json = GltfDocument::Json;

namespace {

    void vcAlign4(std::vector<uint8_t>& bin) {
        while (bin.size() % 4 != 0) bin.push_back(0);
    }

    // a new bufferView of `bytes` in the bin; its index
    size_t vcAddView(Json& root, std::vector<uint8_t>& bin, const std::vector<uint8_t>& bytes, int target) {
        vcAlign4(bin);
        Json view = {{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", bytes.size()}};
        if (target) view["target"] = target;
        bin.insert(bin.end(), bytes.begin(), bytes.end());
        root["bufferViews"].push_back(view);
        return root["bufferViews"].size() - 1;
    }

    size_t vcAddAccessor(Json& root, Json accessor) {
        root["accessors"].push_back(std::move(accessor));
        return root["accessors"].size() - 1;
    }

    // the accessor's elements picked by `source` (one output element per entry), dense, as a new accessor
    bool vcRemapAccessor(const GltfDocument& doc, Json& root, std::vector<uint8_t>& bin, size_t accessor, const std::vector<uint32_t>& source,
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
        const Json& old = doc.json()["accessors"][accessor];
        Json created = {{"bufferView", vcAddView(root, bin, bytes, gltf::ARRAY_BUFFER)}, {"componentType", layout.componentType},
                        {"count", source.size()}, {"type", old.value("type", std::string("SCALAR"))}};
        if (layout.normalized) created["normalized"] = true;
        // the same values: the bounds stay true
        if (old.contains("min")) created["min"] = old["min"];
        if (old.contains("max")) created["max"] = old["max"];
        out = vcAddAccessor(root, std::move(created));
        return true;
    }

    size_t vcColorAccessor(Json& root, std::vector<uint8_t>& bin, const std::vector<Vector4>& values, vc::Format format) {
        std::vector<uint8_t> bytes;
        Json accessor = {{"count", values.size()}, {"type", "VEC4"}};
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

    size_t vcIndexAccessor(Json& root, std::vector<uint8_t>& bin, const std::vector<uint32_t>& indices, size_t vertexCount) {
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

    bool vcRemoveExtension(Json& root, const char* list, const char* name) {
        if (!root.contains(list) || !root[list].is_array()) return false;
        Json kept = Json::array();
        for (const Json& item : root[list]) {
            if (!(item.is_string() && item.get<std::string>() == name)) kept.push_back(item);
        }
        const bool removed = kept.size() != root[list].size();
        if (kept.empty()) root.erase(list);
        else root[list] = std::move(kept);
        return removed;
    }

    std::string vcDefaultPath(const std::string& modelPath, const std::string& entityName) {
        const fs::path model = FileUtils::pathFromUtf8(modelPath);
        std::string name;
        for (const char c : entityName) name += (c == '/' || c == '\\' || c == ':') ? '_' : c;
        return (model.parent_path() / FileUtils::pathFromUtf8(FileUtils::pathToUtf8(model.stem()) + " (" + name + ").glb")).generic_string();
    }

}

bool editor::vertexcolor::exportAsModel(Project* project, uint32_t sceneId, Entity entity, const ExportModelOptions& options, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    Scene* scene = vcd::sceneOf(project, sceneId);
    const ModelComponent* model = scene ? scene->findComponent<ModelComponent>(entity) : nullptr;
    const std::string name = CommandNames::entity(project, sceneId, entity);
    if (!model || model->filename.path.empty()) {
        return fail("Export as Model writes a Model's file again: " + name + " has no Model (a Mesh primitive keeps its paint in the component).");
    }
    if (model->mergeStaticMeshes) {
        return fail("Turn off the Model's Merge Static Meshes first: Export as Model writes the file's own meshes, and merged ones are not.");
    }
    const std::shared_ptr<vc::Data> data = vcd::resolve(project, sceneId, entity, error);
    if (!data) return error && !error->empty() ? false : fail(name + " has no vertex colors to export: paint it first.");
    if (data->targets.empty()) return fail(name + " has no painted mesh to export: paint it first.");

    std::string relative = options.path.empty() ? vcDefaultPath(model->filename.path, scene->getEntityName(entity)) : options.path;
    if (FileUtils::pathFromUtf8(relative).extension() != ".glb") return fail("path: Export as Model writes a binary glTF; end the name with .glb.");
    const fs::path destination = vcd::absoluteAssetPath(*project, relative);
    if (!project->isInsideAssetsPath(destination)) return fail("path: '" + relative + "' is outside the project's assets.");
    std::error_code ec;
    if (fs::exists(destination, ec)) return fail("path: '" + relative + "' exists already; choose another name.");

    fs::path source = vcd::absoluteAssetPath(*project, model->filename.path);
    if (FbxImporter::isFbxFile(source)) source = project->getImportedModelPath(source);
    GltfDocument doc;
    std::string why;
    if (!doc.load(source, why)) return fail("The model's file could not be read: " + why);
    for (const std::string& required : doc.requiredExtensions()) {
        if (required == "KHR_draco_mesh_compression") return fail("The model is Draco-compressed: export it without Draco from its tool, then paint and export again.");
    }

    Json root = doc.json();
    std::vector<uint8_t> bin;
    // every existing bufferView into the one buffer, decoded
    if (root.contains("bufferViews") && root["bufferViews"].is_array()) {
        for (size_t v = 0; v < root["bufferViews"].size(); v++) {
            const uint8_t* bytes = nullptr;
            size_t size = 0, stride = 0;
            if (!doc.viewData(v, bytes, size, stride, why)) return fail("The model's file could not be read: " + why);
            vcAlign4(bin);
            Json view = {{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", size}};
            if (stride) view["byteStride"] = stride;
            if (root["bufferViews"][v].contains("target")) view["target"] = root["bufferViews"][v]["target"];
            bin.insert(bin.end(), bytes, bytes + size);
            root["bufferViews"][v] = std::move(view);
        }
    } else {
        root["bufferViews"] = Json::array();
    }
    if (!root.contains("accessors")) root["accessors"] = Json::array();
    vcRemoveExtension(root, "extensionsUsed", "EXT_meshopt_compression");
    vcRemoveExtension(root, "extensionsRequired", "EXT_meshopt_compression");
    vcRemoveExtension(root, "extensionsUsed", "KHR_meshopt_compression");
    vcRemoveExtension(root, "extensionsRequired", "KHR_meshopt_compression");

    // the meshes several nodes draw: a painted node gets its own copy
    std::map<size_t, int> meshUsers;
    if (root.contains("nodes")) {
        for (const Json& node : root["nodes"]) {
            size_t mesh = 0;
            if (GltfDocument::getIndex(node, "mesh", mesh)) meshUsers[mesh]++;
        }
    }
    std::map<int, size_t> copiedFor;   // node -> its mesh copy
    for (const vc::TargetData& target : data->targets) {
        const std::string where = target.key.describe();
        if (target.key.node < 0 || !root.contains("nodes") || (size_t)target.key.node >= root["nodes"].size()) {
            return fail(where + " is not a node of the model's file: the file changed since it was painted. Open and save the scene, then export.");
        }
        Json& node = root["nodes"][(size_t)target.key.node];
        size_t mesh = 0;
        if (!GltfDocument::getIndex(node, "mesh", mesh) || mesh >= root["meshes"].size()) return fail(where + " draws no mesh in the model's file.");
        if (meshUsers[mesh] > 1 && !copiedFor.count(target.key.node)) {
            root["meshes"].push_back(root["meshes"][mesh]);
            copiedFor[target.key.node] = root["meshes"].size() - 1;
            node["mesh"] = root["meshes"].size() - 1;
        }
        if (copiedFor.count(target.key.node)) mesh = copiedFor[target.key.node];
        Json& primitives = root["meshes"][mesh]["primitives"];
        if (target.key.primitive < 0 || (size_t)target.key.primitive >= primitives.size()) return fail(where + " is not a primitive of the model's file.");
        Json& primitive = primitives[(size_t)target.key.primitive];

        const std::shared_ptr<const vc::MeshView> view = vc::sceneMeshView(scene, entity, target.key);
        if (!view || view->fingerprint() != target.fingerprint || view->vertexCount() != target.base.size()) {
            return fail(where + " of " + name + " was painted on another version of its mesh: open the scene and save it (the colors are transferred), then export.");
        }
        size_t position = 0;
        if (!GltfDocument::getIndex(primitive["attributes"], "POSITION", position)) return fail(where + " has no POSITION.");
        GltfAccessorLayout layout;
        if (!doc.accessorLayout(position, layout, why) || layout.count != target.base.size()) {
            return fail(where + " has " + std::to_string(layout.count) + " vertices in the file and " + std::to_string(target.base.size()) +
                        " painted: the file changed since it was painted. Open and save the scene, then export.");
        }

        const vc::SplitResult split = options.splitHardEdges && target.hasOverrides() ? vc::buildSplit(target, *view, {}) : vc::SplitResult();
        if (split.split) {
            Json attributes = Json::object();
            for (auto it = primitive["attributes"].begin(); it != primitive["attributes"].end(); ++it) {
                if (it.key() == "COLOR_0") continue;
                size_t created = 0;
                if (!vcRemapAccessor(doc, root, bin, it.value().get<size_t>(), split.sourceVertex, created, why)) return fail(where + ": " + why);
                attributes[it.key()] = created;
            }
            primitive["attributes"] = std::move(attributes);
            if (primitive.contains("targets") && primitive["targets"].is_array()) {
                for (Json& morph : primitive["targets"]) {
                    for (auto it = morph.begin(); it != morph.end(); ++it) {
                        size_t created = 0;
                        if (!vcRemapAccessor(doc, root, bin, it.value().get<size_t>(), split.sourceVertex, created, why)) return fail(where + ": " + why);
                        it.value() = created;
                    }
                }
            }
            primitive["indices"] = vcIndexAccessor(root, bin, split.indices, split.values.size());
            // levels over the old vertices no longer fit
            if (primitive.contains("extensions") && primitive["extensions"].is_object()) primitive["extensions"].erase("JE_mesh_lods");
        }
        primitive["attributes"]["COLOR_0"] = vcColorAccessor(root, bin, split.split ? split.values : target.base, data->format);
    }

    // one buffer, in the GLB; images beside the old file stay reachable from the new one
    root["buffers"] = Json::array({Json{{"byteLength", bin.size()}}});
    if (root.contains("images") && root["images"].is_array()) {
        for (Json& image : root["images"]) {
            if (!image.contains("uri") || !image["uri"].is_string()) continue;
            const std::string uri = image["uri"].get<std::string>();
            if (uri.rfind("data:", 0) == 0) continue;
            const fs::path file = (source.parent_path() / FileUtils::pathFromUtf8(uri)).lexically_normal();
            image["uri"] = file.lexically_relative(destination.parent_path()).generic_string();
        }
    }
    const std::vector<uint8_t> glb = GltfDocument::writeGlb(root, bin);
    fs::create_directories(destination.parent_path(), ec);
    {
        ScopedExpectedWrite expected(destination);
        const io::WriteResult written = io::writeFileAtomic(destination, std::string_view(reinterpret_cast<const char*>(glb.data()), glb.size()));
        if (!written.ok()) return fail("Could not write '" + relative + "': " + io::describeWriteError(written));
    }
    AssetDatabase& database = project->getAssetDatabase();
    if (database.getAssetGuid(destination).isNull()) project->onAssetFilesChanged(database.importAssets(destination));

    if (options.replaceInScene) {
        // the new file in the Model's place and the paint it carries off the GameObject: one step
        CommandHistory* history = CommandHandle::get(sceneId);
        const size_t mark = history->mark();
        LatticeBakeCmd::ModelChange change;
        change.entity = entity;
        change.filename = relative;
        if (!history->addCommandNoMerge(new LatticeBakeCmd(project, sceneId, {change}, {}))) {
            return fail("'" + relative + "' was written, but the Model could not load it; it stays on disk.");
        }
        history->addCommandNoMerge(new RemoveComponentCmd(project, sceneId, entity, ComponentType::VertexColorsComponent));
        history->groupSince(mark, "Export as Model (" + name + ")");
    }
    return true;
}
