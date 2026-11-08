// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md §6.2-§6.5)
// - Purpose: what S83's effect families share on top of S80's builders (TitleFxParams.h): the
//   animation marks of §6.3 (L live, X relayout, the stepped live seed, S85's multiply for light
//   intensities), Vec2 params, the canvas centre and light directions in frame px, and the static
//   cost model of §6.4 (FRE, taps, the class a cost falls in).
// - Units: frame px are target px, y down (TitleFrame.h); canvas px are document px.
// - Thread-safety: pure functions.

#pragma once

#include "Export.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/render/TitleEffectContext.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::titleart::tfx {

    // ---- params (§6.3's marks) ----

    // L: changes per frame through the definition's live variant (D10)
    inline ParamDef live(ParamDef p) {
        p.anim = FxAnim::Live;
        return p;
    }
    // L on a seed or an enum: live, holding between keys (a strobe reseeded every N frames, R16)
    inline ParamDef liveStepped(ParamDef p) {
        p.anim = FxAnim::Live;
        p.interp = ParamInterp::Stepped;
        return p;
    }
    // X: the CPU or GPU layout re-runs
    inline ParamDef relayout(ParamDef p) {
        p.anim = FxAnim::Relayout;
        return p;
    }
    // S85: an intensity of light combines with its track by multiplying
    inline ParamDef multiplies(ParamDef p) {
        p.combine = ParamCombine::Multiply;
        return p;
    }

    // a Vec2 param ([x, y], canvas px from the layer box centre or the canvas centre, +y up)
    inline ParamDef vec2(const char* key, const char* label, double x, double y, const char* doc) {
        ParamDef p;
        p.key = key;
        p.label = label;
        p.kind = ParamKind::Vec2;
        p.unit = ParamUnit::Px;
        p.defaultValue = Json::array({x, y});
        p.description = doc;
        return p;
    }

    // ---- frame space ----

    // the canvas centre in target px
    inline TitleVec2 canvasCentre(const EffectContext& ctx) {
        return ctx.documentToTarget.apply(TitleVec2{0.0, 0.0});
    }
    // a point given in canvas px from the canvas centre (+y up) in target px
    inline TitleVec2 canvasPoint(const EffectContext& ctx, const TitleVec2& p) {
        return ctx.documentToTarget.apply(p);
    }
    // the direction toward a light at `degrees` counter-clockwise from +x (Photoshop's "comes
    // from"), as a frame vector (y down)
    inline TitleVec2 lightToward(double degrees) {
        const double r = degrees * 3.14159265358979323846 / 180.0;
        return {std::cos(r), -std::sin(r)};
    }
    // an angle clockwise on screen as Pachislop's y-up vector (dirVec)
    inline TitleVec2 dirUp(double degrees) {
        const double r = degrees * 3.14159265358979323846 / 180.0;
        return {std::cos(r), -std::sin(r)};
    }
    // the document seed added to an element seed (S80 Deviation 39: seed + (document seed - 1))
    inline float seedOf(const EffectContext& ctx, const ParamValues& p, const char* key = "seed") {
        return (float)(p.integer(key, 1) + (int)ctx.seed - 1);
    }

    // ---- the static cost model (§6.4) ----

    // accumulates passes: `fraction` of the region's pixels, each reading `taps` texels
    struct CostBuilder {
        FxCost cost;
        CostBuilder& pass(double fraction, int taps, int count = 1) {
            cost.passes += count;
            cost.fre += count * fraction * std::max(1.0, taps / 4.0);
            cost.maxTapsPerPixel = std::max(cost.maxTapsPerPixel, taps);
            return *this;
        }
        // the content's distance field: shared work, counted once per layer as its own row
        // (sdfCost), so only flagged here
        CostBuilder& field() {
            cost.sdf = true;
            return *this;
        }
        // the bounds reduction (a pair of texels per 4 x 4 block a level: the first writes 1/8 of
        // the region's pixels, 16 taps each)
        CostBuilder& box() {
            pass(1.0 / 8.0, 16).pass(1.0 / 128.0, 16).pass(1.0 / 2048.0, 16);
            return *this;
        }
        // fx::blur of radius px: halvings to sigma <= 6, two passes of up to 37 taps
        CostBuilder& blur(double radiusPx) {
            const double sigma = radiusPx / 3.0;
            if (sigma < 0.3) return pass(1.0, 1);
            int level = 0;
            while (sigma / std::ldexp(1.0, level) > 6.0 && level < 5) level++;
            double frac = 1.0;
            for (int l = 1; l <= level; l++) pass(frac /= 4.0, 1);
            const int taps = 2 * std::min(24, (int)std::ceil(3.0 * sigma / std::ldexp(1.0, level))) + 1;
            pass(frac, taps, 2);
            return pass(1.0, 1);
        }
    };

    // the shared distance field's own row: seed, the bounded flood, resolve over the region
    inline FxCost sdfCost(double reachPx, const FxSize& size) {
        CostBuilder b;
        const int side = std::max(1, std::max(size.width, size.height));
        int k = (int)std::ceil(std::log2((double)side));
        if (reachPx > 0.0) k = std::min(k, (int)std::ceil(std::log2(reachPx + 2.0)));
        b.pass(1.0, 9).pass(1.0, 9, std::max(1, k) + 1).pass(1.0, 2);
        b.cost.sdf = true;
        return b.cost;
    }

    // the class a definition's own cost falls in (§5 Performance's static limits); needing the
    // CPU analysis makes it C3 at least, the shared distance field is its own row
    inline CostClass costClassOf(const FxCost& c) {
        CostClass k = CostClass::C4;
        if (c.fre <= 1.0 && c.maxTapsPerPixel <= 16) k = CostClass::C0;
        else if (c.fre <= 3.0 && c.maxTapsPerPixel <= 64) k = CostClass::C1;
        else if (c.fre <= 10.0 && c.maxTapsPerPixel <= 200) k = CostClass::C2;
        else if (c.fre <= 40.0) k = CostClass::C3;
        if (c.analysis && k < CostClass::C3) k = CostClass::C3;
        return k;
    }

}
