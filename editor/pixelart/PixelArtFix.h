// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Applying a fix plan (docs/specs/S42-pixel-art-mode.md R2, R13): project settings through the
// settings registry (one ProjectSettingsValuesCmd), import settings through SetAssetMetaCmd (the
// cook queue recooks what they change), files through ProjectFileWrites, scene edits through
// the component and scene commands; every history the plan touches gets one step of that name,
// linked so one Ctrl+Z undoes them all. Main thread. A scene that is closed opens first.

#include "pixelart/PixelArtCheck.h"

#include "json.hpp"

#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart {

    struct FixApplyReport {
        nlohmann::json undo;                    // {id, name} of the step, null when nothing changed
        std::vector<std::string> changed;       // targets changed ("project", scene#entity, texture)
        std::vector<std::string> recooking;     // textures whose import settings changed
        std::vector<std::string> savedScenes;   // scenes that need a save to keep the edits
    };

    // "Fix Pixel Art (n)", n the fixes applied
    std::string fixStepName(const FixPlan& plan);

    // The changes whose `from` no longer holds: what was planned from an earlier check (the
    // window's last result, an MCP job's snapshot, the preset dialog's preview) against an edit
    // made since. The project is checked again on `scope` (read as Pixel Art Mode sees it, so
    // every planned change has its finding) and each change must still be a fix of it with the
    // same `from`; a project setting is compared with its current value. "<target> <field>" each.
    std::vector<std::string> staleChanges(Project& project, const Scope& scope, const std::vector<FixChange>& changes);

    // The changes of one or more options (the preset, the window's Fix button), identical ones
    // applied once. verify: the scope of the check that planned them; when given, nothing is
    // applied if a change is stale (error starts with "stale: "). Null for changes made from the
    // current state the moment before (the importer's Apply to N References).
    bool applyChanges(Project& project, const std::vector<FixChange>& changes, const std::string& stepName, FixApplyReport& report,
                      std::string& error, const Scope* verify);
    bool applyFixPlan(Project& project, const FixPlan& plan, const std::string& stepName, FixApplyReport& report, std::string& error,
                      const Scope* verify);

}
