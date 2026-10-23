// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The named looks of a FluidSimulationComponent. Every preset sets the same fields (the solver
// and the look: resolutions, dissipations, pressure, curl, splats, pointer colours, shading,
// bloom, sunrays), so applying one after another gives the same result whatever came before and
// at most one preset matches. The compositing and the clock (output, transparent, backColor,
// interactive, aspectRatio, simulationRate, maxSubsteps, timeScale, paused, splatsOnStart,
// randomSplatCount, seed) are left alone: they depend on where the fluid is shown, not on how it
// looks.
//
// Pass counts (Pavel's pipeline): a step is 7 + pressureIterations fullscreen passes on the
// velocity grid plus one on the dye; a frame's look is 1 display pass, bloom 2 x levels + 1
// (levels: bloomIterations, at most until the chain reaches 2 texels) and sunrays 4.

#include "FluidPresets.h"

#include <functional>

using namespace jasperengine;

namespace {

    // one component field by its Catalog name: copy from a preset's values, compare
    struct FluidField{
        const char* name;
        void (*copy)(FluidSimulationComponent& to, const FluidSimulationComponent& from);
        bool (*equal)(const FluidSimulationComponent& a, const FluidSimulationComponent& b);
    };

#define FLUID_FIELD(member) \
    FluidField{ #member, \
        [](FluidSimulationComponent& to, const FluidSimulationComponent& from){ to.member = from.member; }, \
        [](const FluidSimulationComponent& a, const FluidSimulationComponent& b){ return a.member == b.member; } }

    // every serialized field a preset may list
    const std::vector<FluidField>& fluidFields(){
        static const std::vector<FluidField> fields = {
            FLUID_FIELD(enabled),
            FLUID_FIELD(output),
            FLUID_FIELD(interactive),
            FLUID_FIELD(aspectRatio),
            FLUID_FIELD(simResolution),
            FLUID_FIELD(dyeResolution),
            FLUID_FIELD(densityDissipation),
            FLUID_FIELD(velocityDissipation),
            FLUID_FIELD(pressure),
            FLUID_FIELD(pressureIterations),
            FLUID_FIELD(curl),
            FLUID_FIELD(gradientScale),
            FLUID_FIELD(simulationRate),
            FLUID_FIELD(maxSubsteps),
            FLUID_FIELD(timeScale),
            FLUID_FIELD(paused),
            FLUID_FIELD(splatRadius),
            FLUID_FIELD(splatForce),
            FLUID_FIELD(colorful),
            FLUID_FIELD(colorUpdateSpeed),
            FLUID_FIELD(splatsOnStart),
            FLUID_FIELD(randomSplatCount),
            FLUID_FIELD(seed),
            FLUID_FIELD(shading),
            FLUID_FIELD(backColor),
            FLUID_FIELD(transparent),
            FLUID_FIELD(bloom),
            FLUID_FIELD(bloomIterations),
            FLUID_FIELD(bloomResolution),
            FLUID_FIELD(bloomIntensity),
            FLUID_FIELD(bloomThreshold),
            FLUID_FIELD(bloomSoftKnee),
            FLUID_FIELD(sunrays),
            FLUID_FIELD(sunraysResolution),
            FLUID_FIELD(sunraysWeight),
        };
        return fields;
    }

#undef FLUID_FIELD

    const FluidField* findField(const std::string& name){
        for (const FluidField& field : fluidFields())
            if (name == field.name) return &field;
        return nullptr;
    }

    // the fields every preset sets
    const std::vector<std::string>& lookFields(){
        static const std::vector<std::string> fields = {
            "simResolution", "dyeResolution",
            "densityDissipation", "velocityDissipation",
            "pressure", "pressureIterations", "curl", "gradientScale",
            "splatRadius", "splatForce", "colorful", "colorUpdateSpeed",
            "shading",
            "bloom", "bloomIterations", "bloomResolution", "bloomIntensity", "bloomThreshold", "bloomSoftKnee",
            "sunrays", "sunraysResolution", "sunraysWeight",
        };
        return fields;
    }

    FluidPreset makePreset(const char* id, const char* name, const char* description, const std::function<void(FluidSimulationComponent&)>& set){
        FluidPreset preset;
        preset.id = id;
        preset.name = name;
        preset.description = description;
        preset.fields = lookFields();
        set(preset.values);    // from Pavel's defaults (the component's)
        return preset;
    }

