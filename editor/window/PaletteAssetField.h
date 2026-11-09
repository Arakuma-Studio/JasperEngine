// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Palette asset field of the inspectors (docs/specs/S43-palettes.md R7, R35, R36): the
// palette's name over a strip of its colours, a drop target for a project .palette, and a popup
// listing the project's palettes and the built-ins. Picking returns a name for
// pixelart::usePalette, which copies a built-in into the project on its first use.

#include "pixelart/Rgba8.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // True when the user picked something this frame: `picked` is an assets-relative .palette
    // path, "builtin:<id>", or "" for None (offered with allowNone). `current` is the stored
    // assets-relative path. `width` <= 0 fills the cell less -width.
    bool paletteAssetField(const char* id, Project& project, const std::string& current, std::string& picked, bool allowNone = true,
                           float width = -1.0f);

    // One row of swatches, `maxColors` at most (then "+N"), each with its hex in a tooltip
    void paletteSwatchStrip(const char* id, const std::vector<jasperengine::pixelart::Rgba8>& colors, float width, float height,
                            int maxColors = 64);

}
