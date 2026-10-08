// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LUNASVGRUNTIME_H
#define LUNASVGRUNTIME_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jasperengine {

    // LunaSVG, the engine's SVG rasteriser (RmlUi's SVG plugin and TextureData). LunaSVG is
    // built without system font discovery and without file access, so the engine registers its
    // fonts here: the built-in Roboto under the generic families and the names LunaSVG falls
    // back to ("sans-serif", "serif", "monospace", "system-ui", "Arial", "DejaVu Sans", ...).
    // Main thread only.
    class JASPERENGINE_API LunaSVGRuntime {
    public:
        // Registers the fonts on the first call (LunaSVG keeps them for the process);
        // later calls do nothing. Every user of LunaSVG calls it before parsing.
        static void ensureFonts();

        // The rasterisation shared by TextureData and the tests: parses the SVG, renders it at
        // its intrinsic size (CSS px, 96 dpi) x scale, rounded up, into tightly packed RGBA8.
        // premultiplied = false gives straight alpha (the engine's texture convention).
        // false on a parse error, an empty size or a size over maxDimension (logged).
        static bool rasterize(const char* data, size_t length, float scale, bool premultiplied,
                              std::vector<uint8_t>& pixels, int& width, int& height,
                              float maxDimension = 16384.0f);
    };

}

#endif //LUNASVGRUNTIME_H
