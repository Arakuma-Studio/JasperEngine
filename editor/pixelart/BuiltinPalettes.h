// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor's built-in palettes (docs/specs/S43-palettes.md R7, R9): pixelfx's 27 ramps, 10
// classic palettes and "pixelfx: Effects", embedded from editor/resources/palettes/<id>.palette.
// They are templates: using one copies it into the project (R7), so the runtime never sees them.
// Adding one is a file there plus a NOTICE.md line.

#include "pixelart/Palette.h"

#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::pixelart {

    struct BuiltinPalette {
        std::string id;                              // the file stem: "fire", "pico-8", "pixelfx-effects"
        jasperengine::pixelart::Palette palette;
    };

    constexpr const char* kBuiltinPrefix = "builtin:";

    // every built-in, parsed once, sorted by id
    const std::vector<BuiltinPalette>& builtinPalettes();
    // by "id" or "builtin:id"; nullptr when there is none
    const BuiltinPalette* findBuiltinPalette(std::string_view id);
    // the embedded .palette text of a built-in ("" when there is none): a copy into the project
    // is these bytes
    std::string_view builtinPaletteText(std::string_view id);

}
