// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelCooker.h"

#include "model/GltfDocument.h"
#include "model/ModelSourceDecode.h"

#include "pool/MeshLodPool.h"
#include "util/FileUtils.h"

#include "meshoptimizer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    using Json = GltfDocument::Json;

    // the vertex codec version KHR_meshopt_compression allows (EXT_ allows 0 only)
    constexpr int kVertexCodecVersion = 1;
    // what a prebuilt player holds per mesh (MAX_EXTERNAL_BUFFERS): the cook interleaves a mesh's
    // streams rather than split it into more buffers than that, or than the source had
    constexpr size_t kPlayerMeshBuffers = 30;
    // the extensions on a primitive whose data the cook knows how to keep
    const char* const KEPT_PRIMITIVE_EXTENSIONS[] = {"KHR_materials_variants", "JE_mesh_lods"};
    const char* const MESHOPT_NAMES[] = {"KHR_meshopt_compression", "EXT_meshopt_compression"};

    size_t align4(size_t value){ return (value + 3) & ~size_t(3); }

    std::string meshLabel(const Json& root, size_t mesh, size_t primitive){
        std::string name;
        if (const Json* meshes = GltfDocument::arrayOf(root, "meshes"); meshes && mesh < meshes->size())
            name = (*meshes)[mesh].value("name", std::string());
        if (name.empty())
            name = "mesh " + std::to_string(mesh);
        return name + "/" + std::to_string(primitive);
    }

    float componentValue(const uint8_t* p, int componentType, bool normalized){
        switch (componentType){
            case gltf::BYTE: { int8_t v; std::memcpy(&v, p, 1); return normalized ? std::max((float)v / 127.0f, -1.0f) : (float)v; }
            case gltf::UNSIGNED_BYTE: return normalized ? (float)*p / 255.0f : (float)*p;
            case gltf::SHORT: { int16_t v; std::memcpy(&v, p, 2); return normalized ? std::max((float)v / 32767.0f, -1.0f) : (float)v; }
            case gltf::UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, p, 2); return normalized ? (float)v / 65535.0f : (float)v; }
            case gltf::UNSIGNED_INT: { uint32_t v; std::memcpy(&v, p, 4); return (float)v; }
            default: { float v; std::memcpy(&v, p, 4); return v; }
        }
    }

    std::vector<float> floatsOf(const std::vector<uint8_t>& data, const GltfAccessorLayout& layout){
        const size_t size = (size_t)gltf::componentSize(layout.componentType);
        std::vector<float> out(layout.count * (size_t)layout.components);
        for (size_t i = 0; i < out.size(); i++)
            out[i] = componentValue(data.data() + i * size, layout.componentType, layout.normalized);
        return out;
    }

    // a JSON member that names a bufferView from somewhere the cook does not rewrite
    std::string foreignViewReference(const Json& node, const std::string& path){
        if (node.is_object()){
            for (auto it = node.begin(); it != node.end(); ++it){
                const std::string child = path + "/" + it.key();
                // the members the cook rewrites itself
                if (path.empty() && (it.key() == "bufferViews" || it.key() == "buffers" || it.key() == "accessors"))
                    continue;
                if (it.key() == "bufferView" && path.rfind("/images/", 0) == 0 && std::count(path.begin(), path.end(), '/') == 2)
                    continue;
                if (it.key() == "bufferView")
                    return child;
                const std::string inner = foreignViewReference(*it, child);
                if (!inner.empty())
                    return inner;
            }
        }else if (node.is_array()){
            for (size_t i = 0; i < node.size(); i++){
                const std::string inner = foreignViewReference(node[i], path + "/" + std::to_string(i));
                if (!inner.empty())
                    return inner;
            }
        }
        return {};
    }

    // what the model's order invariant (R9) compares: the node graph, each mesh's primitives
    // and their materials, materials, skins, animations and scenes
    std::string structureSignature(const Json& root){
        std::string out;
        auto sizeOf = [&](const char* key){
            const Json* list = GltfDocument::arrayOf(root, key);
            return list ? list->size() : 0;
        };
        out += "nodes " + std::to_string(sizeOf("nodes")) + ";";
        if (const Json* nodes = GltfDocument::arrayOf(root, "nodes")){
            for (const Json& node : *nodes){
                out += node.value("name", std::string()) + "|" + (node.contains("mesh") ? node["mesh"].dump() : "-") + "|" +
                       (node.contains("skin") ? node["skin"].dump() : "-") + "|" + (node.contains("children") ? node["children"].dump() : "-") + ";";
            }
        }
        out += "meshes " + std::to_string(sizeOf("meshes")) + ";";
        if (const Json* meshes = GltfDocument::arrayOf(root, "meshes")){
            for (const Json& mesh : *meshes){
                const Json primitives = mesh.value("primitives", Json::array());
                out += std::to_string(primitives.size()) + ":";
                for (const Json& primitive : primitives)
                    out += (primitive.contains("material") ? primitive["material"].dump() : "-") + "," +
                           std::to_string(primitive.value("targets", Json::array()).size()) + ",";
                out += ";";
            }
        }
        out += "materials " + std::to_string(sizeOf("materials")) + ";skins " + std::to_string(sizeOf("skins")) + ";";
        out += "animations " + std::to_string(sizeOf("animations")) + ";";
        if (const Json* animations = GltfDocument::arrayOf(root, "animations")){
            for (const Json& animation : *animations)
                out += animation.value("name", std::string()) + ":" + std::to_string(animation.value("channels", Json::array()).size()) +
                       "," + std::to_string(animation.value("samplers", Json::array()).size()) + ";";
        }
        out += "scenes " + std::to_string(sizeOf("scenes")) + (root.contains("scene") ? root["scene"].dump() : "-");
        return out;
    }

    void removeExtension(Json& root, const char* key, const std::string& name){
        auto it = root.find(key);
        if (it == root.end() || !it->is_array())
            return;
        Json kept = Json::array();
        for (const Json& item : *it)
            if (!(item.is_string() && item.get<std::string>() == name))
                kept.push_back(item);
        if (kept.empty())
            root.erase(key);
        else
            *it = kept;
    }

    void addExtension(Json& root, const char* key, const std::string& name){
        if (!root.contains(key) || !root[key].is_array())
            root[key] = Json::array();
        for (const Json& item : root[key])
            if (item.is_string() && item.get<std::string>() == name)
                return;
        root[key].push_back(name);
    }

    struct Primitive {
        size_t mesh = 0, index = 0;
        int mode = 4;
        std::optional<size_t> indices;
        std::vector<std::pair<std::string, size_t>> attributes;     // sorted by semantic
        std::vector<std::vector<std::pair<std::string, size_t>>> targets;
        bool blended = false;
        std::string foreignExtension;
        int group = -1;
        // the cook's
        std::vector<uint32_t> list;                 // LOD 0's indices, when indexed
        bool indexed = false;
        bool newIndices = false;                    // indices made by the cook (an unindexed source)
        size_t indexAccessor = 0;
        int indexType = gltf::UNSIGNED_SHORT;
        std::vector<std::pair<std::vector<uint32_t>, float>> lods;  // indices, error
        std::vector<double> lodDeviation;
        std::vector<float> lodCoverage;             // MSFT_screencoverage of a level, -1 without
        // MSFT_lod's lower levels folded into this primitive's vertices (R4): their indices over
        // them, and the screen coverage each switches at (-1 when the file gives none)
        std::vector<std::vector<uint32_t>> authored;
        std::vector<float> authoredCoverage;
        // MSFT_lod's levels whose attributes differ: their triangle counts, the generated levels' targets
        std::vector<long long> lodTargets;
    };

    struct Stream {
        std::string semantic;
        int target = -1;                // -1: the primitive's own attribute
        size_t accessor = 0;
        GltfAccessorLayout layout;
        std::vector<uint8_t> data;      // count * elementSize, as it will be stored (before a filter)
        std::string filter = "NONE";
        std::vector<uint8_t> encoded;   // the filter's encoding (the codec's input) when filtered
        bool quantized = false;         // a lossy level changed its values
        bool newAccessor = false;       // made by the cook (Normals Calculate)
    };

    struct Group {
        std::vector<size_t> primitives;
        std::vector<Stream> streams;
        size_t vertexCount = 0;
        bool locked = false;
        std::string lockReason;
    };

    // one output bufferView
    struct View {
        enum Kind { Vertex, Index, Lod, Other, Image } kind = Other;
        std::vector<size_t> accessors;  // in it, with their byte offsets
        std::vector<size_t> offsets;
        size_t stride = 0;              // element stride (vertex and other views)
        int target = 0;
        std::string filter = "NONE";
        std::vector<uint8_t> data;      // what the view holds once decoded
        std::vector<uint8_t> codecInput;// what the codec encodes (the filter's encoding), when it differs
        size_t count = 0;               // elements (vertex, other) or indices (index, lod)
        bool triangles = true;
        size_t mesh = SIZE_MAX;         // the mesh whose draws read it
    };

    class Cook {
    public:
        Cook(const ModelCookRequest& request, ModelCookRecord& record, const std::function<bool()>& cancelled):
            request(request), settings(request.settings), record(record), cancelled(cancelled) {}

        bool run(std::vector<uint8_t>& glb, std::string& error);

    private:
        const ModelCookRequest& request;
        const ResolvedModelSettings& settings;
        ModelCookRecord& record;
        const std::function<bool()>& cancelled;
        GltfDocument doc;
        std::vector<Primitive> primitives;
        std::vector<Group> groups;
        // per accessor: its uses
        std::vector<std::set<int>> vertexGroups;    // the groups reading it as a vertex stream
        std::vector<bool> indexUse;
        std::vector<bool> otherUse;
        std::vector<int> indexGroup;                // the group whose primitives read it as indices (-2: several)
        size_t sourceAccessors = 0;
        long long dedupeRemoved = 0;
        long long dedupeBefore = 0;
        // folded and target levels by (mesh, primitive), until collect() makes the primitives
        std::map<std::pair<size_t, size_t>, std::vector<std::pair<std::vector<uint32_t>, float>>> foldedLevels;
        std::map<std::pair<size_t, size_t>, std::vector<long long>> targetLevels;

        bool isCancelled() const { return cancelled && cancelled(); }
        bool collect(std::string& error);
        void buildGroups();
        bool readGroup(Group& group, std::string& error);
        bool processGroup(Group& group, std::string& error);
        void applyRemap(Group& group, std::vector<std::vector<uint32_t>*>& lists, const std::vector<uint32_t>& remap, size_t count);
        bool finishIndices(std::string& error);
        void quantize(Group& group);
        void settleIndexRotation(Group& group);
        bool foldAuthoredLods(const std::vector<GltfLodChain>& chains, std::string& error);
        void buildLods(Group& group);
        std::vector<float> positionsOf(const Group& group) const;
        ModelGeometryStats sourceStats(std::string& error);
        ModelGeometryStats outputStats(const std::vector<View>& views);
        bool assemble(std::vector<uint8_t>& bin, std::vector<View>& views, std::string& error);
        size_t addAccessor(Json accessor);
    };

    bool Cook::collect(std::string& error){
        Json& root = doc.json();
        const Json* accessors = GltfDocument::arrayOf(root, "accessors");
        sourceAccessors = accessors ? accessors->size() : 0;
        vertexGroups.assign(sourceAccessors, {});
        indexUse.assign(sourceAccessors, false);
        otherUse.assign(sourceAccessors, false);
        indexGroup.assign(sourceAccessors, -1);
        auto checkAccessor = [&](const Json& object, const char* key, const std::string& where, std::optional<size_t>& out) -> bool {
            if (!object.contains(key))
                return true;
            size_t value = 0;
            if (!GltfDocument::getIndex(object, key, value) || value >= sourceAccessors){
                error = where + " names an accessor that does not exist";
                return false;
            }
            out = value;
            return true;
        };
        // what the materials draw blended
        std::set<size_t> blendedMaterials;
        if (const Json* materials = GltfDocument::arrayOf(root, "materials"))
            for (size_t m = 0; m < materials->size(); m++)
                if ((*materials)[m].value("alphaMode", std::string()) == "BLEND")
                    blendedMaterials.insert(m);
        if (const Json* meshes = GltfDocument::arrayOf(root, "meshes")){
            for (size_t m = 0; m < meshes->size(); m++){
                const Json& mesh = (*meshes)[m];
                if (!mesh.contains("primitives") || !mesh["primitives"].is_array())
                    continue;
                for (size_t p = 0; p < mesh["primitives"].size(); p++){
                    const Json& primitive = mesh["primitives"][p];
                    const std::string where = request.assetPath + " " + meshLabel(root, m, p);
                    Primitive info;
                    info.mesh = m;
                    info.index = p;
                    info.mode = (int)primitive.value("mode", 4);
                    if (!checkAccessor(primitive, "indices", where, info.indices))
                        return false;
                    if (!primitive.contains("attributes") || !primitive["attributes"].is_object()){
                        error = where + " has no attributes";
                        return false;
                    }
                    for (auto it = primitive["attributes"].begin(); it != primitive["attributes"].end(); ++it){
                        std::optional<size_t> accessor;
                        if (!checkAccessor(primitive["attributes"], it.key().c_str(), where, accessor) || !accessor){
                            if (error.empty()) error = where + "'s " + it.key() + " is not an accessor";
                            return false;
                        }
                        info.attributes.push_back({it.key(), *accessor});
                    }
                    std::sort(info.attributes.begin(), info.attributes.end());
                    if (primitive.contains("targets") && primitive["targets"].is_array()){
                        for (const Json& target : primitive["targets"]){
                            std::vector<std::pair<std::string, size_t>> streams;
                            if (target.is_object()){
                                for (auto it = target.begin(); it != target.end(); ++it){
                                    std::optional<size_t> accessor;
                                    if (!checkAccessor(target, it.key().c_str(), where, accessor) || !accessor){
                                        if (error.empty()) error = where + "'s morph target " + it.key() + " is not an accessor";
                                        return false;
                                    }
                                    streams.push_back({it.key(), *accessor});
                                }
                            }
                            std::sort(streams.begin(), streams.end());
                            info.targets.push_back(std::move(streams));
                        }
                    }
                    size_t material = 0;
                    info.blended = (GltfDocument::getIndex(primitive, "material", material) && blendedMaterials.count(material)) ||
                                   request.blended.count({(int)m, (int)p}) || request.blended.count({-1, (int)p});
                    if (primitive.contains("extensions") && primitive["extensions"].is_object()){
                        for (auto it = primitive["extensions"].begin(); it != primitive["extensions"].end(); ++it){
                            if (it.key() == "KHR_draco_mesh_compression"){
                                // takeDraco moved every decoded stream: one left was not decoded
                                error = request.assetPath + " " + meshLabel(root, m, p) + ": its Draco stream was not decoded";
                                return false;
                            }
                            if (std::find_if(std::begin(KEPT_PRIMITIVE_EXTENSIONS), std::end(KEPT_PRIMITIVE_EXTENSIONS),
                                             [&](const char* name){ return it.key() == name; }) == std::end(KEPT_PRIMITIVE_EXTENSIONS))
                                info.foreignExtension = it.key();
                        }
                    }
                    const auto folded = foldedLevels.find({m, p});
                    if (folded != foldedLevels.end()){
                        for (auto& [list, coverage] : folded->second){
                            info.authored.push_back(std::move(list));
                            info.authoredCoverage.push_back(coverage);
                        }
                    }
                    const auto targets = targetLevels.find({m, p});
                    if (targets != targetLevels.end())
                        info.lodTargets = targets->second;
                    primitives.push_back(std::move(info));
                }
            }
        }
        // the other users of accessors: skins, animations, instancing
        auto markOther = [&](const Json& object, const char* key){
            size_t value = 0;
            if (GltfDocument::getIndex(object, key, value) && value < sourceAccessors)
                otherUse[value] = true;
        };
        if (const Json* skins = GltfDocument::arrayOf(root, "skins"))
            for (const Json& skin : *skins)
                markOther(skin, "inverseBindMatrices");
        if (const Json* animations = GltfDocument::arrayOf(root, "animations"))
            for (const Json& animation : *animations)
                if (animation.contains("samplers") && animation["samplers"].is_array())
                    for (const Json& sampler : animation["samplers"]){
                        markOther(sampler, "input");
                        markOther(sampler, "output");
                    }
        if (const Json* nodes = GltfDocument::arrayOf(root, "nodes"))
            for (const Json& node : *nodes)
                if (node.contains("extensions") && node["extensions"].is_object() && node["extensions"].contains("EXT_mesh_gpu_instancing")){
                    const Json& instancing = node["extensions"]["EXT_mesh_gpu_instancing"];
                    if (instancing.contains("attributes") && instancing["attributes"].is_object())
                        for (auto it = instancing["attributes"].begin(); it != instancing["attributes"].end(); ++it){
                            size_t value = 0;
                            if (it->is_number() && GltfDocument::getIndex(instancing["attributes"], it.key().c_str(), value) && value < sourceAccessors)
                                otherUse[value] = true;
                        }
                }
        // a previous cook's levels of a cooked source: kept as they were
        for (const Primitive& primitive : primitives){
            const Json& json = root["meshes"][primitive.mesh]["primitives"][primitive.index];
            if (json.contains("extensions") && json["extensions"].contains("JE_mesh_lods") && json["extensions"]["JE_mesh_lods"].contains("levels"))
                for (const Json& level : json["extensions"]["JE_mesh_lods"]["levels"])
                    markOther(level, "indices");
        }
        return true;
    }

    void Cook::buildGroups(){
        std::map<std::string, int> byKey;
        for (size_t i = 0; i < primitives.size(); i++){
            Primitive& primitive = primitives[i];
            std::string key;
            for (const auto& [semantic, accessor] : primitive.attributes)
                key += semantic + "=" + std::to_string(accessor) + ";";
            for (size_t t = 0; t < primitive.targets.size(); t++)
                for (const auto& [semantic, accessor] : primitive.targets[t])
                    key += "t" + std::to_string(t) + ":" + semantic + "=" + std::to_string(accessor) + ";";
            auto [it, added] = byKey.emplace(key, (int)groups.size());
            if (added){
                Group group;
                for (const auto& [semantic, accessor] : primitive.attributes){
                    Stream stream;
                    stream.semantic = semantic;
                    stream.accessor = accessor;
                    group.streams.push_back(stream);
                }
                for (size_t t = 0; t < primitive.targets.size(); t++){
                    for (const auto& [semantic, accessor] : primitive.targets[t]){
                        Stream stream;
                        stream.semantic = semantic;
                        stream.target = (int)t;
                        stream.accessor = accessor;
                        group.streams.push_back(stream);
                    }
                }
                groups.push_back(std::move(group));
            }
            primitive.group = it->second;
            groups[(size_t)it->second].primitives.push_back(i);
        }
        for (size_t g = 0; g < groups.size(); g++){
            for (const Stream& stream : groups[g].streams)
                vertexGroups[stream.accessor].insert((int)g);
            for (size_t p : groups[g].primitives){
                if (!primitives[p].indices)
                    continue;
                const size_t accessor = *primitives[p].indices;
                indexUse[accessor] = true;
                indexGroup[accessor] = indexGroup[accessor] == -1 || indexGroup[accessor] == (int)g ? (int)g : -2;
            }
        }
        // a group whose data has other users keeps its vertex order (and its triangle order: an
        // index list shared with another group cannot follow this one's vertices)
        for (size_t g = 0; g < groups.size(); g++){
            Group& group = groups[g];
            auto lock = [&](const std::string& why){
                if (!group.locked){
                    group.locked = true;
                    group.lockReason = why;
                }
            };
            for (const Stream& stream : group.streams){
                if (vertexGroups[stream.accessor].size() > 1) lock("its vertices are shared with other primitives");
                if (otherUse[stream.accessor] || indexUse[stream.accessor]) lock("its vertices are read by something else");
            }
            for (size_t p : group.primitives){
                const Primitive& primitive = primitives[p];
                if (primitive.mode != 4) lock("it is not a triangle list");
                if (!primitive.foreignExtension.empty()) lock("it has the extension " + primitive.foreignExtension);
                if (primitive.indices && (indexGroup[*primitive.indices] == -2 || otherUse[*primitive.indices] ||
                                          !vertexGroups[*primitive.indices].empty()))
                    lock("its indices are shared with other primitives");
            }
            bool hasPosition = false;
            for (const Stream& stream : group.streams)
                hasPosition = hasPosition || (stream.semantic == "POSITION" && stream.target < 0);
            if (!hasPosition) lock("it has no POSITION");
        }
    }

    bool Cook::readGroup(Group& group, std::string& error){
        group.vertexCount = 0;
        for (Stream& stream : group.streams){
            if (!doc.readAccessorRaw(stream.accessor, stream.data, stream.layout, error))
                return false;
            if (stream.layout.elementSize > 256){
                error = "accessor " + std::to_string(stream.accessor) + " of " + request.assetPath + " is not a vertex attribute";
                return false;
            }
            if (stream.target < 0 && stream.semantic == "POSITION")
                group.vertexCount = stream.layout.count;
        }
        if (group.vertexCount == 0 && !group.streams.empty())
            group.vertexCount = group.streams[0].layout.count;
        for (const Stream& stream : group.streams){
            if (stream.layout.count != group.vertexCount && !group.locked){
                group.locked = true;
                group.lockReason = "its attributes have different counts";
            }
        }
        return true;
    }

    void Cook::applyRemap(Group& group, std::vector<std::vector<uint32_t>*>& lists, const std::vector<uint32_t>& remap, size_t count){
        for (Stream& stream : group.streams){
            std::vector<uint8_t> out(count * stream.layout.elementSize);
            meshopt_remapVertexBuffer(out.data(), stream.data.data(), group.vertexCount, stream.layout.elementSize, remap.data());
            stream.data.swap(out);
            stream.layout.count = count;
        }
        for (std::vector<uint32_t>* list : lists)
            meshopt_remapIndexBuffer(list->data(), list->data(), list->size(), remap.data());
        group.vertexCount = count;
    }

    std::vector<float> Cook::positionsOf(const Group& group) const{
        for (const Stream& stream : group.streams){
            if (stream.semantic != "POSITION" || stream.target >= 0 || stream.layout.components != 3)
                continue;
            if (stream.filter == "EXPONENTIAL"){
                // what the player decodes
                std::vector<uint8_t> decoded = stream.encoded;
                meshopt_decodeFilterExp(decoded.data(), stream.layout.count, 12);
                std::vector<float> out(stream.layout.count * 3);
                std::memcpy(out.data(), decoded.data(), out.size() * sizeof(float));
                return out;
            }
            return floatsOf(stream.data, stream.layout);
        }
        return {};
    }

    bool Cook::processGroup(Group& group, std::string& error){
        std::vector<std::vector<uint32_t>> lists(group.primitives.size());
        for (size_t i = 0; i < group.primitives.size(); i++){
            Primitive& primitive = primitives[group.primitives[i]];
            if (primitive.indices){
                if (!doc.readIndices(*primitive.indices, lists[i], error))
                    return false;
                for (uint32_t index : lists[i]){
                    if (index >= group.vertexCount){
                        error = request.assetPath + " " + meshLabel(doc.json(), primitive.mesh, primitive.index) + " has an index past its vertices";
                        return false;
                    }
                }
            }else{
                lists[i].resize(group.vertexCount);
                for (size_t v = 0; v < group.vertexCount; v++)
                    lists[i][v] = (uint32_t)v;
            }
            if (lists[i].size() % 3 != 0){
                group.locked = true;
                group.lockReason = "its index count is not a multiple of 3";
                return true;
            }
        }
        // the folded MSFT_lod levels follow every pass with LOD 0, after it (its vertices first)
        std::vector<std::pair<size_t, size_t>> levelOf;     // lists past the primitives': (primitive, level)
        for (size_t i = 0; i < group.primitives.size(); i++){
            Primitive& primitive = primitives[group.primitives[i]];
            for (size_t l = 0; l < primitive.authored.size(); l++){
                lists.push_back(primitive.authored[l]);
                levelOf.push_back({i, l});
            }
        }
        auto ownerOf = [&](size_t list){ return list < group.primitives.size() ? list : levelOf[list - group.primitives.size()].first; };
        const bool reorder = settings.optimizeMesh != OptimizeMesh::Nothing || settings.normals == ModelNormals::Calculate;
        std::vector<std::vector<uint32_t>*> listRefs;
        for (std::vector<uint32_t>& list : lists)
            listRefs.push_back(&list);
        auto combined = [&](){
            std::vector<uint32_t> all;
            for (const std::vector<uint32_t>& list : lists)
                all.insert(all.end(), list.begin(), list.end());
            return all;
        };
        auto streamsOf = [&](){
            std::vector<meshopt_Stream> streams;
            for (const Stream& stream : group.streams)
                streams.push_back({stream.data.data(), stream.layout.elementSize, stream.layout.elementSize});
            return streams;
        };

        // Normals: Calculate, per corner across the creases, then indexed again over every stream
        if (settings.normals == ModelNormals::Calculate){
            const std::vector<float> positions = positionsOf(group);
            if (positions.empty()){
                record.warnings.push_back(meshLabel(doc.json(), primitives[group.primitives[0]].mesh, primitives[group.primitives[0]].index) +
                                          " kept its normals: its positions are not three floats");
            }else{
                const std::vector<uint32_t> all = combined();
                std::vector<float> normals(all.size() * 3);
                meshopt_generateNormals(normals.data(), all.data(), all.size(), positions.data(), group.vertexCount, sizeof(float) * 3,
                                        settings.smoothingAngle * 3.14159265358979f / 180.0f, 0.0f);
                // every stream per corner
                for (Stream& stream : group.streams){
                    std::vector<uint8_t> corners(all.size() * stream.layout.elementSize);
                    for (size_t c = 0; c < all.size(); c++)
                        std::memcpy(corners.data() + c * stream.layout.elementSize, stream.data.data() + (size_t)all[c] * stream.layout.elementSize,
                                    stream.layout.elementSize);
                    stream.data.swap(corners);
                    stream.layout.count = all.size();
                }
                auto normal = std::find_if(group.streams.begin(), group.streams.end(), [](const Stream& s){ return s.semantic == "NORMAL" && s.target < 0; });
                if (normal == group.streams.end()){
                    Stream stream;
                    stream.semantic = "NORMAL";
                    stream.newAccessor = true;
                    group.streams.push_back(stream);
                    normal = group.streams.end() - 1;
                }
                normal->layout.componentType = gltf::FLOAT;
                normal->layout.components = 3;
                normal->layout.normalized = false;
                normal->layout.count = all.size();
                normal->layout.elementSize = 12;
                normal->data.resize(normals.size() * sizeof(float));
                std::memcpy(normal->data.data(), normals.data(), normal->data.size());
                group.vertexCount = all.size();
                size_t at = 0;
                for (std::vector<uint32_t>& list : lists){
                    for (uint32_t& index : list)
                        index = (uint32_t)at++;
                }
                const std::vector<uint32_t> identity = combined();
                std::vector<uint32_t> remap(group.vertexCount);
                const std::vector<meshopt_Stream> streams = streamsOf();
                const size_t unique = meshopt_generateVertexRemapMulti(remap.data(), identity.data(), identity.size(), group.vertexCount,
                                                                       streams.data(), streams.size());
                applyRemap(group, listRefs, remap, unique);
            }
        }
        if (isCancelled())
            return true;

        // Everything: exact duplicates welded
        if (settings.optimizeMesh == OptimizeMesh::Everything && settings.weldVertices){
            const std::vector<uint32_t> all = combined();
            std::vector<uint32_t> remap(group.vertexCount);
            const std::vector<meshopt_Stream> streams = streamsOf();
            const size_t before = group.vertexCount;
            const size_t unique = meshopt_generateVertexRemapMulti(remap.data(), all.data(), all.size(), group.vertexCount,
                                                                   streams.data(), streams.size());
            dedupeBefore += (long long)before;
            dedupeRemoved += (long long)before - (long long)unique;
            applyRemap(group, listRefs, remap, unique);
        }

        // Everything, Polygon Order: the vertex cache's and overdraw's triangle order (never on a
        // primitive drawn blended, never one worse for the cache than the source's)
        if (settings.optimizeMesh == OptimizeMesh::Everything || settings.optimizeMesh == OptimizeMesh::PolygonOrder){
            const std::vector<float> positions = positionsOf(group);
            for (size_t i = 0; i < lists.size(); i++){
                Primitive& primitive = primitives[group.primitives[ownerOf(i)]];
                if (primitive.blended){
                    if (i < group.primitives.size())
                        record.keptOrder.push_back(meshLabel(doc.json(), primitive.mesh, primitive.index) + " (blended)");
                    continue;
                }
                std::vector<uint32_t>& list = lists[i];
                if (list.empty())
                    continue;
                std::vector<uint32_t> optimized(list.size());
                meshopt_optimizeVertexCache(optimized.data(), list.data(), list.size(), group.vertexCount);
                if (!positions.empty()){
                    std::vector<uint32_t> overdraw(list.size());
                    meshopt_optimizeOverdraw(overdraw.data(), optimized.data(), optimized.size(), positions.data(), group.vertexCount,
                                             sizeof(float) * 3, 1.05f);
                    optimized.swap(overdraw);
                }
                const float before = meshopt_analyzeVertexCache(list.data(), list.size(), group.vertexCount, 16, 0, 0).acmr;
                const float after = meshopt_analyzeVertexCache(optimized.data(), optimized.size(), group.vertexCount, 16, 0, 0).acmr;
                if (after <= before)
                    list.swap(optimized);
            }
        }
        if (isCancelled())
            return true;

        // Everything, Vertex Order: the fetch order
        if (settings.optimizeMesh == OptimizeMesh::Everything || settings.optimizeMesh == OptimizeMesh::VertexOrder){
            const std::vector<uint32_t> all = combined();
            std::vector<uint32_t> remap(group.vertexCount);
            const size_t unique = meshopt_optimizeVertexFetchRemap(remap.data(), all.data(), all.size(), group.vertexCount);
            applyRemap(group, listRefs, remap, unique);
        }

        for (size_t i = 0; i < levelOf.size(); i++)
            primitives[group.primitives[levelOf[i].first]].authored[levelOf[i].second] = std::move(lists[group.primitives.size() + i]);
        for (size_t i = 0; i < group.primitives.size(); i++){
            Primitive& primitive = primitives[group.primitives[i]];
            if (!primitive.indices && !reorder)
                continue;
            primitive.list = std::move(lists[i]);
            primitive.indexed = true;
            primitive.newIndices = !primitive.indices;
        }
        return true;
    }

    bool Cook::finishIndices(std::string& error){
        for (Group& group : groups){
            // a locked group's lists, as they are (and the processed ones that kept theirs)
            for (size_t p : group.primitives){
                Primitive& primitive = primitives[p];
                if (primitive.indexed || !primitive.indices)
                    continue;
                if (!doc.readIndices(*primitive.indices, primitive.list, error))
                    return false;
                primitive.indexed = true;
            }
            const bool wide = group.vertexCount > 65535;
            for (size_t p : group.primitives){
                Primitive& primitive = primitives[p];
                if (!primitive.indexed)
                    continue;
                if (settings.indexFormat == IndexFormat::Bits16 && wide){
                    error = request.assetPath + " " + meshLabel(doc.json(), primitive.mesh, primitive.index) + " has " +
                            std::to_string(group.vertexCount) + " vertices, more than 16-bit indices reach (65,535): "
                            "set Index Format to Auto or 32 bit";
                    return false;
                }
                primitive.indexType = settings.indexFormat == IndexFormat::Bits32 || wide ? gltf::UNSIGNED_INT : gltf::UNSIGNED_SHORT;
            }
        }
        return true;
    }

    void Cook::quantize(Group& group){
        const ModelCooker::QuantizationBits bits = ModelCooker::quantizationBits(settings.meshCompression);
        if (bits.position == 0)
            return;
        for (Stream& stream : group.streams){
            if (stream.target >= 0 || stream.layout.componentType != gltf::FLOAT || stream.layout.normalized ||
                    (stream.accessor < sourceAccessors && otherUse[stream.accessor]))
                continue;
            const size_t count = stream.layout.count;
            const std::vector<float> values = floatsOf(stream.data, stream.layout);
            const std::string& semantic = stream.semantic;
            if (semantic == "POSITION" && stream.layout.components == 3){
                stream.encoded.resize(count * 12);
                meshopt_encodeFilterExp(stream.encoded.data(), count, 12, bits.position, values.data(), meshopt_EncodeExpSharedComponent);
                std::vector<uint8_t> decoded = stream.encoded;
                meshopt_decodeFilterExp(decoded.data(), count, 12);
                double worst = 0.0;
                for (size_t v = 0; v < count; v++){
                    float d[3];
                    std::memcpy(d, decoded.data() + v * 12, 12);
                    const double dx = d[0] - values[v * 3], dy = d[1] - values[v * 3 + 1], dz = d[2] - values[v * 3 + 2];
                    worst = std::max(worst, std::sqrt(dx * dx + dy * dy + dz * dz));
                }
                record.positionError = std::max(record.positionError, worst);
                stream.data = decoded;
                stream.filter = "EXPONENTIAL";
                stream.quantized = true;
            }else if ((semantic == "NORMAL" && stream.layout.components == 3) || (semantic == "TANGENT" && stream.layout.components == 4)){
                const size_t stride = bits.normal <= 8 ? 4 : 8;
                std::vector<float> input(count * 4);
                for (size_t v = 0; v < count; v++){
                    const size_t c = (size_t)stream.layout.components;
                    float x = values[v * c], y = values[v * c + 1], z = values[v * c + 2];
                    const float length = std::sqrt(x * x + y * y + z * z);
                    if (length > 0.0f){ x /= length; y /= length; z /= length; }
                    input[v * 4] = x;
                    input[v * 4 + 1] = y;
                    input[v * 4 + 2] = z;
                    input[v * 4 + 3] = c == 4 ? (values[v * c + 3] < 0.0f ? -1.0f : 1.0f) : 0.0f;
                }
                stream.encoded.resize(count * stride);
                meshopt_encodeFilterOct(stream.encoded.data(), count, stride, bits.normal, input.data());
                stream.data = stream.encoded;
                meshopt_decodeFilterOct(stream.data.data(), count, stride);
                stream.layout.componentType = stride == 4 ? gltf::BYTE : gltf::SHORT;
                stream.layout.normalized = true;
                // the filter's elements are four components; a normal reads the first three
                stream.layout.elementSize = stride;
                stream.filter = "OCTAHEDRAL";
                stream.quantized = true;
            }else if (semantic.rfind("TEXCOORD_", 0) == 0 && stream.layout.components == 2){
                const bool unit = std::all_of(values.begin(), values.end(), [](float v){ return v >= 0.0f && v <= 1.0f; });
                if (unit){
                    // N bits of precision in 16-bit normalized integers (core glTF)
                    const double levels = (double)((1 << bits.uv) - 1);
                    stream.data.resize(count * 4);
                    for (size_t i = 0; i < count * 2; i++){
                        const double q = std::round(values[i] * levels);
                        const uint16_t stored = (uint16_t)std::lround(q / levels * 65535.0);
                        std::memcpy(stream.data.data() + i * 2, &stored, 2);
                    }
                    stream.layout.componentType = gltf::UNSIGNED_SHORT;
                    stream.layout.normalized = true;
                    stream.layout.elementSize = 4;
                }else{
                    stream.encoded.resize(count * 8);
                    meshopt_encodeFilterExp(stream.encoded.data(), count, 8, bits.uv, values.data(), meshopt_EncodeExpSharedComponent);
                    stream.data = stream.encoded;
                    meshopt_decodeFilterExp(stream.data.data(), count, 8);
                    stream.filter = "EXPONENTIAL";
                }
                stream.quantized = true;
            }else if (semantic.rfind("COLOR_", 0) == 0 && (stream.layout.components == 3 || stream.layout.components == 4)){
                const size_t c = (size_t)stream.layout.components;
                stream.data.resize(count * c);
                for (size_t i = 0; i < count * c; i++)
                    stream.data[i] = (uint8_t)std::lround(std::clamp(values[i], 0.0f, 1.0f) * 255.0f);
                stream.layout.componentType = gltf::UNSIGNED_BYTE;
                stream.layout.normalized = true;
                stream.layout.elementSize = c;
                stream.quantized = true;
            }
        }
    }

    // The index codec may turn a triangle's corners (its winding kept), and the runtime builds
    // levels from what it decodes: the cook takes the lists through one round trip of the group's
    // index view, as assemble lays it out, before it bakes (a second trip changes nothing)
    void Cook::settleIndexRotation(Group& group){
        if (settings.meshCompression == MeshCompression::Off)
            return;
        std::vector<uint32_t> all;
        std::map<size_t, size_t> offsets;
        std::vector<std::pair<size_t, size_t>> slices;
        for (size_t p : group.primitives){
            const Primitive& primitive = primitives[p];
            if (!primitive.indexed)
                continue;
            if (primitive.mode != 4 || primitive.list.size() % 3 != 0)
                return;
            const size_t key = primitive.indices && !primitive.newIndices ? *primitive.indices : std::numeric_limits<size_t>::max() - p;
            auto [at, added] = offsets.emplace(key, all.size());
            if (added)
                all.insert(all.end(), primitive.list.begin(), primitive.list.end());
            slices.emplace_back(p, at->second);
        }
        if (all.empty())
            return;
        const size_t vertexBound = *std::max_element(all.begin(), all.end()) + 1;
        std::vector<unsigned char> encoded(meshopt_encodeIndexBufferBound(all.size(), vertexBound));
        encoded.resize(meshopt_encodeIndexBuffer(encoded.data(), encoded.size(), all.data(), all.size()));
        std::vector<uint32_t> decoded(all.size());
        if (encoded.empty() || meshopt_decodeIndexBuffer(decoded.data(), decoded.size(), sizeof(uint32_t), encoded.data(), encoded.size()) != 0)
            return;
        for (const auto& [p, offset] : slices){
            std::vector<uint32_t>& list = primitives[p].list;
            std::copy(decoded.begin() + (std::ptrdiff_t)offset, decoded.begin() + (std::ptrdiff_t)(offset + list.size()), list.begin());
        }
    }

    // MSFT_lod (R4): each lower level's primitive whose attributes are the base primitive's (the
    // same semantics and formats, no morph targets) has its vertices appended to the base's and
    // becomes one of its levels; the others give the generated levels' triangle targets. The
    // lower levels' own meshes stay in the file (the loader takes them off the scene).
    bool Cook::foldAuthoredLods(const std::vector<GltfLodChain>& chains, std::string& error){
        if (settings.generateLods == GenerateLods::Off || chains.empty())
            return true;
        Json& root = doc.json();
        if (!GltfDocument::arrayOf(root, "meshes") || !GltfDocument::arrayOf(root, "accessors"))
            return true;
        doc.prepareViews();
        const Json* meshes = GltfDocument::arrayOf(root, "meshes");
        std::set<size_t> foldedMeshes;
        for (const GltfLodChain& chain : chains){
            const size_t base = (size_t)chain.base.mesh;
            if (chain.base.mesh < 0 || base >= meshes->size() || !foldedMeshes.insert(base).second)
                continue;
            const Json& baseMesh = (*meshes)[base];
            if (!baseMesh.contains("primitives") || !baseMesh["primitives"].is_array())
                continue;
            for (size_t p = 0; p < baseMesh["primitives"].size(); p++){
                const Json& primitive = root["meshes"][base]["primitives"][p];
                auto triangles = [&](const Json& object, std::vector<uint32_t>& list, size_t& vertices) -> bool {
                    size_t position = 0;
                    if ((int)object.value("mode", 4) != 4 || !object.contains("attributes") ||
                            !GltfDocument::getIndex(object["attributes"], "POSITION", position))
                        return false;
                    GltfAccessorLayout layout;
                    std::string ignored;
                    if (!doc.accessorLayout(position, layout, ignored))
                        return false;
                    vertices = layout.count;
                    size_t indices = 0;
                    if (GltfDocument::getIndex(object, "indices", indices)){
                        if (!doc.readIndices(indices, list, ignored))
                            return false;
                    }else{
                        list.resize(vertices);
                        for (size_t v = 0; v < vertices; v++)
                            list[v] = (uint32_t)v;
                    }
                    return list.size() % 3 == 0 && std::all_of(list.begin(), list.end(), [&](uint32_t i){ return i < vertices; });
                };
                std::vector<uint32_t> baseList;
                size_t baseVertices = 0;
                if (!triangles(primitive, baseList, baseVertices) || primitive.contains("targets"))
                    continue;
                // the levels that match fold; the rest give targets
                std::vector<std::pair<size_t, float>> matching;     // level mesh, its coverage
                std::vector<long long> targets;
                for (const GltfLodLevel& level : chain.levels){
                    if (level.mesh < 0 || (size_t)level.mesh >= meshes->size())
                        continue;
                    const Json& levelMesh = (*meshes)[(size_t)level.mesh];
                    if (!levelMesh.contains("primitives") || !levelMesh["primitives"].is_array() || p >= levelMesh["primitives"].size())
                        continue;
                    const Json& other = levelMesh["primitives"][p];
                    std::vector<uint32_t> list;
                    size_t vertices = 0;
                    if (!triangles(other, list, vertices))
                        continue;
                    bool same = !other.contains("targets") && other["attributes"].size() == primitive["attributes"].size();
                    for (auto it = primitive["attributes"].begin(); same && it != primitive["attributes"].end(); ++it){
                        size_t mine = 0, theirs = 0;
                        GltfAccessorLayout a, b;
                        std::string ignored;
                        same = it->is_number_unsigned() && GltfDocument::getIndex(other["attributes"], it.key().c_str(), theirs) &&
                               GltfDocument::getIndex(primitive["attributes"], it.key().c_str(), mine) &&
                               doc.accessorLayout(mine, a, ignored) && doc.accessorLayout(theirs, b, ignored) &&
                               a.componentType == b.componentType && a.components == b.components && a.normalized == b.normalized;
                    }
                    if (same)
                        matching.push_back({(size_t)level.mesh, level.screenCoverage});
                    else
                        targets.push_back((long long)list.size() / 3);
                }
                // float positions only: the folded accessor's bounds are recomputed from them
                GltfAccessorLayout position;
                std::string ignored;
                if (!doc.accessorLayout(primitive["attributes"]["POSITION"].get<size_t>(), position, ignored) ||
                        position.componentType != gltf::FLOAT){
                    for (const auto& level : matching){
                        const Json& other = root["meshes"][level.first]["primitives"][p];
                        std::vector<uint32_t> list;
                        size_t count = 0;
                        triangles(other, list, count);
                        targets.push_back((long long)list.size() / 3);
                    }
                    matching.clear();
                }
                if (!targets.empty())
                    targetLevels[{base, p}] = targets;
                if (matching.empty())
                    continue;
                // the base's streams with every matching level's vertices after its own
                std::vector<std::pair<std::vector<uint32_t>, float>> levels;
                std::map<std::string, std::vector<uint8_t>> streams;
                std::map<std::string, GltfAccessorLayout> layouts;
                for (auto it = primitive["attributes"].begin(); it != primitive["attributes"].end(); ++it){
                    if (!doc.readAccessorRaw(it->get<size_t>(), streams[it.key()], layouts[it.key()], error))
                        return false;
                }
                size_t vertices = baseVertices;
                for (const auto& [levelMesh, coverage] : matching){
                    const Json& other = root["meshes"][levelMesh]["primitives"][p];
                    std::vector<uint32_t> list;
                    size_t count = 0;
                    triangles(other, list, count);
                    for (auto it = other["attributes"].begin(); it != other["attributes"].end(); ++it){
                        std::vector<uint8_t> data;
                        GltfAccessorLayout layout;
                        if (!doc.readAccessorRaw(it->get<size_t>(), data, layout, error))
                            return false;
                        if (layout.count != count){
                            error = request.assetPath + " " + meshLabel(root, levelMesh, p) + ": an MSFT_lod level's attributes have different counts";
                            return false;
                        }
                        streams[it.key()].insert(streams[it.key()].end(), data.begin(), data.end());
                    }
                    for (uint32_t& index : list)
                        index += (uint32_t)vertices;
                    vertices += count;
                    levels.push_back({std::move(list), coverage});
                }
                // the base primitive's accessors now hold every level's vertices
                for (auto it = primitive["attributes"].begin(); it != primitive["attributes"].end(); ++it){
                    const size_t accessor = it->get<size_t>();
                    const GltfAccessorLayout& layout = layouts[it.key()];
                    const size_t stride = (layout.elementSize + 3) / 4 * 4;
                    std::vector<uint8_t> bytes(vertices * stride, 0);
                    for (size_t v = 0; v < vertices; v++)
                        std::memcpy(bytes.data() + v * stride, streams[it.key()].data() + v * layout.elementSize, layout.elementSize);
                    const size_t view = doc.addView(std::move(bytes), stride, gltf::ARRAY_BUFFER);
                    Json& json = root["accessors"][accessor];
                    json["bufferView"] = view;
                    json.erase("byteOffset");
                    json.erase("sparse");
                    json["count"] = vertices;
                    if (it.key() == "POSITION" && layout.componentType == gltf::FLOAT && layout.components == 3){
                        const std::vector<uint8_t>& data = streams[it.key()];
                        float lo[3], hi[3];
                        for (int c = 0; c < 3; c++){ lo[c] = std::numeric_limits<float>::max(); hi[c] = -std::numeric_limits<float>::max(); }
                        for (size_t v = 0; v < vertices; v++){
                            float value[3];
                            std::memcpy(value, data.data() + v * 12, 12);
                            for (int c = 0; c < 3; c++){ lo[c] = std::min(lo[c], value[c]); hi[c] = std::max(hi[c], value[c]); }
                        }
                        json["min"] = Json::array({lo[0], lo[1], lo[2]});
                        json["max"] = Json::array({hi[0], hi[1], hi[2]});
                    }
                }
                foldedLevels[{base, p}] = std::move(levels);
            }
        }
        doc.pruneViews();
        return true;
    }

    void Cook::buildLods(Group& group){
        if (settings.generateLods == GenerateLods::Off)
            return;
        MeshLodRecipe recipe = MeshLodRecipe::runtime();
        if (settings.generateLods == GenerateLods::On){
            const MeshLodReduction reduction = settings.lodReduction == LodReduction::PreserveDetail ? MeshLodReduction::PRESERVE_DETAIL
                                               : settings.lodReduction == LodReduction::Smallest ? MeshLodReduction::SMALLEST
                                                                                                 : MeshLodReduction::BALANCED;
            recipe = MeshLodRecipe::forReduction(reduction, (unsigned int)settings.lodCount);
        }
        // the levels the runtime would build from this data (RenderSystem::readLodSource): FLOAT
        // positions, and as floats the normal, first uv set and bone weights it decodes
        std::vector<float> positions;
        const Stream* normal = nullptr;
        const Stream* uv = nullptr;
        const Stream* weights = nullptr;
        for (const Stream& stream : group.streams){
            if (stream.target >= 0)
                continue;
            if (stream.semantic == "POSITION" && stream.layout.components == 3 && stream.layout.componentType == gltf::FLOAT && !stream.layout.normalized)
                positions = positionsOf(group);
            if (stream.semantic == "NORMAL" && stream.layout.components == 3)
                normal = &stream;
            if (stream.semantic == "TEXCOORD_0" && stream.layout.components == 2)
                uv = &stream;
            if (stream.semantic == "WEIGHTS_0" && stream.layout.components == 4)
                weights = &stream;
        }
        if (positions.empty())
            return;
        // element by element: a filtered normal's elements are wider than its components
        auto guide = [&](const Stream* stream){
            std::vector<float> out;
            if (!stream || stream->layout.count != group.vertexCount)
                return out;
            const size_t components = (size_t)stream->layout.components;
            const size_t size = (size_t)gltf::componentSize(stream->layout.componentType);
            out.resize(group.vertexCount * components);
            for (size_t v = 0; v < group.vertexCount; v++)
                for (size_t c = 0; c < components; c++)
                    out[v * components + c] = componentValue(stream->data.data() + v * stream->layout.elementSize + c * size,
                                                             stream->layout.componentType, stream->layout.normalized);
            return out;
        };
        const std::vector<float> normals = guide(normal), uvs = guide(uv), skinWeights = guide(weights);
        for (size_t p : group.primitives){
            Primitive& primitive = primitives[p];
            if (!primitive.indexed || primitive.mode != 4 || primitive.list.size() % 3 != 0)
                continue;
            if (!primitive.authored.empty()){
                // MSFT_lod's levels as the file has them; `error` measured, as the generated ones'
                for (size_t l = 0; l < primitive.authored.size() && l + 1 < (size_t)MAX_MESH_LODS; l++){
                    const double deviation = MeshLodPool::measureDeviation(positions, primitive.list, primitive.authored[l]);
                    primitive.lodDeviation.push_back(deviation);
                    primitive.lods.push_back({primitive.authored[l], (float)deviation});
                    primitive.lodCoverage.push_back(l < primitive.authoredCoverage.size() ? primitive.authoredCoverage[l] : -1.0f);
                }
                record.authoredLodPrimitives++;
                continue;
            }
            MeshLodRecipe levelRecipe = recipe;
            if (!primitive.lodTargets.empty() && primitive.list.size() >= 3){
                // as many levels as MSFT_lod's, reaching its coarsest triangle count
                const double tris = (double)(primitive.list.size() / 3);
                const double last = std::max(1.0, (double)primitive.lodTargets.back());
                levelRecipe.levels = (unsigned int)std::clamp((int)primitive.lodTargets.size(), 1, MAX_MESH_LODS - 1);
                levelRecipe.ratio = (float)std::clamp(std::pow(last / tris, 1.0 / levelRecipe.levels), 0.05, 0.95);
                levelRecipe.targetError = 1.0f;     // the count decides, not an error budget
            }
            MeshLodSource source;
            source.positions = positions;
            source.indices = primitive.list;
            source.indexType = primitive.indexType == gltf::UNSIGNED_INT ? AttributeDataType::UNSIGNED_INT : AttributeDataType::UNSIGNED_SHORT;
            source.normals = normals;
            source.uvs = uvs;
            source.skinWeights = skinWeights;
            const std::shared_ptr<MeshLodData> data = MeshLodPool::buildLevels(source, levelRecipe);
            if (!data || data->numLevels < 2)
                continue;
            const size_t indexSize = primitive.indexType == gltf::UNSIGNED_INT ? 4 : 2;
            for (unsigned int l = 1; l < data->numLevels; l++){
                const MeshLodLevel& level = data->levels[l];
                std::vector<uint32_t> list(level.indexCount);
                for (unsigned int i = 0; i < level.indexCount; i++){
                    const size_t at = ((size_t)level.indexBase + i) * indexSize;
                    if (indexSize == 4){
                        std::memcpy(&list[i], data->indices.data() + at, 4);
                    }else{
                        uint16_t value;
                        std::memcpy(&value, data->indices.data() + at, 2);
                        list[i] = value;
                    }
                }
                primitive.lodDeviation.push_back((double)level.error);
                primitive.lods.push_back({std::move(list), level.error});
                primitive.lodCoverage.push_back(-1.0f);
            }
        }
    }

    size_t Cook::addAccessor(Json accessor){
        Json& root = doc.json();
        if (!root.contains("accessors") || !root["accessors"].is_array())
            root["accessors"] = Json::array();
        root["accessors"].push_back(std::move(accessor));
        return root["accessors"].size() - 1;
    }

    ModelGeometryStats Cook::sourceStats(std::string& error){
        ModelGeometryStats stats;
        std::set<size_t> vertexAccessors, indexAccessors, positionAccessors;
        double transformed = 0.0, shaded = 0.0, covered = 0.0;
        for (const Primitive& primitive : primitives){
            for (const auto& [semantic, accessor] : primitive.attributes){
                vertexAccessors.insert(accessor);
                if (semantic == "POSITION") positionAccessors.insert(accessor);
            }
            for (const auto& target : primitive.targets)
                for (const auto& stream : target)
                    vertexAccessors.insert(stream.second);
            if (primitive.indices) indexAccessors.insert(*primitive.indices);
            std::vector<uint32_t> list;
            size_t vertexCount = 0;
            GltfAccessorLayout layout;
            for (const auto& [semantic, accessor] : primitive.attributes)
                if (semantic == "POSITION" && doc.accessorLayout(accessor, layout, error))
                    vertexCount = layout.count;
            if (primitive.indices){
                if (!doc.readIndices(*primitive.indices, list, error))
                    return stats;
            }else{
                list.resize(vertexCount);
                for (size_t v = 0; v < vertexCount; v++) list[v] = (uint32_t)v;
            }
            if (primitive.mode != 4 || list.size() < 3)
                continue;
            list.resize(list.size() / 3 * 3);
            stats.triangles += (long long)list.size() / 3;
            if (std::any_of(list.begin(), list.end(), [&](uint32_t i){ return i >= vertexCount; }))
                continue;
            transformed += meshopt_analyzeVertexCache(list.data(), list.size(), vertexCount, 16, 0, 0).vertices_transformed;
            for (const auto& [semantic, accessor] : primitive.attributes){
                if (semantic != "POSITION")
                    continue;
                std::vector<float> positions;
                int components = 0;
                if (doc.readAccessorFloats(accessor, positions, components, error) && components == 3){
                    const meshopt_OverdrawStatistics o = meshopt_analyzeOverdraw(list.data(), list.size(), positions.data(), vertexCount, 12);
                    shaded += o.pixels_shaded;
                    covered += o.pixels_covered;
                }
                error.clear();
            }
        }
        for (size_t accessor : positionAccessors){
            GltfAccessorLayout layout;
            if (doc.accessorLayout(accessor, layout, error))
                stats.vertices += (long long)layout.count;
        }
        for (size_t accessor : vertexAccessors){
            GltfAccessorLayout layout;
            if (doc.accessorLayout(accessor, layout, error))
                stats.vertexBytes += align4(layout.elementSize) * layout.count;
        }
        for (size_t accessor : indexAccessors){
            GltfAccessorLayout layout;
            // byte indices are widened to 16 bits at load
            if (doc.accessorLayout(accessor, layout, error))
                stats.indexBytes += std::max<size_t>(layout.elementSize, 2) * layout.count;
        }
        error.clear();
        stats.acmr = stats.triangles > 0 ? transformed / (double)stats.triangles : 0.0;
        stats.overdraw = covered > 0.0 ? shaded / covered : 0.0;
        return stats;
    }

    ModelGeometryStats Cook::outputStats(const std::vector<View>& views){
        ModelGeometryStats stats;
        double transformed = 0.0, shaded = 0.0, covered = 0.0;
        for (const Group& group : groups){
            const std::vector<float> positions = positionsOf(group);
            stats.vertices += (long long)group.vertexCount;
            for (size_t p : group.primitives){
                const Primitive& primitive = primitives[p];
                std::vector<uint32_t> list = primitive.list;
                if (!primitive.indexed){
                    list.resize(group.vertexCount);
                    for (size_t v = 0; v < group.vertexCount; v++) list[v] = (uint32_t)v;
                }
                if (primitive.mode != 4 || list.size() < 3)
                    continue;
                list.resize(list.size() / 3 * 3);
                stats.triangles += (long long)list.size() / 3;
                transformed += meshopt_analyzeVertexCache(list.data(), list.size(), group.vertexCount, 16, 0, 0).vertices_transformed;
                if (!positions.empty()){
                    const meshopt_OverdrawStatistics o = meshopt_analyzeOverdraw(list.data(), list.size(), positions.data(), group.vertexCount, 12);
                    shaded += o.pixels_shaded;
                    covered += o.pixels_covered;
                }
            }
        }
        for (const View& view : views){
            if (view.kind == View::Vertex) stats.vertexBytes += view.data.size();
            if (view.kind == View::Index || view.kind == View::Lod) stats.indexBytes += view.data.size();
        }
        stats.acmr = stats.triangles > 0 ? transformed / (double)stats.triangles : 0.0;
        stats.overdraw = covered > 0.0 ? shaded / covered : 0.0;
        return stats;
    }

    bool Cook::assemble(std::vector<uint8_t>& bin, std::vector<View>& views, std::string& error){
        Json& root = doc.json();
        const bool compress = settings.meshCompression != MeshCompression::Off;
        const int level = settings.meshCompression == MeshCompression::High ? 3 : 2;
        const size_t accessorCount = root.contains("accessors") ? root["accessors"].size() : 0;
        std::vector<bool> placed(accessorCount, false);
        // what each accessor holds now: its layout and data (the groups' streams, the index
        // lists, or the source's bytes)
        std::vector<GltfAccessorLayout> layouts(accessorCount);
        std::vector<std::vector<uint8_t>> data(accessorCount);
        std::vector<std::string> filters(accessorCount, "NONE");
        std::vector<std::vector<uint8_t>> encoded(accessorCount);
        std::vector<bool> fromCook(accessorCount, false);
        std::vector<bool> quantized(accessorCount, false);

        // the index lists' accessors (made for an unindexed primitive, or the source's)
        for (Primitive& primitive : primitives){
            if (!primitive.indexed)
                continue;
            if (primitive.newIndices){
                primitive.indexAccessor = addAccessor(Json{{"componentType", primitive.indexType}, {"count", primitive.list.size()}, {"type", "SCALAR"}});
                root["meshes"][primitive.mesh]["primitives"][primitive.index]["indices"] = primitive.indexAccessor;
            }else{
                primitive.indexAccessor = *primitive.indices;
            }
        }
        // the calculated normals of primitives that had none
        for (Group& group : groups){
            for (Stream& stream : group.streams){
                if (!stream.newAccessor)
                    continue;
                stream.accessor = addAccessor(Json{{"componentType", gltf::FLOAT}, {"count", stream.layout.count}, {"type", "VEC3"}});
                stream.newAccessor = false;
                for (size_t p : group.primitives)
                    root["meshes"][primitives[p].mesh]["primitives"][primitives[p].index]["attributes"]["NORMAL"] = stream.accessor;
            }
        }
        // the levels' accessors
        for (Primitive& primitive : primitives){
            Json& json = root["meshes"][primitive.mesh]["primitives"][primitive.index];
            if (json.contains("extensions") && json["extensions"].contains("JE_mesh_lods") && !primitive.lods.empty())
                json["extensions"].erase("JE_mesh_lods");
            if (primitive.lods.empty())
                continue;
            Json levels = Json::array();
            for (size_t l = 0; l < primitive.lods.size(); l++){
                const auto& [list, lodError] = primitive.lods[l];
                const size_t accessor = addAccessor(Json{{"componentType", primitive.indexType}, {"count", list.size()}, {"type", "SCALAR"}});
                Json level = {{"indices", accessor}, {"error", lodError}};
                // an authored level's switch point (MSFT_screencoverage), optional in contract C1
                if (l < primitive.lodCoverage.size() && primitive.lodCoverage[l] >= 0.0f)
                    level["coverage"] = primitive.lodCoverage[l];
                levels.push_back(std::move(level));
            }
            if (!json.contains("extensions") || !json["extensions"].is_object())
                json["extensions"] = Json::object();
            json["extensions"]["JE_mesh_lods"] = Json{{"recipe", kMeshLodRecipe}, {"levels", levels}};
        }
        const size_t total = root.contains("accessors") ? root["accessors"].size() : 0;
        layouts.resize(total);
        data.resize(total);
        filters.resize(total, "NONE");
        encoded.resize(total);
        fromCook.resize(total, false);
        quantized.resize(total, false);
        placed.resize(total, false);

        auto indexBytes = [](const std::vector<uint32_t>& list, int type){
            std::vector<uint8_t> out(list.size() * (type == gltf::UNSIGNED_INT ? 4 : 2));
            for (size_t i = 0; i < list.size(); i++){
                if (type == gltf::UNSIGNED_INT){
                    std::memcpy(out.data() + i * 4, &list[i], 4);
                }else{
                    const uint16_t value = (uint16_t)list[i];
                    std::memcpy(out.data() + i * 2, &value, 2);
                }
            }
            return out;
        };
        auto indexLayout = [](size_t count, int type){
            GltfAccessorLayout layout;
            layout.componentType = type;
            layout.components = 1;
            layout.count = count;
            layout.elementSize = type == gltf::UNSIGNED_INT ? 4 : 2;
            return layout;
        };
        for (const Group& group : groups){
            for (const Stream& stream : group.streams){
                layouts[stream.accessor] = stream.layout;
                data[stream.accessor] = stream.data;
                filters[stream.accessor] = stream.filter;
                encoded[stream.accessor] = stream.encoded;
                fromCook[stream.accessor] = true;
                quantized[stream.accessor] = stream.quantized;
            }
        }
        for (const Primitive& primitive : primitives){
            if (!primitive.indexed)
                continue;
            layouts[primitive.indexAccessor] = indexLayout(primitive.list.size(), primitive.indexType);
            data[primitive.indexAccessor] = indexBytes(primitive.list, primitive.indexType);
            fromCook[primitive.indexAccessor] = true;
            const Json& levels = root["meshes"][primitive.mesh]["primitives"][primitive.index].value("extensions", Json::object())
                                     .value("JE_mesh_lods", Json::object()).value("levels", Json::array());
            for (size_t l = 0; l < primitive.lods.size() && l < levels.size(); l++){
                const size_t accessor = levels[l]["indices"].get<size_t>();
                layouts[accessor] = indexLayout(primitive.lods[l].first.size(), primitive.indexType);
                data[accessor] = indexBytes(primitive.lods[l].first, primitive.indexType);
                fromCook[accessor] = true;
            }
        }
        for (size_t a = 0; a < total; a++){
            if (fromCook[a])
                continue;
            if (!doc.readAccessorRaw(a, data[a], layouts[a], error))
                return false;
        }

        // the views: a group's streams one each (interleaved when a mesh would need more
        // buffers than a player holds and the source did not), its index lists together, its
        // levels together; everything else one per accessor
        std::map<size_t, size_t> sourceViewsOfMesh;
        {
            std::map<size_t, std::set<size_t>> views;
            for (const Primitive& primitive : primitives){
                std::vector<size_t> accessors;
                for (const auto& attribute : primitive.attributes) accessors.push_back(attribute.second);
                for (const auto& target : primitive.targets) for (const auto& stream : target) accessors.push_back(stream.second);
                if (primitive.indices) accessors.push_back(*primitive.indices);
                for (size_t a : accessors){
                    size_t view = 0;
                    if (a < sourceAccessors && GltfDocument::getIndex(root["accessors"][a], "bufferView", view))
                        views[primitive.mesh].insert(view);
                }
            }
            for (const auto& [mesh, set] : views) sourceViewsOfMesh[mesh] = set.size();
        }
        std::map<size_t, size_t> outputViewsOfMesh;
        for (const Group& group : groups){
            const size_t mesh = primitives[group.primitives[0]].mesh;
            outputViewsOfMesh[mesh] += group.streams.size() + 1;
        }
        auto placeVertex = [&](View& view, size_t accessor){
            const size_t offset = view.stride;
            view.accessors.push_back(accessor);
            view.offsets.push_back(offset);
            view.stride += align4(layouts[accessor].elementSize);
            placed[accessor] = true;
        };
        auto fillVertex = [&](View& view){
            view.count = layouts[view.accessors[0]].count;
            view.data.assign(view.count * view.stride, 0);
            const bool filtered = view.filter != "NONE";
            if (filtered)
                view.codecInput.assign(view.count * view.stride, 0);
            for (size_t i = 0; i < view.accessors.size(); i++){
                const size_t a = view.accessors[i];
                const size_t es = layouts[a].elementSize;
                for (size_t e = 0; e < view.count; e++){
                    std::memcpy(view.data.data() + e * view.stride + view.offsets[i], data[a].data() + e * es, es);
                    if (filtered)
                        std::memcpy(view.codecInput.data() + e * view.stride + view.offsets[i], encoded[a].data() + e * es, es);
                }
            }
        };
        for (const Group& group : groups){
            const size_t mesh = primitives[group.primitives[0]].mesh;
            const bool interleave = outputViewsOfMesh[mesh] > std::max(kPlayerMeshBuffers, sourceViewsOfMesh[mesh]);
            View shared;
            shared.kind = View::Vertex;
            shared.target = gltf::ARRAY_BUFFER;
            shared.mesh = mesh;
            for (const Stream& stream : group.streams){
                if (placed[stream.accessor])
                    continue;
                if (interleave && filters[stream.accessor] == "NONE"){
                    placeVertex(shared, stream.accessor);
                    continue;
                }
                View view;
                view.kind = View::Vertex;
                view.target = gltf::ARRAY_BUFFER;
                view.filter = filters[stream.accessor];
                view.mesh = mesh;
                placeVertex(view, stream.accessor);
                fillVertex(view);
                views.push_back(std::move(view));
            }
            if (!shared.accessors.empty()){
                fillVertex(shared);
                views.push_back(std::move(shared));
            }
            View indices, lods;
            indices.kind = View::Index;
            indices.target = gltf::ELEMENT_ARRAY_BUFFER;
            indices.mesh = mesh;
            lods.kind = View::Lod;
            lods.mesh = mesh;
            for (size_t p : group.primitives){
                const Primitive& primitive = primitives[p];
                if (!primitive.indexed)
                    continue;
                if (!placed[primitive.indexAccessor]){
                    indices.accessors.push_back(primitive.indexAccessor);
                    indices.offsets.push_back(indices.data.size());
                    indices.data.insert(indices.data.end(), data[primitive.indexAccessor].begin(), data[primitive.indexAccessor].end());
                    indices.count += layouts[primitive.indexAccessor].count;
                    indices.triangles = indices.triangles && primitive.mode == 4 && primitive.list.size() % 3 == 0;
                    indices.stride = layouts[primitive.indexAccessor].elementSize;
                    placed[primitive.indexAccessor] = true;
                }
                const Json& levels = root["meshes"][primitive.mesh]["primitives"][primitive.index].value("extensions", Json::object())
                                         .value("JE_mesh_lods", Json::object()).value("levels", Json::array());
                for (size_t l = 0; l < primitive.lods.size() && l < levels.size(); l++){
                    const size_t accessor = levels[l]["indices"].get<size_t>();
                    lods.accessors.push_back(accessor);
                    lods.offsets.push_back(lods.data.size());
                    lods.data.insert(lods.data.end(), data[accessor].begin(), data[accessor].end());
                    lods.count += layouts[accessor].count;
                    lods.stride = layouts[accessor].elementSize;
                    placed[accessor] = true;
                }
            }
            if (!indices.accessors.empty())
                views.push_back(std::move(indices));
            if (!lods.accessors.empty())
                views.push_back(std::move(lods));
        }
        // everything else: one view per accessor, the target it had
        for (size_t a = 0; a < total; a++){
            if (placed[a])
                continue;
            View view;
            view.kind = View::Other;
            view.accessors = {a};
            view.offsets = {0};
            view.stride = layouts[a].elementSize;
            view.count = layouts[a].count;
            view.data = data[a];
            size_t source = 0;
            if (a < sourceAccessors && GltfDocument::getIndex(root["accessors"][a], "bufferView", source)){
                const Json& sourceView = root["bufferViews"][source];
                view.target = (int)sourceView.value("target", 0);
            }
            if (view.target == gltf::ARRAY_BUFFER){
                // a vertex attribute's elements start at multiples of 4
                view.kind = View::Vertex;
                view.stride = align4(layouts[a].elementSize);
                view.data.assign(view.count * view.stride, 0);
                for (size_t e = 0; e < view.count; e++)
                    std::memcpy(view.data.data() + e * view.stride, data[a].data() + e * layouts[a].elementSize, layouts[a].elementSize);
            }else if (view.target == gltf::ELEMENT_ARRAY_BUFFER){
                view.kind = View::Index;
                view.triangles = false;
            }
            placed[a] = true;
            views.push_back(std::move(view));
        }
        // images in bufferViews
        std::vector<std::pair<size_t, size_t>> imageViews;     // image, its view
        if (const Json* images = GltfDocument::arrayOf(root, "images")){
            for (size_t i = 0; i < images->size(); i++){
                size_t source = 0;
                if (!GltfDocument::getIndex((*images)[i], "bufferView", source))
                    continue;
                const uint8_t* bytes = nullptr;
                size_t size = 0, stride = 0;
                if (!doc.viewData(source, bytes, size, stride, error))
                    return false;
                View view;
                view.kind = View::Image;
                view.data.assign(bytes, bytes + size);
                imageViews.push_back({i, views.size()});
                views.push_back(std::move(view));
            }
        }

        // the bytes: the BIN chunk holds raw views and compressed streams; a compressed view's
        // decoded bytes have their place in the fallback buffer (no data, as the extension wants)
        Json bufferViews = Json::array();
        size_t fallbackSize = 0;
        bool anyCompressed = false;
        for (View& view : views){
            Json json = Json::object();
            const bool vertexLike = view.kind == View::Vertex || view.kind == View::Other;
            std::string mode;
            if (compress && view.kind != View::Image && !view.data.empty()){
                if (vertexLike && view.stride % 4 == 0 && view.stride > 0 && view.stride <= 256)
                    mode = "ATTRIBUTES";
                else if ((view.kind == View::Index || view.kind == View::Lod) && (view.stride == 2 || view.stride == 4))
                    mode = view.triangles && view.count % 3 == 0 ? "TRIANGLES" : "INDICES";
            }
            while (bin.size() % 4 != 0)
                bin.push_back(0);
            const size_t binOffset = bin.size();
            if (!mode.empty()){
                std::vector<uint8_t> out;
                if (mode == "ATTRIBUTES"){
                    const std::vector<uint8_t>& input = view.codecInput.empty() ? view.data : view.codecInput;
                    out.resize(meshopt_encodeVertexBufferBound(view.count, view.stride));
                    out.resize(meshopt_encodeVertexBufferLevel(out.data(), out.size(), input.data(), view.count, view.stride, level, kVertexCodecVersion));
                }else{
                    std::vector<uint32_t> values(view.count);
                    for (size_t i = 0; i < view.count; i++){
                        if (view.stride == 4){
                            std::memcpy(&values[i], view.data.data() + i * 4, 4);
                        }else{
                            uint16_t value;
                            std::memcpy(&value, view.data.data() + i * 2, 2);
                            values[i] = value;
                        }
                    }
                    const size_t vertexBound = values.empty() ? 0 : *std::max_element(values.begin(), values.end()) + 1;
                    if (mode == "TRIANGLES"){
                        out.resize(meshopt_encodeIndexBufferBound(view.count, vertexBound));
                        out.resize(meshopt_encodeIndexBuffer(out.data(), out.size(), values.data(), view.count));
                    }else{
                        out.resize(meshopt_encodeIndexSequenceBound(view.count, vertexBound));
                        out.resize(meshopt_encodeIndexSequence(out.data(), out.size(), values.data(), view.count));
                    }
                }
                if (out.empty()){
                    error = "the meshopt encoder failed on a stream of " + request.assetPath;
                    return false;
                }
                bin.insert(bin.end(), out.begin(), out.end());
                fallbackSize = align4(fallbackSize);
                json["buffer"] = 1;
                json["byteOffset"] = fallbackSize;
                json["byteLength"] = view.data.size();
                fallbackSize += view.data.size();
                Json extension = {{"buffer", 0}, {"byteOffset", binOffset}, {"byteLength", out.size()}, {"byteStride", view.stride},
                                  {"count", view.count}, {"mode", mode}};
                if (view.filter != "NONE")
                    extension["filter"] = view.filter;
                json["extensions"] = Json{{"KHR_meshopt_compression", extension}};
                anyCompressed = true;
            }else{
                bin.insert(bin.end(), view.data.begin(), view.data.end());
                json["buffer"] = 0;
                if (binOffset) json["byteOffset"] = binOffset;
                json["byteLength"] = view.data.size();
            }
            if (json.contains("byteOffset") && json["byteOffset"].get<size_t>() == 0)
                json.erase("byteOffset");
            if (view.kind == View::Vertex)
                json["byteStride"] = view.stride;
            if (view.target)
                json["target"] = view.target;
            // (key order: the codec's extension last)
            if (json.contains("extensions")){
                Json extensions = json["extensions"];
                json.erase("extensions");
                json["extensions"] = extensions;
            }
            if (view.data.empty()){
                // glTF wants every bufferView at least one byte long
                json["byteLength"] = 1;
                if (!json.contains("extensions"))
                    bin.push_back(0);
            }
            bufferViews.push_back(json);
        }

        // the accessors point at their views
        for (size_t v = 0; v < views.size(); v++){
            const View& view = views[v];
            for (size_t i = 0; i < view.accessors.size(); i++){
                const size_t a = view.accessors[i];
                Json& accessor = root["accessors"][a];
                const GltfAccessorLayout& layout = layouts[a];
                accessor.erase("sparse");
                accessor["bufferView"] = v;
                if (view.offsets[i]) accessor["byteOffset"] = view.offsets[i];
                else accessor.erase("byteOffset");
                accessor["componentType"] = layout.componentType;
                accessor["count"] = layout.count;
                if (layout.normalized) accessor["normalized"] = true;
                else accessor.erase("normalized");
                if (!accessor.contains("type"))
                    accessor["type"] = gltf::typeName(layout.components);
                // bounds: POSITION's are required and exact; the others are kept where the
                // values only moved, recomputed for floats and dropped for what a level quantized
                bool isPosition = false;
                for (const Primitive& primitive : primitives)
                    for (const auto& attribute : primitive.attributes)
                        isPosition = isPosition || (attribute.first == "POSITION" && attribute.second == a);
                if (layout.componentType == gltf::FLOAT && (isPosition || accessor.contains("min") || accessor.contains("max"))){
                    const std::vector<float> values = floatsOf(data[a], layout);
                    Json mins = Json::array(), maxs = Json::array();
                    for (int c = 0; c < layout.components; c++){
                        float lo = std::numeric_limits<float>::max(), hi = -std::numeric_limits<float>::max();
                        for (size_t e = 0; e < layout.count; e++){
                            lo = std::min(lo, values[e * (size_t)layout.components + (size_t)c]);
                            hi = std::max(hi, values[e * (size_t)layout.components + (size_t)c]);
                        }
                        mins.push_back(lo);
                        maxs.push_back(hi);
                    }
                    accessor["min"] = mins;
                    accessor["max"] = maxs;
                }else if (quantized[a] || (view.kind == View::Index && fromCook[a])){
                    accessor.erase("min");
                    accessor.erase("max");
                }
            }
        }
        for (const auto& [image, view] : imageViews)
            root["images"][image]["bufferView"] = view;
        root["bufferViews"] = bufferViews;
        Json buffers = Json::array();
        buffers.push_back(Json{{"byteLength", bin.size()}});
        if (anyCompressed)
            buffers.push_back(Json{{"byteLength", std::max<size_t>(fallbackSize, 1)},
                                   {"extensions", {{"KHR_meshopt_compression", {{"fallback", true}}}}}});
        root["buffers"] = buffers;
        if (bin.empty()){
            root.erase("buffers");
            root.erase("bufferViews");
        }

        for (const char* name : MESHOPT_NAMES){
            removeExtension(root, "extensionsUsed", name);
            removeExtension(root, "extensionsRequired", name);
        }
        if (anyCompressed){
            addExtension(root, "extensionsUsed", "KHR_meshopt_compression");
            addExtension(root, "extensionsRequired", "KHR_meshopt_compression");
        }
        bool nonCoreTypes = false;
        for (const Group& group : groups)
            for (const Stream& stream : group.streams)
                nonCoreTypes = nonCoreTypes || (stream.quantized && (stream.semantic == "NORMAL" || stream.semantic == "TANGENT"));
        if (nonCoreTypes){
            addExtension(root, "extensionsUsed", "KHR_mesh_quantization");
            addExtension(root, "extensionsRequired", "KHR_mesh_quantization");
        }
        bool anyLods = false;
        for (const Primitive& primitive : primitives)
            anyLods = anyLods || !primitive.lods.empty();
        if (anyLods)
            addExtension(root, "extensionsUsed", "JE_mesh_lods");
        return true;
    }

    bool Cook::run(std::vector<uint8_t>& glb, std::string& error){
        const auto start = std::chrono::steady_clock::now();
        std::vector<uint8_t> sourceBytes;
        if (!doc.load(request.source, error, &sourceBytes))
            return false;
        record.sourceBytes = sourceBytes.size();
        record.sourceExtensions = doc.requiredExtensions();
        record.compression = meshCompressionName(settings.meshCompression);
        record.family = modelFamilyName(request.family);
        // the source as the loader reads it: its findings (R5), the editor-only decodes (R6) and
        // the MSFT_lod chains (R4)
        ModelSourceLoad load;
        if (!ModelSourceDecode::inspect(request.source, request.assetPath, load, error))
            return false;
        record.diagnostics = load.result.diagnostics;
        // a game would load it wrong: the cook fails, and with it the export (R5). The editor keeps
        // loading the source, so the model can still be looked at.
        const std::string blocking = ModelSourceDecode::blockingError(record.diagnostics);
        if (!blocking.empty()){
            error = blocking;
            return false;
        }
        const bool draco = std::find(load.result.handledExtensions.begin(), load.result.handledExtensions.end(),
                                     "KHR_draco_mesh_compression") != load.result.handledExtensions.end();
        if (!ModelSourceDecode::takeDraco(doc, load, error))
            return false;
        if (draco)
            record.transcoded.push_back("KHR_draco_mesh_compression");
        if (!ModelSourceDecode::transcodeImages(doc, request.family != ModelFamily::Web, record.transcoded, error))
            return false;
        if (!foldAuthoredLods(load.result.lodChains, error))
            return false;
        load = ModelSourceLoad();
        Json& root = doc.json();
        const std::string signature = structureSignature(root);
        if (const Json* meshes = GltfDocument::arrayOf(root, "meshes"))
            record.meshes = (int)meshes->size();

        const std::string foreign = foreignViewReference(root, "");
        if (!foreign.empty()){
            if (!record.transcoded.empty()){
                error = request.assetPath + " cannot ship as it is (" + foreign.substr(1) + " refers to a bufferView from an extension "
                        "the cook does not know), and it needs " + record.transcoded.front() + " decoded: export it again without one of them";
                return false;
            }
            // a file the cook cannot rewrite safely ships as it is
            std::ifstream in(request.source, std::ios::binary);
            glb.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            record.passThrough = true;
            record.diskBytes = glb.size();
            record.requiredExtensions = record.sourceExtensions;
            record.warnings.push_back("Shipped as it is: " + foreign.substr(1) + " refers to a bufferView from an extension the cook does not know");
            return true;
        }

        if (!collect(error))
            return false;
        record.primitives = (int)primitives.size();
        buildGroups();
        record.before = sourceStats(error);
        if (!error.empty())
            return false;
        for (Group& group : groups){
            if (!readGroup(group, error))
                return false;
            if (!group.locked && !processGroup(group, error))
                return false;
            if (isCancelled()){
                error = "cancelled";
                return false;
            }
            if (group.locked){
                const Primitive& first = primitives[group.primitives[0]];
                record.warnings.push_back(meshLabel(root, first.mesh, first.index) + " kept its vertex order: " + group.lockReason);
            }
        }
        if (!finishIndices(error))
            return false;
        // the model's size (the reference of every deviation)
        float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        float hi[3] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
        for (const Group& group : groups){
            const std::vector<float> positions = positionsOf(group);
            for (size_t i = 0; i + 2 < positions.size(); i += 3)
                for (int c = 0; c < 3; c++){
                    lo[c] = std::min(lo[c], positions[i + (size_t)c]);
                    hi[c] = std::max(hi[c], positions[i + (size_t)c]);
                }
        }
        if (lo[0] <= hi[0])
            record.modelSize = std::sqrt((double)(hi[0] - lo[0]) * (hi[0] - lo[0]) + (double)(hi[1] - lo[1]) * (hi[1] - lo[1]) +
                                         (double)(hi[2] - lo[2]) * (hi[2] - lo[2]));
        for (Group& group : groups){
            quantize(group);
            settleIndexRotation(group);
            buildLods(group);
            if (isCancelled()){
                error = "cancelled";
                return false;
            }
        }
        std::vector<uint8_t> bin;
        std::vector<View> views;
        if (!assemble(bin, views, error))
            return false;
        if (structureSignature(root) != signature){
            error = "the cook would change " + request.assetPath + "'s nodes, meshes, materials or animations: it is not written";
            return false;
        }
        glb = GltfDocument::writeGlb(root, bin);
        record.after = outputStats(views);
        record.diskBytes = glb.size();
        record.requiredExtensions = doc.requiredExtensions();
        record.duplicatePercent = dedupeBefore > 0 ? (double)dedupeRemoved * 100.0 / (double)dedupeBefore : 0.0;
        // the levels over every primitive that has them
        long long lod0 = 0;
        for (const Primitive& primitive : primitives){
            if (primitive.lods.empty())
                continue;
            record.lodPrimitives++;
            lod0 += (long long)primitive.list.size() / 3;
            for (size_t l = 0; l < primitive.lods.size(); l++){
                if (record.lods.size() <= l){
                    ModelLodLevelStats level;
                    level.level = (int)l + 1;
                    record.lods.push_back(level);
                }
                ModelLodLevelStats& level = record.lods[l];
                level.triangles += (long long)primitive.lods[l].first.size() / 3;
                level.error = std::max(level.error, (double)primitive.lods[l].second);
                if (record.modelSize > 0.0)
                    level.deviation = std::max(level.deviation, primitive.lodDeviation[l] / record.modelSize);
            }
        }
        for (ModelLodLevelStats& level : record.lods)
            level.share = lod0 > 0 ? (double)level.triangles / (double)lod0 : 0.0;
        record.cookMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return true;
    }

}

