// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// One model's numbers and problems as the Mesh Optimization Audit window, inspect_models and
// the Inspector's warning rows show them (spec S53 R16, R18, R20, R22): the glTF header's counts,
// the cook's measured record for a platform, what uses it, and each issue with the fix it offers
// (the same arguments in every place: an MCP call, or an undoable command in the editor).

#pragma once

#include "model/ModelCookQueue.h"
#include "model/ModelHeader.h"
#include "model/ModelUsage.h"

#include "mcp/McpJson.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    using mcp::Json;

    struct ModelIssue {
        enum class Fix { None, Settings, Reimport, SceneLod, OpenImporter };
        std::string code;           // "mesh-no-lod"
        std::string severity;       // "error", "warning", "info"
        std::string message;
        Fix fix = Fix::None;
        std::string fixLabel;       // "Generate LODs"
        Json settings;              // Fix::Settings: snake_case settings for set_model_import_settings
        std::string scene;          // Fix::SceneLod: the project-relative scene
    };

    struct ModelAuditRow {
        std::string assetPath;      // asset-relative
        std::string projectPath;    // project-relative
        ModelFamily family = ModelFamily::Desktop;
        bool headerRead = false;
        std::string headerError;
        ModelHeader header;
        bool hasState = false;
        ModelCookState state;
        ResolvedModelSettings resolved;
        ModelImportSettings stored;
        bool storedRead = true;     // false: the section is malformed (its error in headerError)
        ModelUsage usage;
        ModelReadWriteDecision readWrite;
        // what the loader finds in the source (S53 R5): the cook's record, then the model's last load
        std::vector<GltfDiagnostic> diagnostics;
        std::vector<ModelIssue> issues;

        // the numbers the Audit's columns show: the cook's when cooked, else the header's
        long long triangles() const;
        long long vertices() const;
        uint64_t memoryBytes() const;
        uint64_t downloadBytes() const;
        int lodLevels() const;
        std::string status() const;     // "cooked", "cooking", "failed", "not cooked"
    };

    class ModelAudit {
    public:
        // Main thread: reads the model's header, its settings, its cook state and its uses
        // (refresh the usage index first)
        static ModelAuditRow row(Project& project, const std::string& assetPath, ModelFamily family);
        static std::vector<ModelIssue> issues(const ModelAuditRow& row);

        // inspect_models' record of one model (snake_case)
        static Json toJson(const ModelAuditRow& row, bool details);
        static Json recordJson(const ModelCookRecord& record);
        static Json resolvedJson(const ResolvedModelSettings& resolved);
        // the sidecar's camelCase keys <-> MCP's snake_case keys of R8's settings
        static std::string snakeOf(const std::string& metaKey);
        static std::string metaKeyOf(const std::string& snakeKey);
        static Json storedJson(const ModelImportSettings& settings);
        // get_asset_info's and get_model_info's cook block (R23): required extensions, the source's
        // and each platform's cooked sizes and status
        static Json cookSummaryJson(Project& project, const std::string& assetPath);
        static std::string formatBytes(uint64_t bytes);
    };

}
