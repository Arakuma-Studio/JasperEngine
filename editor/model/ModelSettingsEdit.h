// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Writing models' import settings (spec S53 R14, R16, R20, R22): the Inspector's Apply, the Audit
// window's fixes and MCP's set_model_import_settings make the same edit. Every section is built
// and checked before anything is written; the writes are one step of the project history
// (SetAssetMetaCmd per model, no-ops dropped); a section left with nothing set is removed rather
// than written (no meta churn).

#pragma once

#include "model/ModelImportSettings.h"

#include "yaml-cpp/yaml.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct ModelSettingsEdit {
        std::vector<std::pair<std::string, YAML::Node>> set;    // meta key -> value (a null node removes it)
        std::vector<std::string> reset;                         // meta keys back to the default
        std::optional<ModelFamily> platform;                    // the platform's override block (Override turns on)
    };

    // `stored` with the edit; `out` is null when nothing is left set. False with the reason.
    bool editModelSection(const YAML::Node& stored, const ModelSettingsEdit& edit, YAML::Node& out, std::string& error);

    // One undo step "label" writing each asset's section (null removes it). `changed` lists the
    // asset paths whose sidecar changed. False with the reason, and nothing kept, when a write fails.
    bool writeModelSections(Project& project, const std::vector<std::string>& assetPaths, const std::vector<YAML::Node>& sections,
                            const std::string& label, std::vector<std::string>& changed, std::string& error);

}
