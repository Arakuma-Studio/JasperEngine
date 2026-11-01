// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Unity's serialized YAML (particles spec 6.10.2): a `.prefab`, `.unity` or `.mat` file is many
// YAML documents, each headed `--- !u!<classId> &<fileId>` (`stripped` after it for the
// placeholders of a prefab instance's objects). The file is split by those headers and each body
// parsed by yaml-cpp; scalars stay strings until a reader types them (Unity writes
// `m_VertexStreams: 0001030405`, which a YAML 1.1 reader would take for an octal number).

#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine::editor::unity {

    // a reference: {fileID, guid, type}; a local one has no guid
    struct Ref {
        int64_t fileID = 0;
        std::string guid;
        int type = 0;

        bool isNull() const { return fileID == 0 && guid.empty(); }
        bool isLocal() const { return guid.empty(); }
    };

    struct Document {
        int classId = 0;
        int64_t fileId = 0;
        bool stripped = false;
        std::string typeName;   // "ParticleSystem", "GameObject", ...
        YAML::Node body;        // the map under typeName
    };

    class YamlFile {
    public:
        std::vector<Document> documents;

        bool load(const std::filesystem::path& path, std::string& error);
        bool parse(const std::string& text, std::string& error);

        const Document* find(int64_t fileId) const;
        Document* find(int64_t fileId);

    private:
        std::unordered_map<int64_t, size_t> index;
    };

    // Unity's well known class ids (the ones the importer reads)
    enum ClassId : int {
        CLASS_GAMEOBJECT = 1,
        CLASS_TRANSFORM = 4,
        CLASS_MATERIAL = 21,
        CLASS_MONOBEHAVIOUR = 114,
        CLASS_PARTICLESYSTEM = 198,
        CLASS_PARTICLESYSTEMRENDERER = 199,
        CLASS_RECTTRANSFORM = 224,
        CLASS_PREFABINSTANCE = 1001
    };

    // typed readers of a map's key (missing or malformed: the fallback)
    float readFloat(const YAML::Node& node, const char* key, float fallback = 0.0f);
    int readInt(const YAML::Node& node, const char* key, int fallback = 0);
    bool readBool(const YAML::Node& node, const char* key, bool fallback = false);
    std::string readString(const YAML::Node& node, const char* key, const std::string& fallback = "");
    Vector3 readVector3(const YAML::Node& node, const char* key, const Vector3& fallback = Vector3(0, 0, 0));
    // {x, y, z, w}
    Quaternion readQuaternion(const YAML::Node& node, const char* key);
    // {r, g, b, a} as stored (Unity's gamma colours)
    Vector4 readColor(const YAML::Node& node, const char* key, const Vector4& fallback = Vector4(1, 1, 1, 1));
    Ref readRef(const YAML::Node& node, const char* key);
    Ref toRef(const YAML::Node& node);
    // a scalar as a float ("Infinity", "-Infinity", "NaN" included)
    bool scalarFloat(const YAML::Node& node, float& out);

}
