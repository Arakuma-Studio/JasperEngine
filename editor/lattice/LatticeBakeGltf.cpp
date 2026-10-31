// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LatticeBakeGltf.h"
#include "io/AtomicFile.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    using Json = nlohmann::ordered_json;

    const uint32_t GLB_MAGIC = 0x46546C67;      // "glTF"
    const uint32_t GLB_CHUNK_JSON = 0x4E4F534A; // "JSON"
    const uint32_t GLB_CHUNK_BIN = 0x004E4942;  // "BIN\0"
    const int COMPONENT_FLOAT = 5126;
    const int TARGET_ARRAY_BUFFER = 34962;

    // the extensions whose data cannot be read or rewritten without their decoder
    const char* const COMPRESSION_EXTENSIONS[] = {
        "KHR_draco_mesh_compression", "EXT_meshopt_compression", "KHR_meshopt_compression"
    };

    uint32_t readU32(const uint8_t* p){
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    void appendU32(std::vector<uint8_t>& out, uint32_t value){
        out.push_back((uint8_t)(value & 0xFF));
        out.push_back((uint8_t)((value >> 8) & 0xFF));
        out.push_back((uint8_t)((value >> 16) & 0xFF));
        out.push_back((uint8_t)((value >> 24) & 0xFF));
    }

    bool readFile(const fs::path& path, std::vector<uint8_t>& data){
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()){
            return false;
        }
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0){
            return false;
        }
        in.seekg(0, std::ios::beg);
        data.resize((size_t)size);
        if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size)){
            return false;
        }
        return true;
    }

    int componentSize(int componentType){
        switch (componentType){
            case 5120: case 5121: return 1; // BYTE, UNSIGNED_BYTE
            case 5122: case 5123: return 2; // SHORT, UNSIGNED_SHORT
            case 5125: case 5126: return 4; // UNSIGNED_INT, FLOAT
            default: return 0;
        }
    }

    int componentCount(const std::string& type){
        if (type == "SCALAR") return 1;
        if (type == "VEC2") return 2;
        if (type == "VEC3") return 3;
        if (type == "VEC4") return 4;
        if (type == "MAT2") return 4;
        if (type == "MAT3") return 9;
        if (type == "MAT4") return 16;
        return 0;
    }

    const char* typeName(int components){
        switch (components){
            case 1: return "SCALAR";
            case 2: return "VEC2";
            case 3: return "VEC3";
            default: return "VEC4";
        }
    }

    // a non-negative integer member; false when missing or not one
    bool getIndex(const Json& object, const char* key, size_t& out){
        if (!object.is_object()) return false;
        auto it = object.find(key);
        if (it == object.end() || !it->is_number()) return false;
        if (it->is_number_unsigned()){
            out = it->get<size_t>();
            return true;
        }
        if (it->is_number_integer()){
            const int64_t value = it->get<int64_t>();
            if (value < 0) return false;
            out = (size_t)value;
            return true;
        }
        const double value = it->get<double>();
        if (!(value >= 0.0) || value != std::floor(value) || value > 9007199254740992.0) return false;
        out = (size_t)value;
        return true;
    }

    size_t getSize(const Json& object, const char* key, size_t fallback){
        size_t value = 0;
        return getIndex(object, key, value) ? value : fallback;
    }

    const Json* arrayOf(const Json& root, const char* key){
        auto it = root.find(key);
        if (it == root.end() || !it->is_array()) return nullptr;
        return &(*it);
    }

    float readComponent(const uint8_t* p, int componentType, bool normalized){
        switch (componentType){
            case 5120: { int8_t v; std::memcpy(&v, p, 1); return normalized ? std::max((float)v / 127.0f, -1.0f) : (float)v; }
            case 5121: { uint8_t v = *p; return normalized ? (float)v / 255.0f : (float)v; }
            case 5122: { int16_t v; std::memcpy(&v, p, 2); return normalized ? std::max((float)v / 32767.0f, -1.0f) : (float)v; }
            case 5123: { uint16_t v; std::memcpy(&v, p, 2); return normalized ? (float)v / 65535.0f : (float)v; }
            case 5125: { uint32_t v; std::memcpy(&v, p, 4); return (float)v; }
            default: { float v; std::memcpy(&v, p, 4); return v; }
        }
    }

    uint32_t readIndex(const uint8_t* p, int componentType){
        switch (componentType){
            case 5121: return *p;
            case 5123: { uint16_t v; std::memcpy(&v, p, 2); return v; }
            default: { uint32_t v; std::memcpy(&v, p, 4); return v; }
        }
    }

    bool decodeBase64(const std::string& text, size_t start, std::vector<uint8_t>& out){
        auto value = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        };
        out.clear();
        out.reserve((text.size() - start) / 4 * 3);
        uint32_t accumulator = 0;
        int bits = 0;
        bool padding = false;
        for (size_t i = start; i < text.size(); i++){
            const char c = text[i];
            if (c == '='){
                padding = true;
                continue;
            }
            const int v = value(c);
            if (v < 0 || padding){
                return false;
            }
            accumulator = (accumulator << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8){
                bits -= 8;
                out.push_back((uint8_t)((accumulator >> bits) & 0xFF));
            }
        }
        return true;
    }

    // %XX escapes of a relative URI reference
    std::string decodeUri(const std::string& uri){
        std::string result;
        result.reserve(uri.size());
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < uri.size(); i++){
            if (uri[i] == '%' && i + 2 < uri.size() && hex(uri[i + 1]) >= 0 && hex(uri[i + 2]) >= 0){
                result.push_back((char)(hex(uri[i + 1]) * 16 + hex(uri[i + 2])));
                i += 2;
            }else{
                result.push_back(uri[i]);
            }
        }
        return result;
    }

    bool listsExtension(const Json& root, const char* key, const char* name){
        const Json* list = arrayOf(root, key);
        if (!list) return false;
        for (const Json& item : *list){
            if (item.is_string() && item.get<std::string>() == name) return true;
        }
        return false;
    }

}

