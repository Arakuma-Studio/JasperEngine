// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What the project does with each model (spec S53 R10, R11, R20, R22), read from the scene and
// prefab files (each parsed again only when it changed) and the project's scripts:
//   - the submeshes a scene draws blended (a blend override): the cook keeps their triangle order;
//   - the runtime readers of a model's CPU copy (Read/Write Automatic): a Mesh Collider or a
//     convex hull from its mesh, a navigation surface baking at runtime, a particle mesh shape,
//     runtime LOD building, a skinned mesh (poses and retargeting read its vertices) and the
//     scripts that call a mesh-reading API (createMeshCollider, createConvexHullCollider, a
//     navigation bake);
//   - the scenes using it and how many GameObjects (the Audit's Used In, the scene scope).
// Main thread.

#pragma once

#include "model/ModelImportSettings.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct ModelDocumentUse {
        std::string document;           // project-relative .scene or .prefab
        bool scene = true;              // false: a prefab
        int objects = 0;                // GameObjects with a Model of the file
        bool lodEnabled = false;        // the scene draws mesh LOD
        std::set<int> blendedSubmeshes; // submesh indices a blend override marks
    };

    struct ModelUsage {
        std::vector<ModelDocumentUse> documents;
        // why its CPU copy is read at runtime ("Mesh Collider on Rock in scenes/Main.scene")
        std::vector<std::string> readers;

        int objects() const;
        // the cook's blended set: a submesh index of every mesh
        std::set<std::pair<int, int>> blended() const;
        // a scene using it draws mesh LOD
        bool anyLod() const;
    };

    // Read/Write's outcome for one model and platform
    struct ModelReadWriteDecision {
        bool keep = true;
        std::vector<std::string> reasons;   // empty when released
    };

    class ModelUsageIndex {
    public:
        static ModelUsageIndex& get();

        // reads the documents and scripts changed since the last call (cheap when none did)
        void refresh(Project& project);
        void clear();

        // `assetPath` is asset-relative ("models/rock.glb")
        ModelUsage usageOf(const std::string& assetPath) const;
        // "scripts/Spawner.lua calls createMeshCollider"
        const std::vector<std::string>& scriptReaders() const { return scripts; }
        // the documents the index read, project-relative
        std::vector<std::string> documents() const;
        // every model a document uses, asset-relative
        std::vector<std::string> modelsOf(const std::string& document) const;

        // Read/Write (R11): kept when the setting is On or something at runtime reads it.
        // `skinned` and `bakedLevels` come from the model's file and its cook.
        ModelReadWriteDecision decideReadWrite(const std::string& assetPath, ModelReadWrite setting, ScriptMeshData scripts,
                                               bool skinned, bool bakedLevels) const;
        // the same over a snapshot (an export's thread)
        static ModelReadWriteDecision decide(const ModelUsage& usage, const std::vector<std::string>& scriptReaders, ModelReadWrite setting,
                                             ScriptMeshData scripts, bool skinned, bool bakedLevels);

    private:
        struct Document {
            int64_t stamp = 0;
            uint64_t size = 0;
            bool scene = true;
            bool lodEnabled = false;
            // model (asset-relative) -> its use here
            std::map<std::string, ModelDocumentUse> models;
            std::map<std::string, std::vector<std::string>> readers;
        };
        struct Script {
            int64_t stamp = 0;
            uint64_t size = 0;
            std::vector<std::string> calls;
        };
        std::map<std::string, Document> docs;           // project-relative path
        std::map<std::string, Script> scriptFiles;
        std::vector<std::string> scripts;
        std::filesystem::path root;

        void readDocument(Project& project, const std::filesystem::path& absolute, const std::string& relative, Document& out);
    };

}
