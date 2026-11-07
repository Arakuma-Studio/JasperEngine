// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Fractal Noise (docs/specs/S83-title-effects-generators.md §6.7.16): a new generator in the Fill &
// Pattern family, After Effects' Fractal Noise for titles: value-noise octaves (Pachislop's
// hash and noise, tfx_noise.glsl), turbulent and ridged sums or Worley cells, coloured through a
// gradient, with a looping `evolution` and a scrolling `offset` for S85 to key.

#include "titleart/generators/TitleGen.h"
#include "titleart/generators/TitleGenFamilies.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxparams;
using namespace jasperengine::titleart::gen;

namespace {

    constexpr double DEG = 3.14159265358979323846 / 180.0;
    // the mobile tier draws at most this many octaves (§6.7.16)
    constexpr int MOBILE_OCTAVES = 4;

    int octavesOf(const ParamValues& p, FxTier tier) {
        const int complexity = std::clamp(p.integer("complexity"), 1, 8);
        return tier == FxTier::Mobile ? std::min(complexity, MOBILE_OCTAVES) : complexity;
    }

    void renderFractalNoise(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        const TitleLut* ramp = ctx.runner.lut(fx::gradientLut(p.gradient("gradient")));
        if (!ramp) return;
        static const std::initializer_list<const char*> kinds = {"fbm", "turbulent", "ridged", "cells"};
        const TitleVec2 offset = p.vec2("offset");
        const double rotation = p.number("rotation") * DEG;
        TitlePass pass = genPass(ctx, GenProgram::FractalNoise, "fractal_noise", target, ctx.layerToTarget);
        pass.textures[0].lut = ramp;
        pass.set(6, (float)offset.x, (float)offset.y, (float)std::max(1.0, p.number("scale")), (float)std::max(0.1, p.number("aspect")));
        // the pattern turns clockwise on screen (negative in the y-up layer space) when the
        // sampled point turns the other way
        pass.set(7, (float)std::cos(rotation), (float)std::sin(rotation), (float)octavesOf(p, FxTier::Desktop), (float)optionIndex(p, "noise", kinds));
        pass.set(8, (float)p.number("contrast"), (float)p.number("brightness"), (float)p.number("evolution"), (float)std::fmod(seedOf(ctx, p), 4096.0));
        ctx.runner.run(pass);
    }

    FxCost fractalNoiseCost(const ParamValues& p, const FxSize&, FxTier tier) {
        // one full-canvas pass: the ramp fetch plus each octave's noise as two taps (four hashes;
        // a cell octave nine distance tests, three taps)
        const int octaves = octavesOf(p, tier);
        const int perOctave = p.string("noise") == "cells" ? 3 : 2;
        const int taps = 1 + perOctave * octaves;
        return passCost(1, std::max(1.0, taps / 4.0), taps);
    }

    GeneratorDef fractalNoise() {
        GeneratorDef d;
        d.type = "fractal_noise";
        d.label = "Fractal Noise";
        d.category = "Fill & Pattern";
        d.description = "Clouds, fog, plasma, caustics, static or a nebula: octaves of value noise (or cells) through a gradient, with a "
                        "looping Evolution and a scrolling Offset to animate";
        d.params = {
            choice("noise", "Noise", "fbm", {{"fbm", "Basic"}, {"turbulent", "Turbulent"}, {"ridged", "Ridged"}, {"cells", "Cells"}},
                   "Soft billows, turbulent creases, sharp ridges or Worley cell edges"),
            px("scale", "Scale", 240, 4, 4000, "The size of the coarsest octave's features"),
            number("aspect", "Aspect", 1, 0.1, 10, ParamUnit::Factor, "Stretches the noise horizontally"),
            integer("complexity", "Complexity", 5, 1, 8, "How many octaves add finer detail (the mobile tier draws at most 4)"),
            number("contrast", "Contrast", 1, 0, 4, ParamUnit::Factor, "Spreads the values about the middle"),
            number("brightness", "Brightness", 0, -1, 1, ParamUnit::None, "Shifts every value"),
            live(number("evolution", "Evolution", 0, 0, 100, ParamUnit::None, "Moves the noise through its next shapes (keyed, it loops smoothly)")),
            live(point("offset", "Offset", 0, 0, "Scrolls the noise, canvas px")),
            degrees("rotation", "Rotation", 0, "Turns the noise, clockwise", -180, 180),
            light(gradient("gradient", "Gradient", gradientValue({{0.0, "#000000"}, {1.0, "#ffffff"}}), "Colours the values from low to high")),
            seedParam("Reshuffles the noise"),
        };
        d.gizmos = {{GizmoKind::Point, "offset", nullptr}, {GizmoKind::Direction, "rotation", nullptr}};
        d.follow = FollowMode::None;
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F0;
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"evolution", "offset"};
        d.live.program = (uint32_t)GenProgram::FractalNoise | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"fractal noise", "noise", "clouds", "fog", "plasma", "turbulence", "フラクタルノイズ", "雲"};
        d.estimate = fractalNoiseCost;
        d.presetFile = "editor/resources/titleart/fx-presets/fractal_noise.yaml";
        d.render = renderFractalNoise;
        return d;
    }

}

void titleart::registerNoiseGenerators(TitleEffectRegistry& registry) {
    registry.add(fractalNoise());
}
