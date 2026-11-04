// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R34, R35, R52)
// - Purpose: Project Settings > Title Art (project.yaml `titleArt:`, MCP keys title_art.*):
//   Auto Bake (a commit to a document whose outputs exist rebakes them 500 ms after the last
//   edit) and Stale Bakes On Export (rebake, warn or fail on outputs that are stale or missing).
// - Old projects load: a project.yaml without the node takes the defaults, and only values that
//   differ from them are written.
// - Changes are undoable (TitleArtSettingsCmd.h).
// - Thread-safety: main thread.

#pragma once

#include <string>
#include <vector>

namespace YAML {
    class Node;
}

namespace jasperengine::editor {

    enum class TitleStaleBakesPolicy : uint8_t { Rebake, Warn, Fail };
    const char* titleStaleBakesPolicyName(TitleStaleBakesPolicy policy);   // "rebake", "warn", "fail"
    const char* titleStaleBakesPolicyLabel(TitleStaleBakesPolicy policy);  // "Rebake", "Warn", "Fail"
    bool titleStaleBakesPolicyFromName(const std::string& name, TitleStaleBakesPolicy& out);

    struct TitleArtProjectSettings {
        bool autoBake = true;
        TitleStaleBakesPolicy staleBakesOnExport = TitleStaleBakesPolicy::Rebake;

        bool operator==(const TitleArtProjectSettings& other) const {
            return autoBake == other.autoBake && staleBakesOnExport == other.staleBakesOnExport;
        }
        bool operator!=(const TitleArtProjectSettings& other) const { return !(*this == other); }

        // the members that differ from the defaults (an empty map when none)
        YAML::Node encode() const;
        // a value that does not read keeps its default (`warn` is told which)
        static TitleArtProjectSettings decode(const YAML::Node& node, std::vector<std::string>* warnings = nullptr);
    };

}
