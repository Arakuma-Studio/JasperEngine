// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_PALETTERAMPTEXTURE_H
#define PIXELART_PALETTERAMPTEXTURE_H

// Ramp strips (docs/specs/S43-palettes.md R4a): "<palette>.palette#ramp:<name>" in any texture
// field is an N x 1 RGBA8 texture made from the ramp's colours (Nearest, clamp, no mips, never
// compressed), rebuilt when the palette reloads. A ramp that does not exist binds 1 x 1 white,
// with the warning palette_ramp_missing.

#include "Export.h"
#include "texture/TextureData.h"

#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::pixelart {

    // splits "<palette>.palette#ramp:<name>"; false for any other path
    JASPERENGINE_API bool parsePaletteRampPath(std::string_view path, std::string* palette = nullptr, std::string* ramp = nullptr);

    // the strip's pixels (palette by its asset path); *missing tells it is the white stand-in
    JASPERENGINE_API TextureData makePaletteRampTextureData(const std::string& palette, const std::string& ramp, bool* missing = nullptr);

    // Registers the strip as a generated texture of the pool under `id` (Texture::setPath)
    JASPERENGINE_API void registerPaletteRampTexture(const std::string& id, const std::string& path);
    // Hot reload: the strips of a palette made again from its new colours (main thread)
    JASPERENGINE_API void reloadPaletteRampTextures(const std::string& palette);
    JASPERENGINE_API std::vector<std::string> paletteRampTextureIds(const std::string& palette);

}

#endif // PIXELART_PALETTERAMPTEXTURE_H
