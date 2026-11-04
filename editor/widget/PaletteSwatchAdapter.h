// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A Palette asset shown as a read-only swatch library (docs/specs/S43-palettes.md R34): the Color
// window lists the project's palettes under "Palettes" through it, so its swatch grid and a
// click's pick work on them unchanged. The swatches' linear values are the engine's convention
// (pixelart::toEngineLinear, gamma 2.2), so a picked palette colour shows as its exact byte. The
// adapter never writes: edits of a palette go through the palette's own undoable writes.

#include "pixelart/Palette.h"
#include "widget/SwatchLibrary.h"

#include <filesystem>

namespace jasperengine::editor {

    // `file`: the .palette it was read from (the library's path); the library is named after the
    // palette (its file stem when it has no name), Project scope, read-only, every colour a swatch
    // named as the palette names it
    SwatchLibrary paletteSwatchLibrary(const jasperengine::pixelart::Palette& palette, const std::filesystem::path& file);

}
