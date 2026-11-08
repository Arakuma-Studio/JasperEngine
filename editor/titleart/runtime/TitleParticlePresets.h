// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R39, R71)
// - Purpose: the four particle effects around a title (Sparkle, Embers, Confetti, Coin Shower),
//   S20 effects built from create_particles presets with overrides, emitting from a rectangle
//   fitted to the title's alpha bounds. One description serves the Inspector's Add Particles
//   menu and get_title's `particles` (ready-to-call create_particles arguments).
// - Sizes: UI titles in UI units (the companion's rect, y down), world titles in world units
//   (the Sprite's size); the UI ones run on unscaled time like every UI particle system.
// - Thread-safety: pure functions; addTitleParticles runs commands (main thread).

#pragma once

#include "ecs/Entity.h"
#include "mcp/McpJson.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct TitleParticlePreset {
        std::string label;      // "Sparkle"
        std::string base;       // the create_particles preset it starts from ("magic")
        mcp::Json args;         // create_particles arguments (without scene and parent)
    };

    // `width`/`height`: the companion's size; `alpha*`: the title's ink inside its frame, 0..1
    // of the frame (top-left origin); `ui`: UI space; `highlight`: the color scheme's highlight
    // (sRGB 0..1), the gold of the presets when none
    struct TitleParticleFrame {
        float width = 100.0f, height = 100.0f;
        float alphaX = 0.0f, alphaY = 0.0f, alphaWidth = 1.0f, alphaHeight = 1.0f;
        bool ui = true;
        float highlight[3] = {1.0f, 0.86f, 0.45f};
    };

    std::vector<TitleParticlePreset> titleParticlePresets(const TitleParticleFrame& frame);
    // the frame of a Title Art Renderer's GameObject (its companion and sidecar)
    TitleParticleFrame titleParticleFrame(Project* project, uint32_t sceneId, Entity entity);
    // Add Particles: a child Particle System of the title, one undo step "Add Title Particles (Sparkle)";
    // false with `error`
    bool addTitleParticles(Project* project, uint32_t sceneId, Entity title, const std::string& label, std::string* error = nullptr);

}
