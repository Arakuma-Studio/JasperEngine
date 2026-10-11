// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MaterialPresets.h"

using namespace jasperengine;

const std::vector<editor::MaterialPreset>& editor::getMaterialPresets() {
    static const std::vector<MaterialPreset> presets;
    return presets;
}

const editor::MaterialPreset* editor::findMaterialPreset(const std::string& id) {
    for (const MaterialPreset& preset : getMaterialPresets()) {
        if (preset.id == id) return &preset;
    }
    return nullptr;
}
