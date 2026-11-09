// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_PALETTEBINDING_H
#define PIXELART_PALETTEBINDING_H

// A material's palette binding (docs/specs/S43-palettes.md R13, R45): what a shader's
// `// @palette <uniform>` slot draws with. Row 0 is the source's own colours; rows 1...n are the
// source's variants (with useVariants), then the extra targets in order.

#include "asset/AssetRef.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::pixelart {

    struct Palette;

    // One extra row of a binding: a palette's colours by index, or one of its variants. The text
    // form, "<palette>" or "<palette>#<variant>", is what Lua, MCP and the inspector show;
    // "runtime:<n>" names a palette made at run time (Palette.new).
    struct PaletteTarget {
        AssetRef palette;
        std::string variant;
        // the runtime palette (Palette.new) a "runtime:<n>" row names, held while the binding lives:
        // a script that keeps no reference does not lose it before the first draw (never saved)
        std::shared_ptr<const Palette> runtime;

        std::string text() const { return variant.empty() ? palette.path : palette.path + "#" + variant; }
        static PaletteTarget parse(const std::string& text) {
            PaletteTarget target;
            const size_t hash = text.find('#');
            target.palette = hash == std::string::npos ? text : text.substr(0, hash);
            if (hash != std::string::npos) target.variant = text.substr(hash + 1);
            return target;
        }
        bool operator==(const PaletteTarget& other) const { return palette == other.palette && variant == other.variant; }
    };

    struct PaletteBinding {
        AssetRef source;                     // a palette asset (its path relative to the assets directory)
        std::vector<PaletteTarget> targets;  // at most 15 rows with the variants
        bool useVariants = true;

        bool empty() const { return source.empty() && targets.empty(); }
        bool operator==(const PaletteBinding& other) const {
            return source == other.source && targets == other.targets && useVariants == other.useVariants;
        }
    };

    // uniform -> binding, as Material::shaderPalettes holds them
    using ShaderPalettes = std::vector<std::pair<std::string, PaletteBinding>>;

}

#endif // PIXELART_PALETTEBINDING_H
