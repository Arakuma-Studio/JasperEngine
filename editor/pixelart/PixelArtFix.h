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

    // the changes of one or more options as a plan (the preset, the window's Fix button)
    bool applyChanges(Project& project, const std::vector<FixChange>& changes, const std::string& stepName, FixApplyReport& report,
                      std::string& error);
    bool applyFixPlan(Project& project, const FixPlan& plan, const std::string& stepName, FixApplyReport& report, std::string& error);

}
