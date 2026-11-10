// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The texture reads of library:sprite's variants, counted statically in their Vulkan SPIR-V
// (docs/specs/S48-pixel-fx.md R33, AC9): the SpriteVariants.TapBudget test and the contact
// sheet's frame_cost lint count them the same way.

#include "shader/ShaderData.h"
#include "texture/Material.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    // texture reads in a SPIR-V module: OpImageSample* (87-94), OpImageFetch (95), OpImageGather
    // (96); -1 for no module
    int countSpirvTextureReads(const ShaderBinSource& bytecode);

    // The fragment stage's texture reads of the library:sprite variant with these features (the
    // renderer's sprite inputs, the default environment) minus the base sprite's: what the
    // features add per fragment. -1 when the build has no shader compiler or a variant fails.
    // Cached per feature set and blend mode.
    int librarySpriteExtraTaps(const std::vector<std::string>& features, MaterialBlendMode blend = MaterialBlendMode::AUTO);

}
