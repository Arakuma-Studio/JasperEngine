// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelHeader.h"

#include "model/GltfDocument.h"
#include "model/ModelCooker.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

bool editor::readModelHeader(const fs::path& file, ModelHeader& out, std::string& error){
    out = ModelHeader();
    std::ifstream in(file, std::ios::binary);
    if (!in){
        error = "cannot read " + file.filename().generic_string();
        return false;
    }
    char head[12] = {};
    in.read(head, 12);
    std::string text;
    if (in.gcount() == 12 && std::memcmp(head, "glTF", 4) == 0){
        uint32_t length = 0, type = 0;
        in.read(reinterpret_cast<char*>(&length), 4);
        in.read(reinterpret_cast<char*>(&type), 4);
        if (!in || type != 0x4E4F534A || length > (256u << 20)){
            error = file.filename().generic_string() + " has no valid JSON chunk";
            return false;
        }
        text.resize(length);
        in.read(text.data(), length);
    }else{
        in.clear();
        in.seekg(0);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const GltfDocument::Json json = GltfDocument::Json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("asset")){
        error = file.filename().generic_string() + " is not valid glTF JSON";
        return false;
    }
    std::error_code ec;
    out.fileBytes = (uint64_t)fs::file_size(file, ec);
    std::vector<fs::path> extra;
    std::string ignored;
    if (ModelCooker::sourceFiles(file, extra, ignored))
        for (const fs::path& buffer : extra){
            out.buffers.push_back(buffer.lexically_relative(file.parent_path()).generic_string());
            out.fileBytes += (uint64_t)fs::file_size(buffer, ec);
        }
    for (const char* key : {"extensionsRequired", "extensionsUsed"}){
        if (const GltfDocument::Json* list = GltfDocument::arrayOf(json, key))
            for (const auto& item : *list)
                if (item.is_string())
                    (key[10] == 'R' ? out.requiredExtensions : out.usedExtensions).push_back(item.get<std::string>());
    }
    auto sizeOf = [&](const char* key){
        const GltfDocument::Json* list = GltfDocument::arrayOf(json, key);
        return list ? (int)list->size() : 0;
    };
    out.meshes = sizeOf("meshes");
    out.materials = sizeOf("materials");
    out.nodes = sizeOf("nodes");
    out.animations = sizeOf("animations");
    out.skinned = sizeOf("skins") > 0;
    const GltfDocument::Json* accessors = GltfDocument::arrayOf(json, "accessors");
    auto accessor = [&](const GltfDocument::Json& object, const char* key, size_t& count, size_t& elementSize){
        size_t index = 0;
        if (!accessors || !GltfDocument::getIndex(object, key, index) || index >= accessors->size())
            return false;
        const GltfDocument::Json& a = (*accessors)[index];
        count = a.value("count", (size_t)0);
        elementSize = (size_t)gltf::componentSize(a.value("componentType", 0)) * (size_t)gltf::componentCount(a.value("type", std::string()));
        return true;
    };
    std::set<size_t> vertexAccessors, positionAccessors, indexAccessors;
    if (const GltfDocument::Json* materials = GltfDocument::arrayOf(json, "materials"))
        for (const auto& material : *materials)
            out.blended = out.blended || material.value("alphaMode", std::string()) == "BLEND";
    if (const GltfDocument::Json* meshes = GltfDocument::arrayOf(json, "meshes")){
        for (const auto& mesh : *meshes){
            if (!mesh.contains("primitives") || !mesh["primitives"].is_array())
                continue;
            for (const auto& primitive : mesh["primitives"]){
                out.primitives++;
                size_t vertexCount = 0, elementSize = 0;
                if (primitive.contains("attributes") && primitive["attributes"].is_object()){
                    for (auto it = primitive["attributes"].begin(); it != primitive["attributes"].end(); ++it){
                        size_t index = 0;
                        if (!GltfDocument::getIndex(primitive["attributes"], it.key().c_str(), index))
                            continue;
                        vertexAccessors.insert(index);
                        if (it.key() == "POSITION"){
                            positionAccessors.insert(index);
                            accessor(primitive["attributes"], "POSITION", vertexCount, elementSize);
                        }
                    }
                }
                if (primitive.contains("targets") && primitive["targets"].is_array()){
                    out.morphTargets = std::max(out.morphTargets, (int)primitive["targets"].size());
                    for (const auto& target : primitive["targets"])
                        if (target.is_object())
                            for (auto it = target.begin(); it != target.end(); ++it)
                                if (it->is_number_unsigned() || it->is_number_integer())
                                    vertexAccessors.insert(it->get<size_t>());
                }
                const int mode = primitive.value("mode", 4);
                size_t indexCount = 0;
                if (accessor(primitive, "indices", indexCount, elementSize)){
                    indexAccessors.insert(primitive["indices"].get<size_t>());
                    if (elementSize == 4 && vertexCount <= 65535)
                        out.wideIndices = true;
                }else if (mode == 4){
                    indexCount = vertexCount;
                    out.unindexed = true;
                }
                if (mode == 4)
                    out.triangles += (long long)(indexCount / 3);
            }
        }
    }
    for (size_t index : positionAccessors)
        out.vertices += (long long)(*accessors)[index].value("count", (size_t)0);
    for (size_t index : vertexAccessors){
        if (!accessors || index >= accessors->size())
            continue;
        const auto& a = (*accessors)[index];
        const size_t elementSize = (size_t)gltf::componentSize(a.value("componentType", 0)) * (size_t)gltf::componentCount(a.value("type", std::string()));
        out.vertexBytes += ((elementSize + 3) & ~size_t(3)) * a.value("count", (size_t)0);
    }
    for (size_t index : indexAccessors){
        const auto& a = (*accessors)[index];
        out.indexBytes += (uint64_t)std::max(2, gltf::componentSize(a.value("componentType", 0))) * a.value("count", (size_t)0);
    }
    return true;
}
