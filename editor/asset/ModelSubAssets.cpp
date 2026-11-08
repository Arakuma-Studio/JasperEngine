// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelSubAssets.h"

#include "Engine.h"
#include "Out.h"
#include "asset/AssetFileWrite.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using Json = nlohmann::json;

namespace {

    void addKind(ModelContents& contents, SubAssetKind kind, const std::vector<std::string>& names) {
        const std::vector<std::string> keys = subAssetKeys(kind, names);
        for (size_t i = 0; i < names.size(); i++) {
            contents.subAssets.push_back({kind, names[i], i, keys[i]});
        }
    }

    bool isDataUri(const std::string& uri) {
        return uri.compare(0, 5, "data:") == 0;
    }

    std::string gltfItemName(const Json& item, const char* kind, size_t index) {
        const std::string name = item.is_object() && item.contains("name") && item["name"].is_string()
            ? item["name"].get<std::string>() : std::string();
        return std::string(kind) + " '" + (name.empty() ? "#" + std::to_string(index) : name) + "'";
    }

    // The blend shapes a primitive keeps, as MeshSystem::loadGLTF reads them: the attributes a target
    // has lower what the vertex inputs hold for the targets after it.
    size_t supportedMorphTargets(const Json& targets) {
        bool normals = false, tangents = false;
        size_t loaded = 0;
        for (const Json& target : targets) {
            const size_t supported = (normals && tangents) ? 2 : (normals || tangents) ? 4 : MAX_MORPHTARGETS;
            if (loaded >= supported) {
                return supported;
            }
            loaded++;
            normals = normals || (target.is_object() && target.contains("NORMAL"));
            tangents = tangents || (target.is_object() && target.contains("TANGENT"));
        }
        return targets.size();
    }

    void scanGltfLimits(const Json& root, ModelContents& contents) {
        const Json empty = Json::array();
        const Json& accessors = root.contains("accessors") && root["accessors"].is_array() ? root["accessors"] : empty;
        std::set<int> views;
        const auto addView = [&](const Json& accessor) {
            if (accessor.is_number_integer() && accessor.get<int>() >= 0 && static_cast<size_t>(accessor.get<int>()) < accessors.size()) {
                const Json& read = accessors[accessor.get<size_t>()];
                if (read.is_object() && read.contains("bufferView") && read["bufferView"].is_number_integer()) {
                    views.insert(read["bufferView"].get<int>());
                }
            }
        };
        const Json& meshes = root.contains("meshes") && root["meshes"].is_array() ? root["meshes"] : empty;
        for (size_t m = 0; m < meshes.size(); m++) {
            if (!meshes[m].is_object() || !meshes[m].contains("primitives") || !meshes[m]["primitives"].is_array()) {
                continue;
            }
            size_t count = 0, limit = 0;
            for (const Json& primitive : meshes[m]["primitives"]) {
                if (!primitive.is_object()) {
                    continue;
                }
                if (primitive.contains("indices")) addView(primitive["indices"]);
                if (primitive.contains("attributes") && primitive["attributes"].is_object()) {
                    for (const Json& attribute : primitive["attributes"]) addView(attribute);
                }
                if (primitive.contains("targets") && primitive["targets"].is_array()) {
                    const size_t supported = supportedMorphTargets(primitive["targets"]);
                    if (primitive["targets"].size() > supported && primitive["targets"].size() > count) {
                        count = primitive["targets"].size();
                        limit = supported;
                    }
                    for (const Json& target : primitive["targets"]) {
                        if (target.is_object()) {
                            for (const Json& attribute : target) addView(attribute);
                        }
                    }
                }
            }
            if (count > 0) {
                contents.limits.push_back({gltfItemName(meshes[m], "mesh", m), "blend shapes", count, limit});
            }
        }

        const size_t gpuBuffers = static_cast<size_t>(Engine::getGpuResourceLimits().buffers);
        if (views.size() > gpuBuffers) {
            contents.limits.push_back({"the file", "vertex and index buffers", views.size(), gpuBuffers});
        }
    }

    bool scanGltfJson(const std::string& json, ModelContents& contents) {
        Json root = Json::parse(json, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            return false;
        }
        auto names = [&](const char* key) {
            std::vector<std::string> list;
            if (root.contains(key) && root[key].is_array()) {
                for (const Json& item : root[key]) {
                    list.push_back(item.is_object() && item.contains("name") && item["name"].is_string()
                        ? item["name"].get<std::string>() : std::string());
                }
            }
            return list;
        };
        addKind(contents, SubAssetKind::Mesh, names("meshes"));
        addKind(contents, SubAssetKind::Material, names("materials"));
        addKind(contents, SubAssetKind::Animation, names("animations"));
        addKind(contents, SubAssetKind::Skin, names("skins"));
        addKind(contents, SubAssetKind::Image, names("images"));
        scanGltfLimits(root, contents);
        for (const char* key : {"buffers", "images"}) {
            if (!root.contains(key) || !root[key].is_array()) {
                continue;
            }
            for (const Json& item : root[key]) {
                if (item.is_object() && item.contains("uri") && item["uri"].is_string()) {
                    const std::string uri = item["uri"].get<std::string>();
                    if (!uri.empty() && !isDataUri(uri)) {
                        contents.uris.push_back(uri);
                    }
                }
            }
        }
        return true;
    }