bool ModelCooker::isCookableModel(const std::string& path){
    std::string extension = fs::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return extension == ".gltf" || extension == ".glb" || extension == ".fbx";
}

ModelCooker::QuantizationBits ModelCooker::quantizationBits(MeshCompression compression){
    switch (compression){
        case MeshCompression::Low: return {16, 14, 10, 8};
        case MeshCompression::Medium: return {14, 12, 8, 8};
        case MeshCompression::High: return {12, 10, 8, 8};
        default: return {};
    }
}

bool ModelCooker::sourceFiles(const fs::path& source, std::vector<fs::path>& extra, std::string& error){
    extra.clear();
    std::ifstream in(source, std::ios::binary);
    if (!in){
        error = "cannot read " + source.filename().generic_string();
        return false;
    }
    std::vector<uint8_t> head(12);
    in.read(reinterpret_cast<char*>(head.data()), 12);
    std::string text;
    if (in.gcount() == 12 && std::memcmp(head.data(), "glTF", 4) == 0){
        uint32_t chunkLength = 0;
        in.read(reinterpret_cast<char*>(&chunkLength), 4);
        in.seekg(4, std::ios::cur);
        if (!in || chunkLength > (64u << 20)){
            error = source.filename().generic_string() + " has an invalid JSON chunk";
            return false;
        }
        text.resize(chunkLength);
        in.read(text.data(), chunkLength);
    }else{
        in.clear();
        in.seekg(0);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const GltfDocument::Json json = GltfDocument::Json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()){
        error = source.filename().generic_string() + " is not valid glTF JSON";
        return false;
    }
    if (const GltfDocument::Json* buffers = GltfDocument::arrayOf(json, "buffers")){
        for (const GltfDocument::Json& buffer : *buffers){
            const std::string uri = buffer.value("uri", std::string());
            if (uri.empty() || uri.rfind("data:", 0) == 0)
                continue;
            std::string decoded;
            for (size_t i = 0; i < uri.size(); i++){
                auto hex = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
                if (uri[i] == '%' && i + 2 < uri.size() && hex(uri[i + 1]) >= 0 && hex(uri[i + 2]) >= 0){
                    decoded.push_back((char)(hex(uri[i + 1]) * 16 + hex(uri[i + 2])));
                    i += 2;
                }else{
                    decoded.push_back(uri[i]);
                }
            }
            extra.push_back(source.parent_path() / FileUtils::pathFromUtf8(decoded));
        }
    }
    return true;
}

