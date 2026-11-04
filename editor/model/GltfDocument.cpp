// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GltfDocument.h"

#include "util/FileUtils.h"

#include "meshoptimizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    using Json = GltfDocument::Json;

    const uint32_t GLB_MAGIC = 0x46546C67;      // "glTF"
    const uint32_t GLB_CHUNK_JSON = 0x4E4F534A; // "JSON"
    const uint32_t GLB_CHUNK_BIN = 0x004E4942;  // "BIN\0"
    // what one compressed bufferView may decode to: a malformed count must not allocate the machine
    const size_t MAX_DECODED_VIEW = size_t(1) << 31;

    const char* const MESHOPT_EXTENSIONS[] = {"KHR_meshopt_compression", "EXT_meshopt_compression"};

    uint32_t readU32(const uint8_t* p){
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    void appendU32(std::vector<uint8_t>& out, uint32_t value){
        for (int i = 0; i < 4; i++)
            out.push_back((uint8_t)((value >> (8 * i)) & 0xFF));
    }

    bool readFile(const fs::path& path, std::vector<uint8_t>& data){
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
            return false;
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0)
            return false;
        in.seekg(0, std::ios::beg);
        data.resize((size_t)size);
        return size == 0 || (bool)in.read(reinterpret_cast<char*>(data.data()), size);
    }

    size_t getSize(const Json& object, const char* key, size_t fallback){
        size_t value = 0;
        return GltfDocument::getIndex(object, key, value) ? value : fallback;
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
            if (v < 0 || padding)
                return false;
            accumulator = (accumulator << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8){
                bits -= 8;
                out.push_back((uint8_t)((accumulator >> bits) & 0xFF));
            }
        }
        return true;
    }

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

    // the meshopt extension object of a JSON member, or null
    const Json* meshoptExtension(const Json& object){
        auto extensions = object.find("extensions");
        if (extensions == object.end() || !extensions->is_object())
            return nullptr;
        for (const char* name : MESHOPT_EXTENSIONS){
            auto it = extensions->find(name);
            if (it != extensions->end() && it->is_object())
                return &(*it);
        }
        return nullptr;
    }

    float readComponent(const uint8_t* p, int componentType, bool normalized){
        switch (componentType){
            case gltf::BYTE: { int8_t v; std::memcpy(&v, p, 1); return normalized ? std::max((float)v / 127.0f, -1.0f) : (float)v; }
            case gltf::UNSIGNED_BYTE: { uint8_t v = *p; return normalized ? (float)v / 255.0f : (float)v; }
            case gltf::SHORT: { int16_t v; std::memcpy(&v, p, 2); return normalized ? std::max((float)v / 32767.0f, -1.0f) : (float)v; }
            case gltf::UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, p, 2); return normalized ? (float)v / 65535.0f : (float)v; }
            case gltf::UNSIGNED_INT: { uint32_t v; std::memcpy(&v, p, 4); return (float)v; }
            default: { float v; std::memcpy(&v, p, 4); return v; }
        }
    }

    uint32_t readIndex(const uint8_t* p, int componentType){
        switch (componentType){
            case gltf::UNSIGNED_BYTE: return *p;
            case gltf::UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, p, 2); return v; }
            default: { uint32_t v; std::memcpy(&v, p, 4); return v; }
        }
    }

}

int gltf::componentSize(int componentType){
    switch (componentType){
        case BYTE: case UNSIGNED_BYTE: return 1;
        case SHORT: case UNSIGNED_SHORT: return 2;
        case UNSIGNED_INT: case FLOAT: return 4;
        default: return 0;
    }
}

int gltf::componentCount(const std::string& type){
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    return 0;
}

const char* gltf::typeName(int components){
    switch (components){
        case 1: return "SCALAR";
        case 2: return "VEC2";
        case 3: return "VEC3";
        default: return "VEC4";
    }
}

