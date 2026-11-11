// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Gaussian Blur (docs/specs/S80-title-art-core.md §6.5), ported from Pachislop
// src/effects/a/blur.ts (gaussianBlur) by SafEight, used with the author's permission.

#include "TitleFx.h"
#include "TitleFxParams.h"
#include "TitleFxFamilies.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxparams;

namespace {

    double largerScale(const ParamValues& p) {
        return std::max(p.number("scale_x"), p.number("scale_y")) / 100.0;
    }

    // Photoshop's radius is the standard deviation; After Effects' Repeat Edge Pixels clamps the
    // frame edge instead of fading it in from transparency
    void applyGaussianBlur(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float sigma = (float)std::max(0.0, p.number("radius")) * ctx.pxScale;
        const float sx = sigma * (float)std::max(0.0, p.number("scale_x")) / 100.0f;
        const float sy = sigma * (float)std::max(0.0, p.number("scale_y")) / 100.0f;
        fx::gauss(ctx, input, sx, sy, p.flag("repeat_edges"), output);
    }

    EffectDef gaussianBlur() {
        EffectDef d;
        d.type = "gaussian_blur";
        d.label = "Gaussian Blur";
        d.category = "Blur";
        d.description = "Photoshop's Gaussian Blur: a smooth, even blur of the layer (or of everything below an adjustment layer). Radius "
                        "is the standard deviation; the scales stretch it along one axis";
        d.params = {
            px("radius", "Radius", 8, 0, 500, "The blur's standard deviation", 0.1),
            number("scale_x", "Horizontal Scale", 100, 0, 200, ParamUnit::None, "The horizontal blur, percent of Radius", 1.0),
            number("scale_y", "Vertical Scale", 100, 0, 200, ParamUnit::None, "The vertical blur, percent of Radius", 1.0),
            flag("repeat_edges", "Repeat Edge Pixels", true, "Keep content at the canvas edge from fading to transparent"),
        };
        d.gizmos = {{GizmoKind::Radius, "radius", nullptr}};
        d.reach = [](const ParamValues& p) { return (float)(3.0 * p.number("radius") * largerScale(p)); };
        d.gather = [](const ParamValues& p) { return (float)(4.0 * p.number("radius") * largerScale(p)); };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F1;
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"gaussianBlur", "blur", "soften", "ぼかし (ガウス)"};
        d.apply = applyGaussianBlur;
        return d;
    }

}

void titleart::registerBlurEffects(TitleEffectRegistry& registry) {
    registry.add(gaussianBlur());
}
