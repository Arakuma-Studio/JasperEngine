// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UnityYaml.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor::unity;

namespace {

    // "--- !u!198 &8694752719588448096 stripped"
    bool parseHeader(const std::string& line, Document& doc){
        if (line.compare(0, 7, "--- !u!") != 0) return false;
        const char* p = line.c_str() + 7;
        char* end = nullptr;
        doc.classId = (int)std::strtol(p, &end, 10);
        if (end == p) return false;
        const size_t amp = line.find('&');
        if (amp == std::string::npos) return false;
        doc.fileId = std::strtoll(line.c_str() + amp + 1, &end, 10);
        doc.stripped = line.find(" stripped", amp) != std::string::npos;
        return true;
    }

}

bool YamlFile::load(const std::filesystem::path& path, std::string& error){
    std::ifstream in(path, std::ios::binary);
    if (!in){
        error = "cannot read " + path.string();
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return parse(buffer.str(), error);
}

bool YamlFile::parse(const std::string& text, std::string& error){
    documents.clear();
    index.clear();
    std::istringstream in(text);
    std::string line;
    // the header of the document being read (its body is built at flush: a YAML::Node is never
    // assigned over, which would change the node it shares with a document already stored)
    Document header;
    bool open = false;
    std::string body;
    auto flush = [&]() -> bool {
        if (!open) return true;
        Document doc;
        doc.classId = header.classId;
        doc.fileId = header.fileId;
        doc.stripped = header.stripped;
        try {
            const YAML::Node root = YAML::Load(body);
            if (root && root.IsMap() && root.size() > 0){
                doc.typeName = root.begin()->first.as<std::string>();
                doc.body.reset(root.begin()->second);
            }
        } catch (const YAML::Exception& e){
            error = "YAML error in the document &" + std::to_string(doc.fileId) + ": " + e.what();
            return false;
        }
        index[doc.fileId] = documents.size();
        documents.push_back(doc);
        return true;
    };
    while (std::getline(in, line)){
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 4, "--- ") == 0){
            if (!flush()) return false;
            header.classId = 0;
            header.fileId = 0;
            header.stripped = false;
            body.clear();
            open = parseHeader(line, header);
            if (!open){
                error = "unexpected document header: " + line;
                return false;
            }
            continue;
        }
        if (!open) continue;   // %YAML / %TAG lines before the first document
        body += line;
        body += '\n';
    }
    return flush();
}

const Document* YamlFile::find(int64_t fileId) const{
    auto it = index.find(fileId);
    return it == index.end() ? nullptr : &documents[it->second];
}

Document* YamlFile::find(int64_t fileId){
    auto it = index.find(fileId);
    return it == index.end() ? nullptr : &documents[it->second];
}

bool jasperengine::editor::unity::scalarFloat(const YAML::Node& node, float& out){
    if (!node || !node.IsScalar()) return false;
    const std::string text = node.Scalar();
    if (text.empty()) return false;
    char* end = nullptr;
    // strtof reads "Infinity", "-Infinity", "inf" and "nan"
    out = std::strtof(text.c_str(), &end);
    return end != text.c_str();
}

float jasperengine::editor::unity::readFloat(const YAML::Node& node, const char* key, float fallback){
    float v;
    if (node && node.IsMap() && scalarFloat(node[key], v)) return v;
    return fallback;
}

int jasperengine::editor::unity::readInt(const YAML::Node& node, const char* key, int fallback){
    if (!node || !node.IsMap() || !node[key] || !node[key].IsScalar()) return fallback;
    const std::string text = node[key].Scalar();
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    return end == text.c_str() ? fallback : (int)v;
}

bool jasperengine::editor::unity::readBool(const YAML::Node& node, const char* key, bool fallback){
    return readInt(node, key, fallback ? 1 : 0) != 0;
}

std::string jasperengine::editor::unity::readString(const YAML::Node& node, const char* key, const std::string& fallback){
    if (!node || !node.IsMap() || !node[key] || !node[key].IsScalar()) return fallback;
    return node[key].Scalar();
}

Vector3 jasperengine::editor::unity::readVector3(const YAML::Node& node, const char* key, const Vector3& fallback){
    if (!node || !node.IsMap() || !node[key] || !node[key].IsMap()) return fallback;
    const YAML::Node v = node[key];
    return Vector3(readFloat(v, "x", fallback.x), readFloat(v, "y", fallback.y), readFloat(v, "z", fallback.z));
}

Quaternion jasperengine::editor::unity::readQuaternion(const YAML::Node& node, const char* key){
    if (!node || !node.IsMap() || !node[key] || !node[key].IsMap()) return Quaternion();
    const YAML::Node v = node[key];
    return Quaternion(readFloat(v, "w", 1.0f), readFloat(v, "x", 0.0f), readFloat(v, "y", 0.0f), readFloat(v, "z", 0.0f));
}

Vector4 jasperengine::editor::unity::readColor(const YAML::Node& node, const char* key, const Vector4& fallback){
    if (!node || !node.IsMap() || !node[key] || !node[key].IsMap()) return fallback;
    const YAML::Node v = node[key];
    return Vector4(readFloat(v, "r", fallback.x), readFloat(v, "g", fallback.y), readFloat(v, "b", fallback.z), readFloat(v, "a", fallback.w));
}

Ref jasperengine::editor::unity::toRef(const YAML::Node& node){
    Ref ref;
    if (!node || !node.IsMap()) return ref;
    if (node["fileID"] && node["fileID"].IsScalar()) ref.fileID = std::strtoll(node["fileID"].Scalar().c_str(), nullptr, 10);
    ref.guid = readString(node, "guid");
    ref.type = readInt(node, "type");
    return ref;
}

Ref jasperengine::editor::unity::readRef(const YAML::Node& node, const char* key){
    if (!node || !node.IsMap()) return Ref();
    return toRef(node[key]);
}
