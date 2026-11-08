// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// capture_particles' checks (docs/specs/S49-pixel-particles.md R38): the family's lint results
// (lint/LintTypes.h) over a capture's frames. S49 computes off_grid and over_budget; spill,
// one_shot_end, loop_seam, determinism, off_palette and partial_alpha are S48's EffectLints
// (S48 R28, D13) and report SKIP until it lands.

#include "pixelart/lint/LintTypes.h"
#include "pixelart/particles/ParticleCapture.h"

#include <cstdint>

namespace jasperengine::editor::pixelart {

    // the ids, in the order the report lists them
    const std::vector<std::string>& particleCheckIds();

    // `budget`: the world's particle budget (an effect's share is a sixteenth of it)
    lint::LintReport particleCaptureChecks(const ParticleCapture& capture, uint32_t budget);

}
