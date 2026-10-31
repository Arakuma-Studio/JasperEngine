// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UI_DRAW_COLOR_H
#define UI_DRAW_COLOR_H

#include "math/Vector4.h"
#include "texture/Material.h"

namespace jasperengine{

    // The colour a UI draw uploads (u_fs_uiParams.color): the element's own colour (linear,
    // straight alpha) faded by its Canvas Group alpha g in a way that keeps the blend mode's
    // meaning (ui canvas group spec 7.3):
    // - ALPHA, ADDITIVE, AUTO: a *= g (an additive draw scales its contribution by the alpha)
    // - PREMULTIPLIED: rgba *= g (the colour carries the alpha)
    // - MULTIPLY: rgb = mix(1, rgb, g) (fades towards the identity white; exact without a texture)
    // - NONE: unchanged (an opaque draw has no alpha to fade)
    inline Vector4 uiDrawColor(const Vector4& color, float groupAlpha, MaterialBlendMode blendMode){
        if (groupAlpha >= 1.0f){
            return color;
        }
        const float g = (groupAlpha > 0.0f) ? groupAlpha : 0.0f;
        switch (blendMode){
            case MaterialBlendMode::NONE:
                return color;
            case MaterialBlendMode::PREMULTIPLIED:
                return Vector4(color.x * g, color.y * g, color.z * g, color.w * g);
            case MaterialBlendMode::MULTIPLY:
                return Vector4(1.0f + (color.x - 1.0f) * g, 1.0f + (color.y - 1.0f) * g, 1.0f + (color.z - 1.0f) * g, color.w);
            default:
                return Vector4(color.x, color.y, color.z, color.w * g);
        }
    }

    // A UI element a Canvas Group fades out entirely is not drawn: a group alpha of 0 hides it
    // in every blend mode (also with a custom shader that ignores the uniform colour), and an
    // alpha-blended element faded to a draw alpha of 0 shows nothing. A stencil writer still
    // draws (a mask keeps marking the stencil for the elements it clips). An element outside
    // any group (groupAlpha 1) is never skipped: old scenes draw as before
    inline bool uiDrawSkipped(const Vector4& color, float groupAlpha, MaterialBlendMode blendMode, bool writesStencil){
        if (writesStencil || groupAlpha >= 1.0f){
            return false;
        }
        if (groupAlpha <= 0.0f){
            return true;
        }
        const bool alphaBlended = blendMode == MaterialBlendMode::AUTO || blendMode == MaterialBlendMode::ALPHA ||
                                  blendMode == MaterialBlendMode::ADDITIVE;
        return alphaBlended && color.w * groupAlpha <= 0.0f;
    }

}

#endif //UI_DRAW_COLOR_H
