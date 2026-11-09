// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The texture cook's Pixel Art Cleanup step (docs/specs/S46-pixel-art-cleanup.md R33, D8): a raster
// texture whose .meta has `import.pixelCleanup: {enabled, recipe}` cooks the fixed pixels instead of
// its source. This file is the hook's other half: the cooker (editor/texture/TextureCooker.cpp)
// only knows the TextureCookStep shape, so nothing that links the cooker alone depends on the
// cleanup (R34).
//
// The verdict of a run on a source that changed is what decides: `needs_person` on a changed
// source cooks the raw source, with the guard named (R33). On the source the recipe was saved from,
// a person already saw that verdict and decided.

#pragma once

#include "texture/TextureCooker.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <string>

namespace jasperengine::editor::pixelart::cleanup {

    // The recipe text of an `import.pixelCleanup` section, canonical, whatever its `enabled`; ""
    // when the section is not a map or has no recipe to read
    std::string importSectionRecipeText(const YAML::Node& section);
    // The recipe text the cook hashes and runs for an asset's .meta. "" when the step is off: no
    // key, `enabled: false`, or not a raster source (.aseprite never takes the step).
    std::string importStepRecipeText(const std::filesystem::path& absoluteAsset);

    // The cook step for a recipe text ("": no step)
    TextureCookStep importCookStep(const std::string& recipeText);

}
