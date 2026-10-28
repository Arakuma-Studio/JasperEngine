// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The tables of the structs several components share: a material, its extra pass and stencil
// state (editor/reflect/components/MaterialReflect.cpp)

#include "reflect/Components.h"

#include "texture/Material.h"

namespace jasperengine::editor::reflect {

    // a component's own pipeline stencil (UI, points, lines): edits rebuild its shader
    JE_REFLECT_STRUCT_TRAITS(MaterialStencil, kStencilStruct);
    // a material's (and its extra pass's): edits reload the mesh
    extern const StructDesc kMaterialStencilStruct;
    JE_REFLECT_STRUCT_TRAITS(MaterialPass, kMaterialPassStruct);
    JE_REFLECT_STRUCT_TRAITS(Material, kMaterialStruct);

}