bool LatticeBakeGltf::Slot::operator<(const Slot& other) const{
    if (primitive != other.primitive) return primitive < other.primitive;
    if (target != other.target) return target < other.target;
    return semantic < other.semantic;
}

bool LatticeBakeGltf::Data::sameAs(const Data& other) const{
    return components == other.components && values.size() == other.values.size() &&
           (values.empty() || std::memcmp(values.data(), other.values.data(), values.size() * sizeof(float)) == 0);
}

bool LatticeBakeGltf::load(const fs::path& file, std::string& error){
    json = Json();
    buffers.clear();
    outputs.clear();

    std::vector<uint8_t> bytes;
    if (!readFile(file, bytes)){
        error = "cannot read '" + file.generic_string() + "'";
        return false;
    }

    std::vector<uint8_t> binChunk;
    bool hasBinChunk = false;
    std::string text;
    if (bytes.size() >= 12 && readU32(bytes.data()) == GLB_MAGIC){
        const uint32_t version = readU32(bytes.data() + 4);
        const uint32_t length = readU32(bytes.data() + 8);
        if (version != 2 || length > bytes.size() || length < 20){
            error = "'" + file.generic_string() + "' is not a valid glTF 2.0 binary";
            return false;
        }
        size_t offset = 12;
        bool hasJson = false;
        while (offset + 8 <= length){
            const uint32_t chunkLength = readU32(bytes.data() + offset);
            const uint32_t chunkType = readU32(bytes.data() + offset + 4);
            offset += 8;
            if (chunkLength > length - offset){
                error = "'" + file.generic_string() + "' has a truncated chunk";
                return false;
            }
            if (!hasJson){
                if (chunkType != GLB_CHUNK_JSON){
                    error = "'" + file.generic_string() + "' does not start with its JSON chunk";
                    return false;
                }
                text.assign(reinterpret_cast<const char*>(bytes.data() + offset), chunkLength);
                hasJson = true;
            }else if (chunkType == GLB_CHUNK_BIN && !hasBinChunk){
                binChunk.assign(bytes.begin() + (long)offset, bytes.begin() + (long)(offset + chunkLength));
                hasBinChunk = true;
            }
            offset += chunkLength;
        }
        if (!hasJson){
            error = "'" + file.generic_string() + "' has no JSON chunk";
            return false;
        }
    }else{
        text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    json = Json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()){
        json = Json();
        error = "'" + file.generic_string() + "' is not valid glTF JSON";
        return false;
    }
    auto asset = json.find("asset");
    if (asset == json.end() || !asset->is_object() || !asset->contains("version") || !(*asset)["version"].is_string() ||
            (*asset)["version"].get<std::string>().rfind("2.", 0) != 0){
        error = "'" + file.generic_string() + "' is not a glTF 2.0 file";
        return false;
    }
    for (const char* extension : COMPRESSION_EXTENSIONS){
        if (listsExtension(json, "extensionsUsed", extension) || listsExtension(json, "extensionsRequired", extension)){
            error = "'" + file.generic_string() + "' is compressed with " + extension +
                    ", which the bake cannot decode and rewrite: export the model uncompressed";
            return false;
        }
    }

    if (const Json* list = arrayOf(json, "buffers")){
        for (size_t b = 0; b < list->size(); b++){
            const Json& buffer = (*list)[b];
            size_t byteLength = 0;
            if (!getIndex(buffer, "byteLength", byteLength)){
                error = "buffer " + std::to_string(b) + " of '" + file.generic_string() + "' has no byteLength";
                return false;
            }
            std::vector<uint8_t> data;
            auto uri = buffer.find("uri");
            if (uri == buffer.end()){
                if (b != 0 || !hasBinChunk){
                    error = "buffer " + std::to_string(b) + " of '" + file.generic_string() + "' has no data";
                    return false;
                }
                data = binChunk;
            }else{
                if (!uri->is_string()){
                    error = "buffer " + std::to_string(b) + " of '" + file.generic_string() + "' has an invalid uri";
                    return false;
                }
                const std::string value = uri->get<std::string>();
                if (value.rfind("data:", 0) == 0){
                    const size_t comma = value.find(',');
                    if (comma == std::string::npos || value.substr(0, comma).find(";base64") == std::string::npos ||
                            !decodeBase64(value, comma + 1, data)){
                        error = "buffer " + std::to_string(b) + " of '" + file.generic_string() + "' has a data URI that is not base64";
                        return false;
                    }
                }else{
                    const fs::path path = file.parent_path() / editor::FileUtils::pathFromUtf8(decodeUri(value));
                    if (!readFile(path, data)){
                        error = "cannot read '" + path.generic_string() + "', buffer " + std::to_string(b) + " of '" + file.generic_string() + "'";
                        return false;
                    }
                }
            }
            if (data.size() < byteLength){
                error = "buffer " + std::to_string(b) + " of '" + file.generic_string() + "' is shorter than its byteLength";
                return false;
            }
            data.resize(byteLength);
            buffers.push_back(std::move(data));
        }
    }
    return true;
}

