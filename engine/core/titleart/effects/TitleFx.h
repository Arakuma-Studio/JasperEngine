// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.5, §6.7)
// - Purpose: the effect library every effect and generator definition builds its passes with
//   (Pachislop's src/effects/lib.ts and a/util.ts): the pass skeleton for a context, scratch
//   targets, the Gaussian blurs (lib.ts blur, a pyramid above sigma 6; blur.ts gauss per axis), the
//   jump-flood distance field bounded by the reach its consumer needs (R27), the layer's box as a
//   GPU reduction, gradient ramps as lookups, and the frame-space conversions (directions,
//   colours, the pattern map).
// - Results: helpers that make a target return a Scratch the caller holds while passes read it
//   (it goes back to the runner when it ends). A failed pass leaves the runner's error() set and
//   the helper returns an empty Scratch; callers stop drawing.
// - Units: radii, sizes and offsets here are output px (a Px param times px_scale).
// - Thread-safety: main thread, inside an apply or render call.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/render/TitleEffectContext.h"
#include "titleart/render/TitlePasses.h"

namespace jasperengine::titleart::fx {

    // a runner's target, returned to it when this ends
    class JASPERENGINE_API Scratch {
    public:
        Scratch() = default;
        Scratch(TitlePassRunner& runner, TargetRef* target) : owner(&runner), ref(target) {}
        // a result the runner keeps (TitlePassRunner::recall): read only, never released here
        static Scratch kept(const TargetRef* target);
        Scratch(Scratch&& other) noexcept;
        Scratch& operator=(Scratch&& other) noexcept;
        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;
        ~Scratch();

        TargetRef* get() const { return ref; }
        // lets go of the target without returning it (the runner took it over)
        void forget() { owner = nullptr; ref = nullptr; }
        TargetRef& operator*() const { return *ref; }
        explicit operator bool() const { return ref && *ref; }

    private:
        TitlePassRunner* owner = nullptr;
        TargetRef* ref = nullptr;
    };

    // a direction given in degrees clockwise on screen from +x, as a frame-pixel vector (y down)
    JASPERENGINE_API TitleVec2 dirClockwise(double degrees);
    // the linear value of an sRGB colour (Pachislop's THREE.Color uniforms)
    JASPERENGINE_API TitleColor linearOf(const TitleColor& srgb);

    // the gradient's 256-entry ramp: sRGB-encoded colour and straight alpha per stop, colour and
    // opacity blended apart with the stops' midpoints, `vivid` in OKLCH (Pachislop bakeGradient)
    JASPERENGINE_API TitleLutData gradientLut(const TitleGradient& gradient);
    // the gradient at pos 0..1: sRGB-encoded r, g, b and straight alpha
    JASPERENGINE_API void sampleGradient(const TitleGradient& gradient, double pos, float out[4]);

    // a pass of `program` into `output` with the context's frame and px_scale
    JASPERENGINE_API TitlePass makePass(const EffectContext& ctx, TitleProgram program, const char* label, TargetRef& output);
    JASPERENGINE_API TitlePass makePass(const GeneratorContext& ctx, TitleProgram program, const char* label, TargetRef& output);

    // a target like the effect's own (the accumulator format) of this size; the context's size
    // when width or height is 0
    JASPERENGINE_API Scratch scratch(EffectContext& ctx, int width = 0, int height = 0);
    // a target of a buffer kind of TitleArtCaps at this size
    JASPERENGINE_API Scratch scratchOf(TitlePassRunner& runner, TitleBuffer buffer, int width, int height);

    // dst = src x opacity (sampled by texture coordinate: copies between sizes resample bilinearly)
    JASPERENGINE_API bool copy(EffectContext& ctx, const TargetRef& src, TargetRef& dst, float opacity = 1.0f);

    // Gaussian blur of radius ~3 sigma = radiusPx (lib.ts blur): sigma above 6 runs on a
    // downsampled copy (at most 5 halvings); the result has the context's size, edges clamped
    JASPERENGINE_API Scratch blur(EffectContext& ctx, const TargetRef& src, float radiusPx);
    // blur() for a caller that reads only the result's alpha: of the context's keyed input
    // (EffectContext::inputAlphaKey) it is kept across renders, so a colour change reuses it
    JASPERENGINE_API Scratch blurAlpha(EffectContext& ctx, const TargetRef& src, float radiusPx);
    // Gaussian of standard deviations sigmaX, sigmaY (blur.ts gauss): each axis halved until its
    // sigma is at most 6 level px, written to `output` (the context's size). Without
    // repeatEdges taps outside the whole render read transparent (they fade at the frame edge)
    JASPERENGINE_API bool gauss(EffectContext& ctx, const TargetRef& src, float sigmaX, float sigmaY, bool repeatEdges, TargetRef& output);

    // the flood passes a distance field whose consumers read up to reachPx needs (R27, R43):
    // steps 2^(k-1) .. 1 and one more 1, k = ceil(log2(reach + 2)); the unbounded field (reach
    // <= 0) floods from the target's larger side
    JASPERENGINE_API int floodPasses(float reachPx, int width, int height);
    // the signed distance (+ outside) to src's alpha edge at `threshold`, exact out to reachPx
    // (beyond it "at least reachPx"), in the Distance buffer (R16F or RGBA8 fixed point). It reads
    // alpha only: of the keyed input it is kept across renders like blurAlpha()
    JASPERENGINE_API Scratch distanceField(EffectContext& ctx, const TargetRef& src, float threshold, float reachPx);
    // the box of src's pixels with alpha > 0.004, a 2 x 1 target fx_layerBox reads (kept like
    // distanceField())
    JASPERENGINE_API Scratch layerBounds(EffectContext& ctx, const TargetRef& src);

    // the Material pattern kinds (patterns.glsl): ids of the stored names, 0 for none
    JASPERENGINE_API int patternKindId(std::string_view kind);

}
