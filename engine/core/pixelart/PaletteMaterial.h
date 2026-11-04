// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_PALETTEMATERIAL_H
#define PIXELART_PALETTEMATERIAL_H

// Palette Swap on a material, the way Lua (sprite.palette, paletteRow, setPaletteTarget,
// paletteCycleSpeed), MCP (get_entity's row names) and the inspector read and change it
// (docs/specs/S43-palettes.md R13, R16, R17, R42). The row is the material's `paletteRow`
// parameter and the speed its `paletteCycleSpeed`: writing them rewrites a uniform only.

#include "Export.h"
#include "pixelart/Palette.h"
#include "texture/Material.h"

#include <string>
#include <vector>

namespace jasperengine::pixelart {

    constexpr const char* kPaletteUniform = "u_palette";
    constexpr const char* kPaletteRowParam = "paletteRow";
    constexpr const char* kPaletteCycleSpeedParam = "paletteCycleSpeed";

    // the binding of a uniform, or null
    JASPERENGINE_API const PaletteBinding* findPaletteBinding(const Material& material, const std::string& uniform = kPaletteUniform);
    JASPERENGINE_API PaletteBinding& editPaletteBinding(Material& material, const std::string& uniform = kPaletteUniform);

    // the names of the rows: "" for row 0 (the source's own colours), then the variants and
    // targets (PaletteSwapRow names); just {""} without a readable source
    JASPERENGINE_API std::vector<std::string> paletteRowNames(const Material& material, const std::string& uniform = kPaletteUniform);

    JASPERENGINE_API int getPaletteRow(const Material& material);
    JASPERENGINE_API void setPaletteRow(Material& material, int row);
    JASPERENGINE_API float getPaletteCycleSpeed(const Material& material);
    JASPERENGINE_API void setPaletteCycleSpeed(Material& material, float speed);

    // Row `row` (1-15) shows `palette` (an asset path, "<path>#<variant>" or a runtime key):
    // the binding's rows become explicit targets (the variants named as "<source>#<variant>"),
    // padded with the source, so every other row keeps what it showed
    JASPERENGINE_API bool setPaletteTarget(Material& material, int row, const std::string& palette, std::string* error = nullptr);

}

#endif // PIXELART_PALETTEMATERIAL_H
