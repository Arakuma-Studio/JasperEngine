// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.7 pass 10, Deviation 58)
// - Purpose: the variants of the ELEMENT program (ShaderType::TITLE_ART program 35). One program
//   holding every pattern kind, texture kind, glass, reflection, satin and span seam compiled for
//   4.8-5.3 s in llvmpipe's first draw; each element instead draws with the variant of what its
//   own material shades: the pattern kind (and whether its relief is read), the texture kind, and
//   glass, reflection, satin and spans on or off, each a property bit above the program number
//   (titleart/render/element.glsl makes them constants). The bits come from the element's block
//   values, the ones the shader would branch on, so every variant draws the pixels the whole
//   program draws.
// - Count: 1 + 27 x 2 pattern states (none, or a kind with or without relief) x 13 texture states
//   x 2^4 = 11,440 at most; an element uses one, a document one per distinct material (2-6 for a
//   title with rims), and S32's shader lists hold the ones a project's titles plan.
// - Thread-safety: pure functions.

#pragma once

#include "Export.h"
#include "titleart/render/TitlePrograms.h"

#include <cstdint>

namespace jasperengine::titleart {

    // the property bits (ShaderVariantCompiler: TA_EL_* defines; ShaderPool names Ek0.., Erl, ...)
    constexpr uint32_t TITLE_ELEMENT_PATTERN_SHIFT = 16;     // 5 bits: the pattern kind, 0 = none
    constexpr uint32_t TITLE_ELEMENT_PATTERN_MASK = 0x1Fu << TITLE_ELEMENT_PATTERN_SHIFT;
    constexpr uint32_t TITLE_ELEMENT_RELIEF = 1u << 21;      // the pattern's relief (4 more samples)
    constexpr uint32_t TITLE_ELEMENT_TEXTURE_SHIFT = 22;     // 4 bits: the texture kind, 0 = none
    constexpr uint32_t TITLE_ELEMENT_TEXTURE_MASK = 0xFu << TITLE_ELEMENT_TEXTURE_SHIFT;
    constexpr uint32_t TITLE_ELEMENT_GLASS = 1u << 26;
    constexpr uint32_t TITLE_ELEMENT_REFLECTION = 1u << 27;
    constexpr uint32_t TITLE_ELEMENT_SATIN = 1u << 28;
    constexpr uint32_t TITLE_ELEMENT_SPAN = 1u << 29;        // line / char spans and their seam
    constexpr uint32_t TITLE_ELEMENT_WHOLE = 1u << 30;       // every feature from the block (tests)
    constexpr uint32_t TITLE_ELEMENT_VARIANT_BITS = 0x7FFF0000u;
    constexpr uint32_t TITLE_ELEMENT_PATTERN_KINDS = 28;     // patterns.glsl: none, 1-27
    constexpr uint32_t TITLE_ELEMENT_TEXTURE_KINDS = 13;     // element.glsl: none, 1-12

    // the request's test flag: every element draws with TITLE_ELEMENT_WHOLE (the red control of
    // the compile cost and the variants' parity)
    constexpr uint32_t TITLE_ART_TEST_ELEMENT_WHOLE = 1u << 24;

    // the variant bits of an element pass whose block is `params` (Title2DPass.cpp's element
    // parameters; a knockout's block shades nothing: 0)
    JASPERENGINE_API uint32_t titleElementVariant(const TitleParams& params);

}
