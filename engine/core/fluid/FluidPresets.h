// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FLUID_PRESETS_H
#define FLUID_PRESETS_H

// Named looks of a FluidSimulationComponent, shared by the Inspector, Lua
// (FluidSimulation:applyPreset) and the MCP tools. A preset sets only the fields it lists in
// `fields` (Catalog property names: the camelCase field names); the rest keep their values. matchFluidPreset() names the
// preset the component's values equal (the Inspector's "Custom" otherwise).
// The built-in presets all list the same fields (the solver and the look); the compositing
// (output, transparent, backColor, interactive, aspectRatio) and the clock (simulationRate,
// maxSubsteps, timeScale, paused, splatsOnStart, randomSplatCount, seed) are never touched.
// Floats are compared exactly as stored, Vector3 componentwise.

#include "Export.h"
#include "component/FluidSimulationComponent.h"

#include <string>
#include <vector>

namespace jasperengine{

    struct JASPERENGINE_API FluidPreset{
        std::string id;             // "default", "mobile", "smoke", "ink", "calm-water", "psychedelic"
        std::string name;           // "Default"
        std::string description;    // one sentence, with its cost
        std::vector<std::string> fields;    // the Catalog property names it sets
        FluidSimulationComponent values;    // the values of `fields`
    };

    JASPERENGINE_API const std::vector<FluidPreset>& getFluidPresets();
    JASPERENGINE_API const FluidPreset* findFluidPreset(const std::string& id);
    // sets the preset's fields; false: no such preset
    JASPERENGINE_API bool applyFluidPreset(FluidSimulationComponent& component, const std::string& id);
    // the id of the first preset all of whose fields equal the component's, "" when none does
    JASPERENGINE_API std::string matchFluidPreset(const FluidSimulationComponent& component);

}

#endif // FLUID_PRESETS_H
