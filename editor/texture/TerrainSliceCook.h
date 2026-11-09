// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T3 R16, D8: with Compress Terrain Layers on, an export cooks each terrain array slice as an ordinary 2D
// KTX2 through S14's encoders. Slices are fixed so all of an array share a format: desktop BC7, mobile ASTC
// 6x6, Android ETC2_RGBA8, web UASTC. Keyed by the slice key in je-cooked/<family>/index.json, so shared maps
// dedupe, and the player concatenates each level (terraincore::loadCookedDetailArray).

#include "TextureCookQueue.h"

#include <map>
#include <set>
#include <string>

namespace jasperengine::editor {

    class Project;

    // Main thread. Adds the slices of the open scenes' terrains to inputs, written as lossless PNGs under the
    // project's cache, and names in droppedSources the layer sources nothing else reads, which no longer ship.
    // False with error when a scene the project has is not loaded and so cannot be read: its sources stay.
    bool gatherTerrainSliceInputs(Project& project, std::map<std::string, TextureCookInputs>& inputs, std::set<std::string>& droppedSources, std::string& error);

}