bool LatticeBakeGltf::accessorLayout(int accessor, int& componentType, int& components, size_t& count, std::string& error) const{
    const Json* accessors = arrayOf(json, "accessors");
    if (!accessors || accessor < 0 || (size_t)accessor >= accessors->size() || !(*accessors)[(size_t)accessor].is_object()){
        error = "accessor " + std::to_string(accessor) + " does not exist";
        return false;
    }
    const Json& a = (*accessors)[(size_t)accessor];
    size_t type = 0;
    if (!getIndex(a, "componentType", type) || componentSize((int)type) == 0){
        error = "accessor " + std::to_string(accessor) + " has an invalid componentType";
        return false;
    }
    componentType = (int)type;
    auto typeIt = a.find("type");
    components = (typeIt != a.end() && typeIt->is_string()) ? componentCount(typeIt->get<std::string>()) : 0;
    if (components == 0){
        error = "accessor " + std::to_string(accessor) + " has an invalid type";
        return false;
    }
    if (!getIndex(a, "count", count) || count == 0){
        error = "accessor " + std::to_string(accessor) + " has an invalid count";
        return false;
    }
    return true;
}

bool LatticeBakeGltf::readAccessor(int accessor, std::vector<float>& values, int& components, std::string& error) const{
    int componentType = 0;
    size_t count = 0;
    if (!accessorLayout(accessor, componentType, components, count, error)){
        return false;
    }
    const Json& a = json["accessors"][(size_t)accessor];
    const std::string label = "accessor " + std::to_string(accessor);
    const size_t size = (size_t)componentSize(componentType);
    if (components > 4 && size < 4){
        error = label + " is a padded matrix, which the bake does not read";
        return false;
    }
    if (count > std::numeric_limits<size_t>::max() / ((size_t)components * 4)){
        error = label + " is too large";
        return false;
    }
    const size_t elementSize = size * (size_t)components;
    const bool normalized = a.contains("normalized") && a["normalized"].is_boolean() && a["normalized"].get<bool>();
    const Json* views = arrayOf(json, "bufferViews");

    // the bytes [offset, offset + bytes) of a bufferView, inside its buffer
    auto viewData = [&](size_t viewIndex, size_t offset, size_t bytes, size_t& stride, const uint8_t*& data) -> bool {
        if (!views || viewIndex >= views->size()){
            error = label + " names bufferView " + std::to_string(viewIndex) + ", which does not exist";
            return false;
        }
        const Json& view = (*views)[viewIndex];
        size_t buffer = 0;
        size_t viewLength = 0;
        if (!getIndex(view, "buffer", buffer) || buffer >= buffers.size() || !getIndex(view, "byteLength", viewLength)){
            error = "bufferView " + std::to_string(viewIndex) + " is invalid";
            return false;
        }
        const size_t viewOffset = getSize(view, "byteOffset", 0);
        stride = getSize(view, "byteStride", 0);
        if (viewOffset > buffers[buffer].size() || viewLength > buffers[buffer].size() - viewOffset ||
                offset > viewLength || bytes > viewLength - offset){
            error = label + " reads past the end of bufferView " + std::to_string(viewIndex);
            return false;
        }
        data = buffers[buffer].data() + viewOffset + offset;
        return true;
    };

    values.assign(count * (size_t)components, 0.0f);

    size_t viewIndex = 0;
    if (getIndex(a, "bufferView", viewIndex)){
        const size_t byteOffset = getSize(a, "byteOffset", 0);
        size_t stride = 0;
        const uint8_t* probe = nullptr;
        if (!viewData(viewIndex, 0, 0, stride, probe)){
            return false;
        }
        if (stride == 0){
            stride = elementSize;
        }
        if (stride < elementSize || count - 1 > (std::numeric_limits<size_t>::max() - elementSize) / stride){
            error = label + " has an invalid byteStride";
            return false;
        }
        const uint8_t* data = nullptr;
        if (!viewData(viewIndex, byteOffset, (count - 1) * stride + elementSize, stride, data)){
            return false;
        }
        if (stride == 0){
            stride = elementSize;
        }
        for (size_t i = 0; i < count; i++){
            const uint8_t* element = data + i * stride;
            for (int c = 0; c < components; c++){
                values[i * (size_t)components + (size_t)c] = readComponent(element + (size_t)c * size, componentType, normalized);
            }
        }
    }else if (a.contains("bufferView")){
        error = label + " has an invalid bufferView";
        return false;
    }

    auto sparseIt = a.find("sparse");
    if (sparseIt != a.end()){
        const Json& sparse = *sparseIt;
        size_t sparseCount = 0;
        if (!getIndex(sparse, "count", sparseCount) || sparseCount == 0 || sparseCount > count ||
                !sparse.contains("indices") || !sparse.contains("values")){
            error = label + " has invalid sparse data";
            return false;
        }
        const Json& indices = sparse["indices"];
        const Json& sparseValues = sparse["values"];
        size_t indexView = 0, valueView = 0, indexType = 0;
        if (!getIndex(indices, "bufferView", indexView) || !getIndex(indices, "componentType", indexType) ||
                (indexType != 5121 && indexType != 5123 && indexType != 5125) || !getIndex(sparseValues, "bufferView", valueView)){
            error = label + " has invalid sparse data";
            return false;
        }
        const size_t indexSize = (size_t)componentSize((int)indexType);
        size_t unusedStride = 0;
        const uint8_t* indexData = nullptr;
        const uint8_t* valueData = nullptr;
        if (!viewData(indexView, getSize(indices, "byteOffset", 0), sparseCount * indexSize, unusedStride, indexData) ||
                !viewData(valueView, getSize(sparseValues, "byteOffset", 0), sparseCount * elementSize, unusedStride, valueData)){
            return false;
        }
        for (size_t s = 0; s < sparseCount; s++){
            const uint32_t index = readIndex(indexData + s * indexSize, (int)indexType);
            if (index >= count){
                error = label + " has a sparse index past its count";
                return false;
            }
            for (int c = 0; c < components; c++){
                values[(size_t)index * (size_t)components + (size_t)c] =
                    readComponent(valueData + s * elementSize + (size_t)c * size, componentType, normalized);
            }
        }
    }
    return true;
}