bool ModelCooker::planKey(ModelCookCache& cache, const ModelCookRequest& request, std::string& key, std::string& error){
    std::vector<fs::path> extra;
    if (!sourceFiles(request.source, extra, error))
        return false;
    std::string sha;
    if (!cache.sourceHash(request.source, extra, sha)){
        error = "cannot read " + request.assetPath + " or one of its buffers";
        return false;
    }
    key = ModelCookCache::key(sha, request.settings, request.family, request.blended);
    return true;
}

bool ModelCooker::cookToMemory(const ModelCookRequest& request, std::vector<uint8_t>& glb, ModelCookRecord& record, std::string& error,
                               const std::function<bool()>& cancelled){
    record = ModelCookRecord();
    Cook cook(request, record, cancelled);
    return cook.run(glb, error);
}

ModelCookResult ModelCooker::cook(ModelCookCache& cache, const ModelCookRequest& request, const std::function<bool()>& cancelled){
    ModelCookResult result;
    if (!planKey(cache, request, result.key, result.error))
        return result;
    if (cache.has(result.key) && cache.load(result.key, result.record)){
        result.ok = true;
        result.cacheHit = true;
        return result;
    }
    std::vector<uint8_t> glb;
    if (!cookToMemory(request, glb, result.record, result.error, cancelled)){
        result.cancelled = result.error == "cancelled";
        return result;
    }
    result.record.key = result.key;
    if (!cache.store(result.key, glb, result.record, result.error))
        return result;
    result.ok = true;
    return result;
}
