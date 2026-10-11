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
    // textures it does not set stay
    Material applyMaterialPreset(const MaterialPreset& preset, const Material& material);

    // Adds to `multi` one command per field of submesh `submesh`'s material that differs between
    // `from` and `to`; false when nothing differs
    bool addMaterialChangeCommands(MultiPropertyCmd& multi, Project* project, uint32_t sceneId, Entity entity, unsigned int submesh,
                                   const Material& from, const Material& to, const std::function<void()>& onChanged = nullptr);

}