void LatticeBakeGltf::setAttribute(int node, const Slot& slot, std::vector<float> values, int components){
    Data& data = outputs[node][slot];
    data.components = components;
    data.values = std::move(values);
}

bool LatticeBakeGltf::saveBinary(const fs::path& file, std::string& error){
    if (!json.is_object()){
        error = "no glTF loaded";
        return false;
    }
    for (const auto& [node, slots] : outputs){
        for (const auto& [slot, data] : slots){
            for (float value : data.values){
                if (!std::isfinite(value)){
                    error = "the deformation gives a vertex of node " + std::to_string(node) + " a non-finite " + slot.semantic;
                    return false;
                }
            }
        }
    }

    // ---- one buffer: the old ones one after the other (16-byte aligned), new data after them
    std::vector<uint8_t> bin;
    std::vector<size_t> bases(buffers.size(), 0);
    for (size_t b = 0; b < buffers.size(); b++){
        bin.resize((bin.size() + 15) & ~(size_t)15, 0);
        bases[b] = bin.size();
        bin.insert(bin.end(), buffers[b].begin(), buffers[b].end());
    }
    if (!json.contains("bufferViews") || !json["bufferViews"].is_array()){
        json["bufferViews"] = Json::array();
    }
    Json& views = json["bufferViews"];
    for (size_t v = 0; v < views.size(); v++){
        Json& view = views[v];
        size_t buffer = 0;
        if (!getIndex(view, "buffer", buffer) || buffer >= buffers.size()){
            error = "bufferView " + std::to_string(v) + " names a buffer that does not exist";
            return false;
        }
        const size_t offset = bases[buffer] + getSize(view, "byteOffset", 0);
        view["buffer"] = 0;
        if (offset != 0 || view.contains("byteOffset")){
            view["byteOffset"] = offset;
        }
    }

    if (!json.contains("accessors") || !json["accessors"].is_array()){
        error = "the file has no accessors";
        return false;
    }
    Json& accessors = json["accessors"];
    const size_t accessorCount = accessors.size();

    // ---- meshes: a node whose data differs from the other nodes using its mesh gets a copy
    std::map<std::pair<int, Slot>, const Data*> meshOutputs;
    {
        if (!json.contains("nodes") || !json["nodes"].is_array() || !json.contains("meshes") || !json["meshes"].is_array()){
            if (!outputs.empty()){
                error = "the file has no meshes";
                return false;
            }
        }else{
            Json& nodes = json["nodes"];
            std::map<size_t, std::vector<size_t>> nodesOfMesh;
            for (size_t n = 0; n < nodes.size(); n++){
                size_t mesh = 0;
                if (getIndex(nodes[n], "mesh", mesh) && mesh < json["meshes"].size()){
                    nodesOfMesh[mesh].push_back(n);
                }
            }
            for (const auto& [node, slots] : outputs){
                size_t mesh = 0;
                if (node < 0 || (size_t)node >= nodes.size() || !getIndex(nodes[(size_t)node], "mesh", mesh) || mesh >= json["meshes"].size()){
                    error = "node " + std::to_string(node) + " has no mesh";
                    return false;
                }
            }
            static const std::map<Slot, Data> unchanged;
            for (const auto& [mesh, nodeList] : nodesOfMesh){
                // the nodes keeping their data come first, so they keep the mesh and the copies
                // go to the nodes with new data
                std::vector<size_t> ordered;
                for (size_t node : nodeList){
                    if (!outputs.count((int)node)) ordered.push_back(node);
                }
                for (size_t node : nodeList){
                    if (outputs.count((int)node)) ordered.push_back(node);
                }
                std::vector<std::pair<const std::map<Slot, Data>*, size_t>> groups;
                for (size_t node : ordered){
                    auto found = outputs.find((int)node);
                    const std::map<Slot, Data>* data = (found != outputs.end()) ? &found->second : &unchanged;
                    size_t meshIndex = mesh;
                    bool grouped = false;
                    for (const auto& group : groups){
                        const std::map<Slot, Data>& other = *group.first;
                        bool same = other.size() == data->size();
                        for (auto a = other.begin(), b = data->begin(); same && a != other.end(); ++a, ++b){
                            same = !(a->first < b->first) && !(b->first < a->first) && a->second.sameAs(b->second);
                        }
                        if (same){
                            meshIndex = group.second;
                            grouped = true;
                            break;
                        }
                    }
                    if (!grouped){
                        if (!groups.empty()){
                            Json copy = json["meshes"][mesh];
                            json["meshes"].push_back(std::move(copy));
                            meshIndex = json["meshes"].size() - 1;
                        }
                        groups.push_back({data, meshIndex});
                    }
                    if (meshIndex != mesh){
                        nodes[node]["mesh"] = meshIndex;
                    }
                }
                for (const auto& group : groups){
                    for (const auto& [slot, data] : *group.first){
                        meshOutputs[{(int)group.second, slot}] = &data;
                    }
                }
            }
        }
    }

    // every slot given data must exist in its primitive
    for (const auto& [key, data] : meshOutputs){
        const Json& mesh = json["meshes"][(size_t)key.first];
        const Slot& slot = key.second;
        bool valid = mesh.contains("primitives") && mesh["primitives"].is_array() && slot.primitive >= 0 &&
                     (size_t)slot.primitive < mesh["primitives"].size();
        if (valid){
            const Json& primitive = mesh["primitives"][(size_t)slot.primitive];
            if (slot.target < 0){
                valid = primitive.contains("attributes") && primitive["attributes"].is_object() && primitive["attributes"].contains(slot.semantic);
            }else{
                valid = primitive.contains("targets") && primitive["targets"].is_array() && (size_t)slot.target < primitive["targets"].size() &&
                        primitive["targets"][(size_t)slot.target].is_object() && primitive["targets"][(size_t)slot.target].contains(slot.semantic);
            }
        }
        if (!valid){
            error = "mesh " + std::to_string(key.first) + " has no " + slot.semantic + " in primitive " + std::to_string(slot.primitive) +
                    (slot.target >= 0 ? " target " + std::to_string(slot.target) : std::string());
            return false;
        }
    }

    // ---- who reads each accessor: primitive attributes (that may get new data) and the rest
    struct Ref{
        int mesh;
        Slot slot;
    };
    std::vector<std::vector<Ref>> attributeRefs(accessorCount);
    std::vector<bool> otherUse(accessorCount, false);
    auto markOther = [&](const Json& object, const char* key){
        size_t index = 0;
        if (getIndex(object, key, index) && index < accessorCount) otherUse[index] = true;
    };
    if (const Json* meshes = arrayOf(json, "meshes")){
        for (size_t m = 0; m < meshes->size(); m++){
            const Json* primitives = arrayOf((*meshes)[m], "primitives");
            if (!primitives) continue;
            for (size_t p = 0; p < primitives->size(); p++){
                const Json& primitive = (*primitives)[p];
                if (!primitive.is_object()) continue;
                markOther(primitive, "indices");
                auto attributes = primitive.find("attributes");
                if (attributes != primitive.end() && attributes->is_object()){
                    for (auto it = attributes->begin(); it != attributes->end(); ++it){
                        size_t index = 0;
                        if (getIndex(*attributes, it.key().c_str(), index) && index < accessorCount)
                            attributeRefs[index].push_back({(int)m, {(int)p, -1, it.key()}});
                    }
                }
                if (const Json* targets = arrayOf(primitive, "targets")){
                    for (size_t t = 0; t < targets->size(); t++){
                        const Json& target = (*targets)[t];
                        if (!target.is_object()) continue;
                        for (auto it = target.begin(); it != target.end(); ++it){
                            size_t index = 0;
                            if (getIndex(target, it.key().c_str(), index) && index < accessorCount)
                                attributeRefs[index].push_back({(int)m, {(int)p, (int)t, it.key()}});
                        }
                    }
                }
            }
        }
    }
    if (const Json* skins = arrayOf(json, "skins")){
        for (const Json& skin : *skins) markOther(skin, "inverseBindMatrices");
    }
    if (const Json* animations = arrayOf(json, "animations")){
        for (const Json& animation : *animations){
            if (const Json* samplers = arrayOf(animation, "samplers")){
                for (const Json& sampler : *samplers){
                    markOther(sampler, "input");
                    markOther(sampler, "output");
                }
            }
        }
    }
    if (const Json* nodes = arrayOf(json, "nodes")){
        for (const Json& node : *nodes){
            if (!node.is_object() || !node.contains("extensions") || !node["extensions"].is_object()) continue;
            const Json& extensions = node["extensions"];
            if (extensions.contains("EXT_mesh_gpu_instancing") && extensions["EXT_mesh_gpu_instancing"].is_object() &&
                    extensions["EXT_mesh_gpu_instancing"].contains("attributes") && extensions["EXT_mesh_gpu_instancing"]["attributes"].is_object()){
                const Json& attributes = extensions["EXT_mesh_gpu_instancing"]["attributes"];
                for (auto it = attributes.begin(); it != attributes.end(); ++it) markOther(attributes, it.key().c_str());
            }
        }
    }

    // ---- the bytes every accessor, sparse part and image occupies (for the in-place check)
    struct Span{
        size_t view = SIZE_MAX; // SIZE_MAX: not an interleavable element range
        size_t start = 0;
        size_t end = 0;
        size_t stride = 0;
        size_t inStride = 0;
        size_t element = 0;
    };
    auto viewRange = [&](size_t viewIndex, size_t& start, size_t& length) -> bool {
        if (viewIndex >= views.size()) return false;
        start = getSize(views[viewIndex], "byteOffset", 0);
        length = getSize(views[viewIndex], "byteLength", 0);
        return start <= bin.size() && length <= bin.size() - start;
    };
    std::vector<std::vector<Span>> accessorSpans(accessorCount);
    std::vector<Span> imageSpans;
    for (size_t a = 0; a < accessorCount; a++){
        const Json& accessor = accessors[a];
        int componentType = 0, components = 0;
        size_t count = 0;
        std::string ignored;
        if (!accessorLayout((int)a, componentType, components, count, ignored)) continue;
        const size_t elementSize = (size_t)componentSize(componentType) * (size_t)components;
        size_t viewIndex = 0, viewStart = 0, viewLength = 0;
        if (getIndex(accessor, "bufferView", viewIndex) && viewRange(viewIndex, viewStart, viewLength)){
            Span span;
            span.view = viewIndex;
            span.stride = getSize(views[viewIndex], "byteStride", 0);
            if (span.stride == 0) span.stride = elementSize;
            const size_t offset = getSize(accessor, "byteOffset", 0);
            span.start = viewStart + offset;
            span.end = span.start + (count - 1) * span.stride + elementSize;
            span.inStride = offset % span.stride;
            span.element = elementSize;
            accessorSpans[a].push_back(span);
        }
        if (accessor.contains("sparse") && accessor["sparse"].is_object()){
            for (const char* part : {"indices", "values"}){
                const Json& sparse = accessor["sparse"];
                if (!sparse.contains(part) || !getIndex(sparse[part], "bufferView", viewIndex) || !viewRange(viewIndex, viewStart, viewLength)) continue;
                Span span;
                span.start = viewStart;
                span.end = viewStart + viewLength;
                accessorSpans[a].push_back(span);
            }
        }
    }
    if (const Json* images = arrayOf(json, "images")){
        for (const Json& image : *images){
            size_t viewIndex = 0, viewStart = 0, viewLength = 0;
            if (getIndex(image, "bufferView", viewIndex) && viewRange(viewIndex, viewStart, viewLength)){
                Span span;
                span.start = viewStart;
                span.end = viewStart + viewLength;
                imageSpans.push_back(span);
            }
        }
    }
    auto overlaps = [](const Span& a, const Span& b){
        if (a.end <= b.start || b.end <= a.start) return false;
        // interleaved attributes of one bufferView share its range, not its bytes
        if (a.view != SIZE_MAX && a.view == b.view && a.stride == b.stride &&
                a.inStride + a.element <= a.stride && b.inStride + b.element <= b.stride){
            return a.inStride < b.inStride + b.element && b.inStride < a.inStride + a.element;
        }
        return true;
    };

    auto setMinMax = [](Json& accessor, const Data& data){
        Json min = Json::array();
        Json max = Json::array();
        const size_t count = data.values.size() / (size_t)data.components;
        for (int c = 0; c < data.components; c++){
            float lo = std::numeric_limits<float>::max();
            float hi = -std::numeric_limits<float>::max();
            for (size_t i = 0; i < count; i++){
                const float v = data.values[i * (size_t)data.components + (size_t)c];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            min.push_back((double)lo);
            max.push_back((double)hi);
        }
        accessor["min"] = std::move(min);
        accessor["max"] = std::move(max);
    };

    auto appendView = [&](const float* values, size_t floats, bool vertexAttribute) -> size_t {
        bin.resize((bin.size() + 3) & ~(size_t)3, 0);
        const size_t offset = bin.size();
        bin.resize(offset + floats * sizeof(float));
        if (floats > 0){
            std::memcpy(bin.data() + offset, values, floats * sizeof(float));
        }
        Json view = Json::object();
        view["buffer"] = 0;
        view["byteOffset"] = offset;
        view["byteLength"] = floats * sizeof(float);
        if (vertexAttribute){
            view["target"] = TARGET_ARRAY_BUFFER;
        }
        views.push_back(std::move(view));
        return views.size() - 1;
    };

    // ---- each accessor some primitive gets new data for: in place, or new accessors
    for (size_t a = 0; a < accessorCount; a++){
        std::vector<std::pair<const Data*, std::vector<const Ref*>>> groups;
        bool keepsOldData = otherUse[a];
        for (const Ref& ref : attributeRefs[a]){
            auto found = meshOutputs.find({ref.mesh, ref.slot});
            if (found == meshOutputs.end()){
                keepsOldData = true;
                continue;
            }
            bool grouped = false;
            for (auto& group : groups){
                if (group.first->sameAs(*found->second)){
                    group.second.push_back(&ref);
                    grouped = true;
                    break;
                }
            }
            if (!grouped){
                groups.push_back({found->second, {&ref}});
            }
        }
        if (groups.empty()){
            continue;
        }

        int componentType = 0, components = 0;
        size_t count = 0;
        if (!accessorLayout((int)a, componentType, components, count, error)){
            return false;
        }
        for (const auto& group : groups){
            if (group.first->components <= 0 || group.first->values.size() != count * (size_t)group.first->components){
                error = "the new " + group.second[0]->slot.semantic + " of accessor " + std::to_string(a) + " does not have its " +
                        std::to_string(count) + " elements";
                return false;
            }
        }

        Json& accessor = accessors[a];
        const bool isPosition = groups[0].second[0]->slot.semantic == "POSITION";

        bool inPlace = !keepsOldData && groups.size() == 1 && componentType == COMPONENT_FLOAT &&
                       components == groups[0].first->components && !accessor.contains("sparse") &&
                       !(accessor.contains("normalized") && accessor["normalized"].is_boolean() && accessor["normalized"].get<bool>()) &&
                       accessorSpans[a].size() == 1 && accessorSpans[a][0].view != SIZE_MAX && accessorSpans[a][0].end <= bin.size();
        if (inPlace){
            const Span& span = accessorSpans[a][0];
            for (size_t other = 0; other < accessorCount && inPlace; other++){
                if (other == a) continue;
                for (const Span& otherSpan : accessorSpans[other]){
                    if (overlaps(span, otherSpan)){
                        inPlace = false;
                        break;
                    }
                }
            }
            for (const Span& image : imageSpans){
                if (inPlace && overlaps(span, image)) inPlace = false;
            }
        }

        if (inPlace){
            const Span& span = accessorSpans[a][0];
            const Data& data = *groups[0].first;
            for (size_t i = 0; i < count; i++){
                std::memcpy(bin.data() + span.start + i * span.stride, data.values.data() + i * (size_t)components, span.element);
            }
            if (isPosition || accessor.contains("min") || accessor.contains("max")){
                setMinMax(accessor, data);
            }
            continue;
        }

        // the new accessors go at the end of the list: a reference into it would dangle after
        // the first one (a second group of a shared accessor read freed memory)
        const Json source = accessor;
        for (const auto& group : groups){
            const Data& data = *group.first;
            const Slot& slot = group.second[0]->slot;
            Json created = Json::object();
            if (source.contains("name")){
                created["name"] = source["name"];
            }

            // a sparse morph target without a base keeps its sparsity: the displacement of a
            // vertex the target does not move stays exactly zero
            bool sparse = false;
            if (slot.target >= 0 && source.contains("sparse") && !source.contains("bufferView")){
                std::vector<bool> listed(count, false);
                std::vector<uint32_t> indices;
                const Json& sparseJson = source["sparse"];
                size_t sparseCount = 0, indexView = 0, indexType = 0, viewStart = 0, viewLength = 0;
                if (getIndex(sparseJson, "count", sparseCount) && sparseJson.contains("indices") &&
                        getIndex(sparseJson["indices"], "bufferView", indexView) && getIndex(sparseJson["indices"], "componentType", indexType) &&
                        componentSize((int)indexType) > 0 && viewRange(indexView, viewStart, viewLength)){
                    const size_t indexSize = (size_t)componentSize((int)indexType);
                    const size_t indexOffset = getSize(sparseJson["indices"], "byteOffset", 0);
                    if (indexOffset <= viewLength && sparseCount * indexSize <= viewLength - indexOffset){
                        sparse = true;
                        for (size_t s = 0; s < sparseCount && sparse; s++){
                            const uint32_t index = readIndex(bin.data() + viewStart + indexOffset + s * indexSize, (int)indexType);
                            if (index >= count){
                                sparse = false;
                            }else{
                                listed[index] = true;
                                indices.push_back(index);
                            }
                        }
                    }
                }
                for (size_t i = 0; i < count && sparse; i++){
                    if (listed[i]) continue;
                    for (int c = 0; c < data.components; c++){
                        if (data.values[i * (size_t)data.components + (size_t)c] != 0.0f){
                            sparse = false;
                            break;
                        }
                    }
                }
                if (sparse){
                    std::vector<float> packed;
                    packed.reserve(indices.size() * (size_t)data.components);
                    for (uint32_t index : indices){
                        for (int c = 0; c < data.components; c++){
                            packed.push_back(data.values[(size_t)index * (size_t)data.components + (size_t)c]);
                        }
                    }
                    const size_t valuesView = appendView(packed.data(), packed.size(), false);
                    created["componentType"] = COMPONENT_FLOAT;
                    created["count"] = count;
                    created["type"] = typeName(data.components);
                    Json sparseOut = Json::object();
                    sparseOut["count"] = sparseCount;
                    sparseOut["indices"] = sparseJson["indices"];
                    Json valuesJson = Json::object();
                    valuesJson["bufferView"] = valuesView;
                    sparseOut["values"] = std::move(valuesJson);
                    created["sparse"] = std::move(sparseOut);
                }
            }
            if (!sparse){
                created["bufferView"] = appendView(data.values.data(), data.values.size(), true);
                created["componentType"] = COMPONENT_FLOAT;
                created["count"] = count;
                created["type"] = typeName(data.components);
            }
            if (slot.semantic == "POSITION"){
                setMinMax(created, data);
            }
            accessors.push_back(std::move(created));
            const size_t createdIndex = accessors.size() - 1;

            for (const Ref* ref : group.second){
                Json& primitive = json["meshes"][(size_t)ref->mesh]["primitives"][(size_t)ref->slot.primitive];
                if (ref->slot.target < 0){
                    primitive["attributes"][ref->slot.semantic] = createdIndex;
                }else{
                    primitive["targets"][(size_t)ref->slot.target][ref->slot.semantic] = createdIndex;
                }
            }
        }
    }

    // ---- the single buffer, embedded
    bin.resize((bin.size() + 3) & ~(size_t)3, 0);
    Json buffer = Json::object();
    if (json.contains("buffers") && json["buffers"].is_array() && !json["buffers"].empty() && json["buffers"][0].is_object()){
        buffer = json["buffers"][0];
        buffer.erase("uri");
    }
    buffer["byteLength"] = bin.size();
    Json bufferList = Json::array();
    bufferList.push_back(std::move(buffer));
    json["buffers"] = std::move(bufferList);

    std::string text = json.dump();
    while (text.size() % 4 != 0){
        text.push_back(' ');
    }
    const size_t total = 12 + 8 + text.size() + 8 + bin.size();
    if (total > std::numeric_limits<uint32_t>::max()){
        error = "the baked model is larger than a glTF binary can hold (4 GB)";
        return false;
    }

    std::vector<uint8_t> out;
    out.reserve(total);
    appendU32(out, GLB_MAGIC);
    appendU32(out, 2);
    appendU32(out, (uint32_t)total);
    appendU32(out, (uint32_t)text.size());
    appendU32(out, GLB_CHUNK_JSON);
    out.insert(out.end(), text.begin(), text.end());
    appendU32(out, (uint32_t)bin.size());
    appendU32(out, GLB_CHUNK_BIN);
    out.insert(out.end(), bin.begin(), bin.end());

    std::error_code ec;
    if (fs::exists(file, ec)){
        error = "'" + file.generic_string() + "' already exists";
        return false;
    }
    // a new asset of the project: an atomic Document write (a failure leaves no half file)
    const io::WriteResult written = io::writeFileAtomic(file,
            std::string_view(reinterpret_cast<const char*>(out.data()), out.size()));
    if (!written.ok()){
        error = "cannot write '" + file.generic_string() + "': " + io::describeWriteError(written);
        return false;
    }
    return true;
}
