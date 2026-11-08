// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R37, D7)
// - Purpose: the PNG writer of Title Art's delivery exports: 8- or 16-bit grey, grey+alpha, RGB or
//   RGBA, tagged sRGB the way the PNG specification asks writers to (sRGB perceptual, gAMA 45455,
//   cHRM with the sRGB primaries and D65; never iCCP beside sRGB), pHYs from the document's dpi and
//   an optional iTXt recipe (`jasper.titleart.export`).
// - Encoder (the band design of Pachislop src/io/export.ts and pngWorker.ts, by SafEight, used with
//   the author's permission): every row Sub-filtered; 128-row bands deflated in parallel on the job
//   system with miniz tdefl at level 6, each band but the last ending with TDEFL_SYNC_FLUSH so the
//   raw streams concatenate into one zlib stream; the Adler-32s combined (adler32Combine, ported
//   from Pachislop src/io/pngChunk.ts). Bands always split at the same rows, so the bytes are the
//   same for any thread count.
// - Thread-safety: pure; encodePng uses the job system's workers.

#pragma once

#include "texsynth/SynthImage.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct TitlePngOptions {
        double dpi = 72.0;                  // pHYs; <= 0 writes none
        bool srgbChunks = true;             // sRGB + gAMA + cHRM
        std::string recipeKeyword;          // iTXt keyword ("" writes none)
        std::string recipeText;             // its UTF-8 text
        int bandRows = 128;
        int level = 6;
        bool parallel = true;               // false: the bands one after another (tests compare the bytes)
    };

    // PNG bytes of `image` (1-4 channels, 8 or 16 bits); false with `error`
    bool encodeTitlePng(const texsynth::Image& image, const TitlePngOptions& options, std::vector<uint8_t>& out, std::string& error);

    // zlib's adler32 and adler32_combine (Pachislop's pngChunk.ts)
    uint32_t titleAdler32(const uint8_t* bytes, size_t size, uint32_t adler = 1);
    uint32_t titleAdler32Combine(uint32_t adlerA, uint32_t adlerB, size_t lengthB);

}