    std::vector<FluidPreset> buildPresets(){
        std::vector<FluidPreset> presets;

        // Pavel's config as shipped (the component's defaults)
        presets.push_back(makePreset("default", "Default",
            "Pavel Dobryakov's original look: a 128 velocity grid with 20 pressure iterations (27 passes a step), 1024 dye, shading, 8-level bloom at 256 and sunrays at 196 (about 20 look passes a frame).",
            [](FluidSimulationComponent&){ }));

        // his fallback for devices without linear float filtering (isMobile() alone only halves the dye)
        presets.push_back(makePreset("mobile", "Mobile",
            "Pavel's fallback for weak GPUs: his solver on a 128 grid (27 passes a step) with a 512 dye (a quarter of the dye memory) and no shading, bloom or sunrays (1 display pass a frame).",
            [](FluidSimulationComponent& v){
                v.dyeResolution = 512;
                v.shading = false;
                v.bloom = false;
                v.sunrays = false;
            }));

        presets.push_back(makePreset("smoke", "Smoke",
            "Soft billowing smoke: wide slow-fading puffs that keep drifting, shaded and lit by sunrays without bloom; 128 grid with 20 pressure iterations (27 passes a step), 512 dye, about 5 look passes a frame.",
            [](FluidSimulationComponent& v){
                v.dyeResolution = 512;
                v.densityDissipation = 0.4f;
                v.velocityDissipation = 0.1f;
                v.curl = 20.0f;
                v.splatRadius = 0.5f;
                v.splatForce = 4000.0f;
                v.colorful = false;
                v.bloom = false;
                v.sunraysWeight = 0.6f;
            }));

        presets.push_back(makePreset("ink", "Ink",
            "Ink in still water: thin long-lasting strokes in one colour each that stop quickly and barely curl, shaded, no bloom or sunrays; 128 grid with 30 pressure iterations (37 passes a step), 1024 dye, 1 look pass a frame.",
            [](FluidSimulationComponent& v){
                v.densityDissipation = 0.15f;
                v.velocityDissipation = 0.8f;
                v.pressureIterations = 30;
                v.curl = 8.0f;
                v.splatRadius = 0.12f;
                v.splatForce = 3000.0f;
                v.colorful = false;
                v.bloom = false;
                v.sunrays = false;
            }));

        presets.push_back(makePreset("calm-water", "Calm Water",
            "Gentle water: broad weak splats with little vorticity that settle smoothly, slowly shifting colours, shading and a faint bloom; 128 grid with 30 pressure iterations (37 passes a step), 1024 dye, 6-level bloom at 256 (about 14 look passes a frame).",
            [](FluidSimulationComponent& v){
                v.densityDissipation = 0.5f;
                v.velocityDissipation = 0.5f;
                v.pressureIterations = 30;
                v.curl = 5.0f;
                v.splatRadius = 0.4f;
                v.splatForce = 2500.0f;
                v.colorUpdateSpeed = 2.0f;
                v.bloomIterations = 6;
                v.bloomIntensity = 0.3f;
                v.bloomThreshold = 0.7f;
                v.sunrays = false;
            }));

        presets.push_back(makePreset("psychedelic", "Psychedelic",
            "Swirling neon: strong vorticity, fast colour cycling, lingering trails and a heavy low-threshold bloom with sunrays; 256 grid with 20 pressure iterations (27 passes a step, 4x the cells of the default), 1024 dye, about 20 look passes a frame.",
            [](FluidSimulationComponent& v){
                v.simResolution = 256;
                v.densityDissipation = 0.5f;
                v.velocityDissipation = 0.1f;
                v.curl = 50.0f;
                v.splatRadius = 0.3f;
                v.splatForce = 8000.0f;
                v.colorUpdateSpeed = 30.0f;
                v.bloomIntensity = 1.4f;
                v.bloomThreshold = 0.3f;
                v.bloomSoftKnee = 0.9f;
            }));

        return presets;
    }

}

const std::vector<FluidPreset>& jasperengine::getFluidPresets() {
    static const std::vector<FluidPreset> presets = buildPresets();
    return presets;
}

const FluidPreset* jasperengine::findFluidPreset(const std::string& id) {
    for (const FluidPreset& preset : getFluidPresets())
        if (preset.id == id) return &preset;
    return nullptr;
}

bool jasperengine::applyFluidPreset(FluidSimulationComponent& component, const std::string& id) {
    const FluidPreset* preset = findFluidPreset(id);
    if (!preset) return false;
    for (const std::string& name : preset->fields)
        if (const FluidField* field = findField(name))
            field->copy(component, preset->values);
    return true;
}

std::string jasperengine::matchFluidPreset(const FluidSimulationComponent& component) {
    for (const FluidPreset& preset : getFluidPresets()) {
        bool match = true;
        for (const std::string& name : preset.fields) {
            const FluidField* field = findField(name);
            if (!field || !field->equal(component, preset.values)) {
                match = false;
                break;
            }
        }
        if (match) return preset.id;
    }
    return "";
}
