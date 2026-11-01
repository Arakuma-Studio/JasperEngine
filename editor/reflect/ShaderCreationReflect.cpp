// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Project Settings > Graphics > Shader Creation (docs/specs/S32-async-shaders.md R26, R32) on a
// reflection table: the project file's shaderCreation block (only the values away from their
// defaults, so older projects re-save unchanged), its schema in jasper://reflection, and the
// labels, ranges and help of the settings rows.

#include "reflect/Reflect.h"
#include "reflect/Field.h"
#include "reflect/TypeOps.h"

#include "Generator.h"

namespace jasperengine::editor::reflect {

namespace {

    constexpr FieldDesc kShaderCreationFields[] = {
        JE_FIELD(ShaderCreationProjectSettings, asyncCreation, .flags = OmitIfDefault, .label = "Asynchronous Shader Creation",
                 .tooltip = "Shaders and pipelines are created off the render thread; a variant that is not ready draws a stand-in or waits. Off: everything is created at once on the render thread."),
        JE_FIELD(ShaderCreationProjectSettings, mainThreadBudgetMs, .flags = OmitIfDefault | Clamped, .min = 0.5f, .max = 16.0f,
                 .step = 0.5f, .format = "%.1f ms", .label = "Main Thread Budget (ms)",
                 .tooltip = "The render thread's time per frame for finishing creations; one always finishes."),
        JE_FIELD(ShaderCreationProjectSettings, glWarmDraws, .flags = OmitIfDefault, .label = "GL Warm Draws",
                 .tooltip = "GL: a new shader and state is drawn once on the worker context first, so the driver's first-draw compile does not stall the frame. Desktop GL on a GPU (verified on NVIDIA); never on software GL; GLES not yet."),
        JE_FIELD(ShaderCreationProjectSettings, recordDuringPlay, .flags = OmitIfDefault, .label = "Record During Play",
                 .tooltip = "Play adds the shader variants and pipelines it creates to the warm-up list (ShaderWarmup.json)."),
        JE_FIELD(ShaderCreationProjectSettings, includeInBuilds, .flags = OmitIfDefault, .label = "Include in Builds",
                 .tooltip = "Exports build the list's variants and ship the list, which the game creates at start."),
        JE_FIELD(ShaderCreationProjectSettings, holdFirstScene, .flags = OmitIfDefault, .label = "Hold First Scene",
                 .tooltip = "The game's scenes count as loaded only once the warm-up list is ready (the loading screen covers it)."),
    };

}

JE_REFLECT_STRUCT_TRAITS(ShaderCreationProjectSettings, kShaderCreationStruct);
const StructDesc kShaderCreationStruct = JE_STRUCT_DESC(ShaderCreationProjectSettings, "ShaderCreationSettings", kShaderCreationFields);

const StructDesc& shaderCreationStruct() {
    return kShaderCreationStruct;
}

}