bool GltfDocument::getIndex(const Json& object, const char* key, size_t& out){
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

const Json* GltfDocument::arrayOf(const Json& root, const char* key){
    if (!root.is_object()) return nullptr;
    auto it = root.find(key);
    if (it == root.end() || !it->is_array()) return nullptr;
    return &(*it);
}

std::vector<std::string> GltfDocument::requiredExtensions() const{
    std::vector<std::string> out;
    if (const Json* list = arrayOf(root, "extensionsRequired"))
        for (const Json& item : *list)
            if (item.is_string()) out.push_back(item.get<std::string>());
    return out;
}

std::vector<std::string> GltfDocument::usedExtensions() const{
    std::vector<std::string> out;
    if (const Json* list = arrayOf(root, "extensionsUsed"))
        for (const Json& item : *list)
            if (item.is_string()) out.push_back(item.get<std::string>());
    return out;
}

bool GltfDocument::load(const fs::path& file, std::string& error, std::vector<uint8_t>* read){
    std::vector<uint8_t> bytes;
    if (!readFile(file, bytes)){
        error = "cannot read '" + file.generic_string() + "'";
        return false;
    }
    return loadMemory(bytes, file.parent_path(), file.filename().generic_string(), error, read);
}

bool GltfDocument::loadMemory(const std::vector<uint8_t>& bytes, const fs::path& baseDirectory, const std::string& name,
                              std::string& error, std::vector<uint8_t>* read){
    root = Json();
    buffers.clear();
    decodedViews.clear();
    label = name;
    baseDir = baseDirectory;
    if (read)
        read->insert(read->end(), bytes.begin(), bytes.end());

    std::vector<uint8_t> binChunk;
    bool hasBinChunk = false;
    std::string text;
    if (bytes.size() >= 12 && readU32(bytes.data()) == GLB_MAGIC){
        const uint32_t version = readU32(bytes.data() + 4);
        const uint32_t length = readU32(bytes.data() + 8);
        if (version != 2 || length > bytes.size() || length < 20){
            error = label + " is not a valid glTF 2.0 binary";
            return false;
        }
        size_t offset = 12;
        bool hasJson = false;
        while (offset + 8 <= length){
            const uint32_t chunkLength = readU32(bytes.data() + offset);
            const uint32_t chunkType = readU32(bytes.data() + offset + 4);
            offset += 8;
            if (chunkLength > length - offset){
                error = label + " has a truncated chunk";
                return false;
            }
            if (!hasJson){
                if (chunkType != GLB_CHUNK_JSON){
                    error = label + " does not start with its JSON chunk";
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
            error = label + " has no JSON chunk";
            return false;
        }
    }else{
        text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()){
        root = Json();
        error = label + " is not valid glTF JSON";
        return false;
    }
    auto asset = root.find("asset");
    if (asset == root.end() || !asset->is_object() || !asset->contains("version") || !(*asset)["version"].is_string() ||
            (*asset)["version"].get<std::string>().rfind("2.", 0) != 0){
        error = label + " is not a glTF 2.0 file";
        return false;
    }

    if (const Json* list = arrayOf(root, "buffers")){
        for (size_t b = 0; b < list->size(); b++){
            const Json& buffer = (*list)[b];
            size_t byteLength = 0;
            if (!getIndex(buffer, "byteLength", byteLength)){
                error = "buffer " + std::to_string(b) + " of " + label + " has no byteLength";
                return false;
            }
            std::vector<uint8_t> data;
            auto uri = buffer.find("uri");
            const Json* meshopt = meshoptExtension(buffer);
            const bool fallback = meshopt && meshopt->value("fallback", false);
            if (uri == buffer.end()){
                if (fallback){
                    // the decoded views' placeholder: never read, never the BIN chunk
                    buffers.emplace_back();
                    continue;
                }
                if (b != 0 || !hasBinChunk){
                    error = "buffer " + std::to_string(b) + " of " + label + " has no data";
                    return false;
                }
                data = binChunk;
            }else{
                if (!uri->is_string()){
                    error = "buffer " + std::to_string(b) + " of " + label + " has an invalid uri";
                    return false;
                }
                const std::string value = uri->get<std::string>();
                if (value.rfind("data:", 0) == 0){
                    const size_t comma = value.find(',');
                    if (comma == std::string::npos || value.substr(0, comma).find(";base64") == std::string::npos ||
                            !decodeBase64(value, comma + 1, data)){
                        error = "buffer " + std::to_string(b) + " of " + label + " has a data URI that is not base64";
                        return false;
                    }
                }else{
                    const fs::path path = baseDir / editor::FileUtils::pathFromUtf8(decodeUri(value));
                    if (!readFile(path, data)){
                        // a fallback with a file is optional data: the compressed views decode anyway
                        if (fallback){
                            buffers.emplace_back();
                            continue;
                        }
                        error = "cannot read " + path.filename().generic_string() + ", buffer " + std::to_string(b) + " of " + label;
                        return false;
                    }
                    if (read)
                        read->insert(read->end(), data.begin(), data.end());
                }
            }
            if (data.size() < byteLength){
                error = "buffer " + std::to_string(b) + " of " + label + " is shorter than its byteLength";
                return false;
            }
            data.resize(byteLength);
            buffers.push_back(std::move(data));
        }
    }
    return decodeCompressedViews(error);
}

bool GltfDocument::decodeCompressedViews(std::string& error){
    const Json* views = arrayOf(root, "bufferViews");
    if (!views)
        return true;
    decodedViews.assign(views->size(), {});
    for (size_t v = 0; v < views->size(); v++){
        const Json* ext = meshoptExtension((*views)[v]);
        if (!ext)
            continue;
        const std::string where = "bufferView " + std::to_string(v) + " of " + label;
        size_t buffer = 0, byteLength = 0, stride = 0, count = 0, viewLength = 0;
        if (!getIndex(*ext, "buffer", buffer) || !getIndex(*ext, "byteLength", byteLength) || !getIndex(*ext, "byteStride", stride) ||
                !getIndex(*ext, "count", count) || !getIndex((*views)[v], "byteLength", viewLength)){
            error = where + ": its meshopt compression lacks buffer, byteLength, byteStride or count";
            return false;
        }
        const size_t offset = getSize(*ext, "byteOffset", 0);
        const std::string mode = ext->value("mode", std::string());
        const std::string filter = ext->value("filter", std::string("NONE"));
        if (buffer >= buffers.size() || offset > buffers[buffer].size() || byteLength > buffers[buffer].size() - offset){
            error = where + ": its compressed data lies outside buffer " + std::to_string(buffer);
            return false;
        }
        const bool attributes = mode == "ATTRIBUTES";
        const bool triangles = mode == "TRIANGLES";
        const bool indices = mode == "INDICES";
        if (!attributes && !triangles && !indices){
            error = where + ": unknown meshopt mode '" + mode + "'";
            return false;
        }
        if (attributes ? (stride == 0 || stride > 256 || stride % 4 != 0) : (stride != 2 && stride != 4)){
            error = where + ": byteStride " + std::to_string(stride) + " is invalid for " + mode;
            return false;
        }
        if (count == 0 || count > MAX_DECODED_VIEW / stride || count * stride > viewLength){
            error = where + ": count " + std::to_string(count) + " does not fit its byteLength";
            return false;
        }
        if (triangles && count % 3 != 0){
            error = where + ": a TRIANGLES stream of " + std::to_string(count) + " indices";
            return false;
        }
        const bool filterOk = filter == "NONE" ||
            (attributes && filter == "OCTAHEDRAL" && (stride == 4 || stride == 8)) ||
            (attributes && filter == "QUATERNION" && stride == 8) ||
            (attributes && filter == "EXPONENTIAL" && stride % 4 == 0) ||
            (attributes && filter == "COLOR" && (stride == 4 || stride == 8));
        if (!filterOk){
            error = where + ": filter " + filter + " does not fit mode " + mode + " and byteStride " + std::to_string(stride);
            return false;
        }
        std::vector<uint8_t> out(viewLength, 0);
        const unsigned char* data = buffers[buffer].data() + offset;
        int result;
        if (attributes)
            result = meshopt_decodeVertexBuffer(out.data(), count, stride, data, byteLength);
        else if (triangles)
            result = meshopt_decodeIndexBuffer(out.data(), count, stride, data, byteLength);
        else
            result = meshopt_decodeIndexSequence(out.data(), count, stride, data, byteLength);
        if (result != 0){
            error = where + ": its meshopt " + mode + " stream is corrupt (decoder error " + std::to_string(result) + ")";
            return false;
        }
        if (filter == "OCTAHEDRAL") meshopt_decodeFilterOct(out.data(), count, stride);
        else if (filter == "QUATERNION") meshopt_decodeFilterQuat(out.data(), count, stride);
        else if (filter == "EXPONENTIAL") meshopt_decodeFilterExp(out.data(), count, stride);
        else if (filter == "COLOR") meshopt_decodeFilterColor(out.data(), count, stride);
        decodedViews[v] = std::move(out);
    }
    return true;
}

void GltfDocument::prepareViews(){
    for (const char* key : {"buffers", "bufferViews"}){
        if (!root.contains(key) || !root[key].is_array())
            root[key] = Json::array();
    }
}

void GltfDocument::pruneViews(){
    for (const char* key : {"buffers", "bufferViews"}){
        if (root.contains(key) && root[key].is_array() && root[key].empty())
            root.erase(key);
    }
}

size_t GltfDocument::addView(std::vector<uint8_t> bytes, size_t stride, int target){
    if (!root.contains("buffers") || !root["buffers"].is_array())
        root["buffers"] = Json::array();
    if (!root.contains("bufferViews") || !root["bufferViews"].is_array())
        root["bufferViews"] = Json::array();
    const size_t length = bytes.size();
    root["buffers"].push_back(Json{{"byteLength", length}});
    Json view = {{"buffer", root["buffers"].size() - 1}, {"byteLength", length}};
    if (stride > 0)
        view["byteStride"] = stride;
    if (target != 0)
        view["target"] = target;
    root["bufferViews"].push_back(std::move(view));
    buffers.push_back(std::move(bytes));
    decodedViews.resize(root["bufferViews"].size());
    return root["bufferViews"].size() - 1;
}

bool GltfDocument::imageBytes(size_t image, std::vector<uint8_t>& out, std::string& error) const{
    const Json* images = arrayOf(root, "images");
    if (!images || image >= images->size() || !(*images)[image].is_object()){
        error = "image " + std::to_string(image) + " of " + label + " does not exist";
        return false;
    }
    const Json& object = (*images)[image];
    size_t view = 0;
    if (getIndex(object, "bufferView", view)){
        const uint8_t* data = nullptr;
        size_t size = 0, stride = 0;
        if (!viewData(view, data, size, stride, error))
            return false;
        out.assign(data, data + size);
        return true;
    }
    auto uri = object.find("uri");
    if (uri == object.end() || !uri->is_string()){
        error = "image " + std::to_string(image) + " of " + label + " has neither a bufferView nor a uri";
        return false;
    }
    const std::string value = uri->get<std::string>();
    if (value.rfind("data:", 0) == 0){
        const size_t comma = value.find(',');
        if (comma == std::string::npos || value.substr(0, comma).find(";base64") == std::string::npos || !decodeBase64(value, comma + 1, out)){
            error = "image " + std::to_string(image) + " of " + label + " has a data URI that is not base64";
            return false;
        }
        return true;
    }
    const fs::path path = baseDir / editor::FileUtils::pathFromUtf8(decodeUri(value));
    if (!readFile(path, out)){
        error = "cannot read " + path.filename().generic_string() + ", image " + std::to_string(image) + " of " + label;
        return false;
    }
    return true;
}

bool GltfDocument::viewData(size_t view, const uint8_t*& data, size_t& size, size_t& stride, std::string& error) const{
    const Json* views = arrayOf(root, "bufferViews");
    if (!views || view >= views->size() || !(*views)[view].is_object()){
        error = "bufferView " + std::to_string(view) + " of " + label + " does not exist";
        return false;
    }
    const Json& v = (*views)[view];
    stride = getSize(v, "byteStride", 0);
    if (view < decodedViews.size() && !decodedViews[view].empty()){
        data = decodedViews[view].data();
        size = decodedViews[view].size();
        return true;
    }
    size_t buffer = 0, length = 0;
    if (!getIndex(v, "buffer", buffer) || buffer >= buffers.size() || !getIndex(v, "byteLength", length)){
        error = "bufferView " + std::to_string(view) + " of " + label + " is invalid";
        return false;
    }
    const size_t offset = getSize(v, "byteOffset", 0);
    if (offset > buffers[buffer].size() || length > buffers[buffer].size() - offset){
        error = "bufferView " + std::to_string(view) + " of " + label + " lies outside its buffer";
        return false;
    }
    data = buffers[buffer].data() + offset;
    size = length;
    return true;
}

bool GltfDocument::accessorLayout(size_t accessor, GltfAccessorLayout& out, std::string& error) const{
    const Json* accessors = arrayOf(root, "accessors");
    if (!accessors || accessor >= accessors->size() || !(*accessors)[accessor].is_object()){
        error = "accessor " + std::to_string(accessor) + " of " + label + " does not exist";
        return false;
    }
    const Json& a = (*accessors)[accessor];
    out = GltfAccessorLayout();
    size_t type = 0;
    if (!getIndex(a, "componentType", type) || gltf::componentSize((int)type) == 0){
        error = "accessor " + std::to_string(accessor) + " of " + label + " has an invalid componentType";
        return false;
    }
    out.componentType = (int)type;
    auto typeIt = a.find("type");
    out.components = (typeIt != a.end() && typeIt->is_string()) ? gltf::componentCount(typeIt->get<std::string>()) : 0;
    if (out.components == 0){
        error = "accessor " + std::to_string(accessor) + " of " + label + " has an invalid type";
        return false;
    }
    if (!getIndex(a, "count", out.count) || out.count == 0){
        error = "accessor " + std::to_string(accessor) + " of " + label + " has an invalid count";
        return false;
    }
    out.normalized = a.contains("normalized") && a["normalized"].is_boolean() && a["normalized"].get<bool>();
    out.elementSize = (size_t)gltf::componentSize(out.componentType) * (size_t)out.components;
    out.sparse = a.contains("sparse");
    out.hasView = a.contains("bufferView");
    return true;
}

bool GltfDocument::readAccessorRaw(size_t accessor, std::vector<uint8_t>& out, GltfAccessorLayout& layout, std::string& error) const{
    if (!accessorLayout(accessor, layout, error))
        return false;
    const Json& a = root["accessors"][accessor];
    const std::string where = "accessor " + std::to_string(accessor) + " of " + label;
    if (layout.components > 4 && gltf::componentSize(layout.componentType) < 4){
        error = where + " is a padded matrix, which the cook does not read";
        return false;
    }
    if (layout.count > MAX_DECODED_VIEW / std::max<size_t>(layout.elementSize, 1)){
        error = where + " is too large";
        return false;
    }
    const size_t elementSize = layout.elementSize;
    out.assign(layout.count * elementSize, 0);

    size_t viewIndex = 0;
    if (getIndex(a, "bufferView", viewIndex)){
        const uint8_t* data = nullptr;
        size_t size = 0, stride = 0;
        if (!viewData(viewIndex, data, size, stride, error))
            return false;
        if (stride == 0)
            stride = elementSize;
        const size_t byteOffset = getSize(a, "byteOffset", 0);
        // byteOffset + (count - 1) * stride + elementSize <= size, without overflow
        if (stride < elementSize || byteOffset > size || size - byteOffset < elementSize ||
                (layout.count - 1) > (size - byteOffset - elementSize) / stride){
            error = where + " reads past the end of bufferView " + std::to_string(viewIndex);
            return false;
        }
        for (size_t i = 0; i < layout.count; i++)
            std::memcpy(out.data() + i * elementSize, data + byteOffset + i * stride, elementSize);
    }else if (a.contains("bufferView")){
        error = where + " has an invalid bufferView";
        return false;
    }

    auto sparseIt = a.find("sparse");
    if (sparseIt != a.end()){
        const Json& sparse = *sparseIt;
        size_t sparseCount = 0;
        if (!getIndex(sparse, "count", sparseCount) || sparseCount == 0 || sparseCount > layout.count ||
                !sparse.contains("indices") || !sparse.contains("values")){
            error = where + " has invalid sparse data";
            return false;
        }
        const Json& indices = sparse["indices"];
        const Json& values = sparse["values"];
        size_t indexView = 0, valueView = 0, indexType = 0;
        if (!getIndex(indices, "bufferView", indexView) || !getIndex(indices, "componentType", indexType) ||
                (indexType != gltf::UNSIGNED_BYTE && indexType != gltf::UNSIGNED_SHORT && indexType != gltf::UNSIGNED_INT) ||
                !getIndex(values, "bufferView", valueView)){
            error = where + " has invalid sparse data";
            return false;
        }
        const size_t indexSize = (size_t)gltf::componentSize((int)indexType);
        const uint8_t* indexData = nullptr;
        const uint8_t* valueData = nullptr;
        size_t indexSizeAvailable = 0, valueSizeAvailable = 0, unusedStride = 0;
        if (!viewData(indexView, indexData, indexSizeAvailable, unusedStride, error) ||
                !viewData(valueView, valueData, valueSizeAvailable, unusedStride, error))
            return false;
        const size_t indexOffset = getSize(indices, "byteOffset", 0), valueOffset = getSize(values, "byteOffset", 0);
        if (indexOffset > indexSizeAvailable || sparseCount * indexSize > indexSizeAvailable - indexOffset ||
                valueOffset > valueSizeAvailable || sparseCount * elementSize > valueSizeAvailable - valueOffset){
            error = where + "'s sparse data reads past its bufferViews";
            return false;
        }
        for (size_t s = 0; s < sparseCount; s++){
            const uint32_t index = readIndex(indexData + indexOffset + s * indexSize, (int)indexType);
            if (index >= layout.count){
                error = where + " has a sparse index past its count";
                return false;
            }
            std::memcpy(out.data() + (size_t)index * elementSize, valueData + valueOffset + s * elementSize, elementSize);
        }
    }
    return true;
}

bool GltfDocument::readAccessorFloats(size_t accessor, std::vector<float>& out, int& components, std::string& error) const{
    std::vector<uint8_t> raw;
    GltfAccessorLayout layout;
    if (!readAccessorRaw(accessor, raw, layout, error))
        return false;
    components = layout.components;
    const size_t size = (size_t)gltf::componentSize(layout.componentType);
    out.resize(layout.count * (size_t)layout.components);
    for (size_t i = 0; i < out.size(); i++)
        out[i] = readComponent(raw.data() + i * size, layout.componentType, layout.normalized);
    return true;
}

bool GltfDocument::readIndices(size_t accessor, std::vector<uint32_t>& out, std::string& error) const{
    std::vector<uint8_t> raw;
    GltfAccessorLayout layout;
    if (!readAccessorRaw(accessor, raw, layout, error))
        return false;
    if (layout.components != 1 || (layout.componentType != gltf::UNSIGNED_BYTE && layout.componentType != gltf::UNSIGNED_SHORT &&
                                   layout.componentType != gltf::UNSIGNED_INT)){
        error = "accessor " + std::to_string(accessor) + " of " + label + " is not an index list";
        return false;
    }
    out.resize(layout.count);
    for (size_t i = 0; i < layout.count; i++)
        out[i] = readIndex(raw.data() + i * layout.elementSize, layout.componentType);
    return true;
}

std::vector<uint8_t> GltfDocument::writeGlb(const Json& json, const std::vector<uint8_t>& bin){
    std::string text = json.dump();
    while (text.size() % 4 != 0)
        text.push_back(' ');
    const size_t binLength = (bin.size() + 3) / 4 * 4;
    const size_t total = 12 + 8 + text.size() + (bin.empty() ? 0 : 8 + binLength);
    std::vector<uint8_t> out;
    out.reserve(total);
    appendU32(out, GLB_MAGIC);
    appendU32(out, 2);
    appendU32(out, (uint32_t)total);
    appendU32(out, (uint32_t)text.size());
    appendU32(out, GLB_CHUNK_JSON);
    out.insert(out.end(), text.begin(), text.end());
    if (!bin.empty()){
        appendU32(out, (uint32_t)binLength);
        appendU32(out, GLB_CHUNK_BIN);
        out.insert(out.end(), bin.begin(), bin.end());
        out.resize(total, 0);
    }
    return out;
}
