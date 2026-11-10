// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleElementVariant.h"

using namespace jasperengine;
using namespace jasperengine::titleart;

uint32_t titleart::titleElementVariant(const TitleParams& params) {
    const auto& p = params.v;
    // element.glsl's conditions on the same members (uPatKind, uPatRelief, uTex, uTexAmt, uSheen,
    // uGlass, uRefl, uSatin, uSpan)
    const int pattern = (int)(p[30][0] + 0.5f);
    const int texture = (int)(p[20][3] + 0.5f);
    uint32_t bits = 0;
    if (pattern > 0 && pattern < (int)TITLE_ELEMENT_PATTERN_KINDS) {
        bits |= (uint32_t)pattern << TITLE_ELEMENT_PATTERN_SHIFT;
        if (p[32][2] > 0.0f) bits |= TITLE_ELEMENT_RELIEF;
    }
    // a texture kind is read by the texture (an amount) and by the sheen's grain and shapes
    const bool textureRead = p[21][0] > 0.0f || p[23][0] > 0.5f;
    if (texture > 0 && texture < (int)TITLE_ELEMENT_TEXTURE_KINDS && textureRead) bits |= (uint32_t)texture << TITLE_ELEMENT_TEXTURE_SHIFT;
    if (p[13][3] > 0.5f) bits |= TITLE_ELEMENT_GLASS;
    if (p[16][0] > 0.5f) bits |= TITLE_ELEMENT_REFLECTION;
    if (p[18][0] > 0.5f) bits |= TITLE_ELEMENT_SATIN;
    if ((int)(p[8][3] + 0.5f) > 0) bits |= TITLE_ELEMENT_SPAN;
    return bits;
}
