// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_NAMEDCOLORS_H
#define COMPAT_CSS_NAMEDCOLORS_H

#include "Export.h"
#include "htmlui/compat/CompatTypes.h"

#include <cstdint>
#include <string_view>

namespace jasperengine::htmlui::compat {

    // A color keyword and its sRGB value, 0xRRGGBBAA.
    struct NamedColor {
        const char* name;   // lowercase
        uint32_t rgba;
    };

    // The 148 named colors of CSS Color 4 (with transparent), sorted by name.
    JASPERENGINE_API Span<const NamedColor> namedColors();
    // ASCII case-insensitive; nullptr when not a named color ("transparent" is one)
    JASPERENGINE_API const NamedColor* findNamedColor(std::string_view name);

    // The CSS system colors (CSS Color 4 §6.2) and the deprecated CSS2 ones, with Chromium's
    // light-scheme values (phase-2 §3.9.6 step 4), sorted by lowercase name.
    JASPERENGINE_API Span<const NamedColor> systemColors();
    JASPERENGINE_API const NamedColor* findSystemColor(std::string_view name);

}

#endif //COMPAT_CSS_NAMEDCOLORS_H
