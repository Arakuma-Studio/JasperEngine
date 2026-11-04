// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.3 "Layout", R37)
// - Purpose: the numbers of Title Art's GPU programs, member for member
//   engine/shaderlib/titleart/programs.glsl (ShaderType::TITLE_ART's property bits are the
//   number; titleart.frag compiles the one TA_PROGRAM names), and the parameter block every
//   program reads (u_fs_titleart: TITLE_PARAMS vec4s).
// - 1..TITLE_RENDER_PROGRAM_LAST are the renderer's; the numbers above are the registry's
//   effect and generator programs (S80's core effects, S83's catalogue), which their definitions
//   declare beside their pass code.
// - Thread-safety: constants.

#pragma once

#include "Export.h"

#include <cstdint>

namespace jasperengine::titleart {

    enum class TitleRenderProgram : uint32_t {
        Copy = 1,
        Blend = 2,
        Adjust = 3,
        AdjustMix = 4,
        MaskValue = 5,
        MaskGradient = 6,
        Blur = 7,
        BlurRaw = 8,
        Output = 9,
        Seed = 10,
        Flood = 11,
        Resolve = 12,
        Height = 13,
        CoverInit = 14,
        CoverFlood = 15,
        CoverRadius = 16,
        ReduceMax = 17,
        Span = 18,
        Element = 19,
        Place = 20,
        Erode = 21,
        MaskKeep = 22,
        Grow = 23,
        SeedAlpha = 24,
        ResolveAlpha = 25
    };

    constexpr uint32_t TITLE_RENDER_PROGRAM_FIRST = 1;
    constexpr uint32_t TITLE_RENDER_PROGRAM_COUNT = 25;
    constexpr uint32_t TITLE_RENDER_PROGRAM_LAST = 63;
    constexpr uint32_t TITLE_RENDER_PROGRAM_MAX = 255;

    // the program's name in titleart/programs.glsl without its TA_PROGRAM_ prefix ("COPY")
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
