// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What create_particles (WorldTools.cpp) shares with the particles_* tools (ParticleTools.cpp):
// its presets as effects.

#include "mcp/McpJson.h"
#include "particles/ParticleEffect.h"

#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

    // The effect of a create_particles preset (fire, smoke, sparks, explosion, snow, rain,
    // magic, none) with that tool's overrides in `args` (rate, life, emitter, ...; scale for 2D)
    bool particlePresetEffect(const Json& args, const std::string& preset, bool is2D, particles::ParticleEffectDocument& out, std::string& error);
    const std::vector<std::string>& particlePresetNames();

}
