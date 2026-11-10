// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A whole-material change as property commands on the Catalog paths of the fields it changes
// (submeshes[N].material.<field>), so a model mesh keeps across reloads only those fields
// (Submesh::overrideFields) and a prefab instance overrides only those. Used by the material
// presets (inspector and MCP apply_material_preset).

#pragma once

#include "Catalog.h"
#include "texture/Material.h"

#include <functional>
#include <string>

namespace jasperengine::editor {

    class MultiPropertyCmd;
    class Project;
    struct MaterialPreset;

    // The material `preset` makes of `material` (its apply on a copy); the name, PBR values and
    // textures it does not set stay. meshShader: the shader the material draws with when it names
    // none (meshShaderFor; "" for a .material file, judged by its own shader)
    Material applyMaterialPreset(const MaterialPreset& preset, const Material& material, const std::string& meshShader = std::string());

    // The shader submeshes of `entity`'s mesh draw with when their material names none: the
    // mesh's customShader, else the scene's default mesh shader (RenderSystem::getSubmeshShader)
    std::string meshShaderFor(Scene* scene, Entity entity);

    // Adds to `multi` one command per field of submesh `submesh`'s material that differs between
    // `from` and `to`; false when nothing differs
    bool addMaterialChangeCommands(MultiPropertyCmd& multi, Project* project, uint32_t sceneId, Entity entity, unsigned int submesh,
                                   const Material& from, const Material& to, const std::function<void()>& onChanged = nullptr);

    // A Pixel FX preset's ramp (S48 R38) names a built-in palette's ramp, "builtin:<id>#ramp:<name>":
    // points each such shader texture at the project's copy of that palette ("<copy>#ramp:<name>",
    // S43 R4a), writing the copy as a project step when there is none (pixelart::usePalette; wroteCopy
    // tells, and pixelart::linkPaletteCopy makes it one undo step with the edit). False with error
    // when a copy cannot be resolved or written.
    bool resolvePresetPalettes(Project& project, Material& material, bool& wroteCopy, std::string& error);

}
