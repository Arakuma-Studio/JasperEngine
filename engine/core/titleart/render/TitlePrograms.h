// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.3 "Layout", R37)
// - Purpose: the numbers of the Title Art renderer's GPU programs, member for member
//   engine/shaderlib/titleart/render/programs.glsl (ShaderType::TITLE_ART's property bits 0-6 are
//   the number; titleart_render.frag compiles the one TA_PROGRAM names), and the parameter block
//   those programs read (u_fs_titleart: TITLE_PARAMS vec4s).
// - 1..17 are the effect and generator programs of titleart.frag (TitlePasses.h's TitleProgram,
//   16 vec4s); the renderer's run from TITLE_RENDER_PROGRAM_FIRST (18) to
//   TITLE_RENDER_PROGRAM_LAST. TitleGpu runs both kinds.
// - Thread-safety: constants.

#pragma once

#include "Export.h"

#include <cstdint>

namespace jasperengine::titleart {

    enum class TitleRenderProgram : uint32_t {
        Copy = 18,
        Blend = 19,
        AdjustMix = 20,
        MaskValue = 21,
        MaskGradient = 22,
        Blur = 23,
        BlurRaw = 24,
        Output = 25,
        Seed = 26,
        Flood = 27,
        Resolve = 28,
        Height = 29,
        CoverInit = 30,
        CoverFlood = 31,
        CoverRadius = 32,
        ReduceMax = 33,
        Span = 34,
        Element = 35,
        Place = 36,
        Erode = 37,
        MaskKeep = 38,
        Grow = 39,
        SeedAlpha = 40,
        ResolveAlpha = 41,
        GlyphCompose = 42           // S81's Glyph Atlas compose (TitleGlyphAtlas)
    };

    constexpr uint32_t TITLE_RENDER_PROGRAM_FIRST = 18;
    constexpr uint32_t TITLE_RENDER_PROGRAM_COUNT = 25;
    constexpr uint32_t TITLE_RENDER_PROGRAM_LAST = 63;

    // the program's name in titleart/render/programs.glsl without its TA_PROGRAM_ prefix ("COPY")
    JASPERENGINE_API const char* titleRenderProgramName(uint32_t program);

    // u_fs_titleart: p[0] the target size and its inverse (the GPU layer writes it), p[1] the
    // output's encoding, p[2..43] the program's own, p[44..47] the encodings of the raw slots
    constexpr int TITLE_PARAMS = 48;
    constexpr int TITLE_PARAM_OUT_ENCODING = 1;
    constexpr int TITLE_PARAM_RAW_ENCODING = 44;
    constexpr int TITLE_TEXTURE_SLOTS = 8;      // u_taTex0..7 (filterable)
    constexpr int TITLE_RAW_SLOTS = 5;          // u_taRaw0..4 (texelFetch, unfilterable)

    struct JASPERENGINE_API TitleParams {
        float v[TITLE_PARAMS][4] = {};

        void set(int index, float x, float y = 0.0f, float z = 0.0f, float w = 0.0f) {
            v[index][0] = x;
            v[index][1] = y;
            v[index][2] = z;
            v[index][3] = w;
        }
    };

}
