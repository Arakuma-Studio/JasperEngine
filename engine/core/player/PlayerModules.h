// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The optional engine modules (docs/specs/S34-player-size.md R1): a player variant or a Compile
// build leaves out those its project cannot reach. The names are the ones of
// engine/player/variants.json, player-info.json features[] and content-manifest.json modules[].

#include "Export.h"

#include <string>
#include <vector>

namespace jasperengine::player {

    // every optional module, in a fixed order: htmlui video runtimeshaders physics3d physics2d navigation soundlayer noise
    JASPERENGINE_API const std::vector<std::string>& allModules();
    // those this build was compiled with (its JASPERENGINE_* definitions)
    JASPERENGINE_API std::vector<std::string> builtModules();
    JASPERENGINE_API bool isModuleBuilt(const std::string& module);
    // what a module is, for messages ("the HTML UI", "3D physics")
    JASPERENGINE_API std::string moduleDisplayName(const std::string& module);

}
