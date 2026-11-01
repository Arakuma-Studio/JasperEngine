// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A Unity project read for the importer (particles spec 6.10.3): its root (the folder holding
// Assets/ and ProjectSettings/), the asset of each guid (from every .meta under Assets/), and
// Unity's built-in assets, which have no file: one table of the ids the importer knows.

#include "UnityYaml.h"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace jasperengine::editor::unity {

    // a built-in asset (guid 0000000000000000e000000000000000 for meshes,
    // 0000000000000000f000000000000000 for the other resources)
    struct BuiltinAsset {
        enum Kind { Mesh, Material, Shader, Texture } kind;
        const char* name;    // Unity's name ("Cube", "Default-Particle", "Mobile/Particles/Additive")
        const char* engine;  // a mesh's primitive ("cube"), else ""
    };

    class Project {
    public:
        // the nearest ancestor of `file` holding Assets/ and ProjectSettings/; empty without one
        static std::filesystem::path findRoot(const std::filesystem::path& file);

        bool open(const std::filesystem::path& root, std::string& error);
        const std::filesystem::path& root() const { return projectRoot; }

        // the asset of a guid (absolute), empty when unknown
        std::filesystem::path pathOf(const std::string& guid) const;
        // a path relative to the project root ("Assets/...")
        std::string relative(const std::filesystem::path& path) const;
        // the .meta of an asset, parsed (null node when missing)
        YAML::Node metaOf(const std::filesystem::path& asset) const;

        // the built-in asset of a reference, null when it is not one or its id is unknown
        static const BuiltinAsset* builtin(const Ref& ref);
        static bool isBuiltinGuid(const std::string& guid);

    private:
        std::filesystem::path projectRoot;
        std::unordered_map<std::string, std::filesystem::path> guids;
    };

}
