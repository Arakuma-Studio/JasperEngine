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

    // S81 R58: a value per S56 platform group (desktop, mobile, web)
    struct TitleArtGroupValues {
        double desktop = 0.0, mobile = 0.0, web = 0.0;
        bool operator==(const TitleArtGroupValues& o) const { return desktop == o.desktop && mobile == o.mobile && web == o.web; }
        bool operator!=(const TitleArtGroupValues& o) const { return !(*this == o); }
    };

    struct TitleArtProjectSettings {
        bool autoBake = true;
        TitleStaleBakesPolicy staleBakesOnExport = TitleStaleBakesPolicy::Rebake;
        // S81 R15, R20, R11, R55 (Realtime and Glyph Atlas titles): the frame budget (ms of
        // estimated GPU + main-thread CPU work; the editor's Edit mode apart), the Release Delay
        // of idle targets (s), the Cache Budget (MiB), the reference display height of the
        // prebaked states, and whether Play simulates the build's Characters
        TitleArtGroupValues frameBudgetMs{4.0, 2.0, 2.0};
        double editorFrameBudgetMs = 8.0;
        TitleArtGroupValues releaseDelaySeconds{10.0, 3.0, 3.0};
        TitleArtGroupValues cacheBudgetMiB{64.0, 16.0, 16.0};
        int referenceHeight = 1080;
        bool simulateExportCharacters = true;

        bool operator==(const TitleArtProjectSettings& other) const {
            return autoBake == other.autoBake && staleBakesOnExport == other.staleBakesOnExport && frameBudgetMs == other.frameBudgetMs &&
                   editorFrameBudgetMs == other.editorFrameBudgetMs && releaseDelaySeconds == other.releaseDelaySeconds &&
                   cacheBudgetMiB == other.cacheBudgetMiB && referenceHeight == other.referenceHeight &&
                   simulateExportCharacters == other.simulateExportCharacters;
        }
        bool operator!=(const TitleArtProjectSettings& other) const { return !(*this == other); }

        // the members that differ from the defaults (an empty map when none)
        YAML::Node encode() const;
        // a value that does not read keeps its default (`warn` is told which)
        static TitleArtProjectSettings decode(const YAML::Node& node, std::vector<std::string>* warnings = nullptr);
    };

}
