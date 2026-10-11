// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Material presets (shader library phase 2, section 2.5): one-click setups of a material's
// shader, extra pass and render state. Stub with the agreed interface and no presets; the
// shader library agent owns the real table.

#pragma once

#include "texture/Material.h"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct MaterialPreset {
        std::string id;          // stable, snake_case, e.g. "outline_hull"
        std::string name;        // display name
        std::string description; // one line
        std::function<void(Material&)> apply; // sets shader, extraPass, state, stencil...
    };

    const std::vector<MaterialPreset>& getMaterialPresets();
    const MaterialPreset* findMaterialPreset(const std::string& id);

}