    uint32_t readU32(const std::string& bytes, size_t offset) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    bool scanGlb(const std::string& bytes, ModelContents& contents) {
        // header: magic "glTF", version, length; then the JSON chunk: length, type "JSON"
        if (bytes.size() < 20 || bytes.compare(0, 4, "glTF") != 0 || bytes.compare(16, 4, "JSON") != 0) {
            return false;
        }
        const uint32_t length = readU32(bytes, 12);
        if (20 + static_cast<size_t>(length) > bytes.size()) {
            return false;
        }
        return scanGltfJson(bytes.substr(20, length), contents);
    }

    std::string subAssetTrim(const std::string& text) {
        size_t begin = 0, end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) begin++;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
        return text.substr(begin, end - begin);
    }

    bool scanObj(const std::string& bytes, ModelContents& contents) {
        // a mesh per object ("o"), else per group ("g"); faces before any object are one
        // unnamed mesh. Materials by name, in order of first use.
        std::vector<std::string> objects, groups, materials;
        std::vector<std::string> libraries;
        bool facesBeforeObject = false;
        bool facesBeforeGroup = false;
        for (size_t begin = 0; begin < bytes.size();) {
            size_t end = bytes.find('\n', begin);
            if (end == std::string::npos) end = bytes.size();
            const std::string line = subAssetTrim(bytes.substr(begin, end - begin));
            begin = end + 1;
            if (line.size() < 2 || line[0] == '#') {
                continue;
            }
            const size_t space = line.find_first_of(" \t");
            const std::string keyword = line.substr(0, space);
            const std::string value = space == std::string::npos ? std::string() : subAssetTrim(line.substr(space));
            if (keyword == "o") {
                objects.push_back(value);
            } else if (keyword == "g") {
                groups.push_back(value);
            } else if (keyword == "f") {
                facesBeforeObject |= objects.empty();
                facesBeforeGroup |= groups.empty();
            } else if (keyword == "usemtl") {
                if (std::find(materials.begin(), materials.end(), value) == materials.end()) {
                    materials.push_back(value);
                }
            } else if (keyword == "mtllib" && !value.empty()) {
                libraries.push_back(value);
            }
        }
        std::vector<std::string> meshes = !objects.empty() ? objects : groups;
        const bool leading = !objects.empty() ? facesBeforeObject : facesBeforeGroup;
        if (leading || meshes.empty()) {
            meshes.insert(meshes.begin(), std::string());
        }
        addKind(contents, SubAssetKind::Mesh, meshes);
        addKind(contents, SubAssetKind::Material, materials);
        contents.uris = libraries;
        return true;
    }

}

std::string editor::modelLimitMessage(const std::string& model, const ModelLimit& limit) {
    std::string message = "Model " + model + ": " + limit.what + " has " + std::to_string(limit.count) + " " + limit.kind +
                          ", more than the maximum allowed (" + std::to_string(limit.limit) + ")";
    if (limit.kind == "blend shapes") {
        message += ": the ones over it are left out";
    } else {
        message += ": what does not fit does not draw";
    }
    return message;
}

void editor::reportModelLimits(const std::string& model, const ModelContents& contents) {
    for (const ModelLimit& limit : contents.limits) {
        Out::error("%s", modelLimitMessage(model, limit).c_str());
    }
}

bool editor::scanModelBytes(const std::string& extension, const std::string& bytes, ModelContents& contents) {
    contents = ModelContents();
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".gltf") return scanGltfJson(bytes, contents);
    if (ext == ".glb") return scanGlb(bytes, contents);
    if (ext == ".obj") return scanObj(bytes, contents);
    return false;
}

bool editor::scanModelFile(const std::filesystem::path& file, ModelContents& contents) {
    std::string bytes;
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".glb") {
        // only the header and the JSON chunk: the binary chunk can be large
        std::ifstream in(file, std::ios::binary);
        std::string header(20, '\0');
        if (!in.read(&header[0], 20)) {
            contents = ModelContents();
            return false;
        }
        const uint32_t length = readU32(header, 12);
        std::string json(length, '\0');
        if (length > 0 && !in.read(&json[0], length)) {
            contents = ModelContents();
            return false;
        }
        return scanModelBytes(ext, header + json, contents);
    }
    // an FBX (upstream's import, loaded through the glTF it converts into) has no sub-assets
    // here: nothing to read
    if (ext != ".gltf" && ext != ".obj") {
        contents = ModelContents();
        return false;
    }
    if (!assetio::readFile(file, bytes)) {
        contents = ModelContents();
        return false;
    }
    return scanModelBytes(file.extension().string(), bytes, contents);
}
