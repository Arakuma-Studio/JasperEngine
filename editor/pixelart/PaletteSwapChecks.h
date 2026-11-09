// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What the material inspector, get_entity and check say about a Palette Swap material
// (docs/specs/S43-palettes.md R14, R15, R18, R35): the lossy-texture warning and its fix, the
// opaque colours that will not swap, the binding's build diagnostics, and Detect (the one project
// palette covering a texture). The image work (decode, counts) blocks: the inspector runs it on a
// worker, MCP in its tool.

#include "pixelart/PaletteAssets.h"
#include "pixelart/PaletteBinding.h"
#include "pixelart/RgbaImage.h"
#include "texture/Material.h"

#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart {

    // The shader a material draws with when it names none of its own is the caller's (the mesh's
    // or the scene's): `shader` overrides material.shader when not empty.
    // Whether one of the shader's @palette slots is on (Palette Swap or Cycling)
    bool paletteSwapEnabled(const Material& material, const std::string& shader = std::string());

    // R15: the sprite texture's import settings (every shown platform) and sampler
    struct PaletteSwapTextureCheck {
        std::string texture;            // assets-relative; empty: no image texture
        std::vector<std::string> reasons;   // "is compressed", "has mipmaps", "is resized (...)", "is filtered (...)"
        // a reason the fix cannot remove (the project's Texture Strategy resizes it)
        bool projectResize = false;
        int width = 0, height = 0;      // the image's, 0 when unread
        std::string subject = "Palette Swap";   // who needs it (message)

        bool lossy() const { return !reasons.empty(); }
        // "Palette Swap needs lossless textures: hero.png is compressed, has mipmaps"; "" when lossless
        std::string message() const;
    };
    // Reads the image's size from its header when width or height is 0
    PaletteSwapTextureCheck checkPaletteSwapTexture(Project& project, const Material& material, const std::string& shader = std::string(),
                                                    int width = 0, int height = 0);
    // The import settings alone (every shown platform) of any texture: S49's pixel particles
    PaletteSwapTextureCheck checkLosslessTexture(Project& project, const std::string& texture, int width = 0, int height = 0);
    // The warning palette_swap_lossy_texture (get_entity, check): its text, "" when Palette Swap
    // is off, there is no image texture, or the texture is lossless
    std::string paletteSwapLossyTexture(Project& project, const Material& material, const std::string& shader = std::string());

    // Fix: lossless (no compression, an explicit format only RGBA8), no mipmaps, a Max Size that
    // keeps the image's size, on the import settings and every platform override; Nearest on
    // the material's texture sampler
    struct PaletteSwapTextureFix {
        std::string texture;            // assets-relative
        bool importChanged = false;
        YAML::Node importSettings;      // the section to write at TextureImportSettings::metaKey
        bool samplerChanged = false;
        Texture sampler;                // the material's base texture with Nearest filters
    };
    bool planPaletteSwapTextureFix(Project& project, const Material& material, const PaletteSwapTextureCheck& check,
                                   PaletteSwapTextureFix& fix);

    // The distinct colours (by RGB, pixels of alpha 1 and up) of the image that are not in the
    // source palette: they draw unchanged
    int64_t paletteSwapOffPaletteColours(const RgbaImage& image, const jasperengine::pixelart::Palette& source);
    // The same for a material's base texture and its u_palette source; -1 when it has no source
    // or either cannot be read. Decodes the image: not for a frame of the UI
    int64_t paletteSwapOffPaletteColours(Project& project, const Material& material);

    // Detect (R18): the project palettes (assets-relative paths) that hold every colour of the
    // image's visible pixels; none for an image without one
    std::vector<std::string> palettesCoveringImage(const RgbaImage& image, const std::vector<ProjectPalette>& palettes);

    // What the renderer reports when it builds the binding's swap textures (R14): too many
    // colours, a cell of more than four, duplicates, a target that does not resolve. Builds them
    // on the CPU through PaletteLibrary: a few milliseconds, so callers cache it.
    std::vector<jasperengine::pixelart::Diagnostic> paletteSwapDiagnostics(const jasperengine::pixelart::PaletteBinding& binding);

}
