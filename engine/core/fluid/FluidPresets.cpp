// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract stub: the fluid-obj branch fills in the presets.

#include "FluidPresets.h"

using namespace jasperengine;

const std::vector<FluidPreset>& jasperengine::getFluidPresets() {
    static const std::vector<FluidPreset> presets;
    return presets;
}

const FluidPreset* jasperengine::findFluidPreset(const std::string& id) {
    for (const FluidPreset& preset : getFluidPresets())
        if (preset.id == id) return &preset;
    return nullptr;
}

bool jasperengine::applyFluidPreset(FluidSimulationComponent& component, const std::string& id) {
    (void)component;
    return findFluidPreset(id) != nullptr;
}

std::string jasperengine::matchFluidPreset(const FluidSimulationComponent& component) {
    (void)component;
    return "";
}
