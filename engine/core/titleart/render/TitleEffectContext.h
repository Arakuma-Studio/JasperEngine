// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.3, §6.4, §6.5)
// - Purpose: what an effect's `apply` and a generator's `render` (TitleEffectRegistry.h) get
//   besides their targets: the runner of their passes, the target's place in the whole render,
//   px_scale, the document values they read, and for effects the layer's pixels before its
//   effects and its ink (§6.4 "Ink vs light"). The renderer fills one per effect of a stack and
//   per generator layer; tests and tools construct one around a TitlePassKit.
// - Targets: `input` and `output` of an effect have the same key (the node's region of
//   interest in output pixels, the accumulator format); `content`, `ink` and `inputInk` too.
//   An effect writes all of `output` and never reads it; scratch targets come from
//   runner.acquire() and go back with release().
// - Frame space: target pixels, memory rows (y down, TitleFrame.h); `frame` is where the whole
//   render (the canvas at px_scale) lies in it, so an effect can cut light at the canvas edge
//   and Repeat Edge Pixels clamps there.
// - Determinism: everything an effect draws is a function of its input, its params and these
//   values (`seed` is the document's: element seeds add to it, R17).
// - Canvas positions: a definition whose pixels depend on where the layer lies on the canvas,
//   not only on its input (it anchors to `documentToTarget` or `frame`: a canvas-aligned
//   pattern, a canvas-wide fill) sets `usesCanvasPosition`; the renderer then keys the node's
//   cached pixels by its position instead of moving them with the layer (R14).
// - Followed layers (generators, §6.4 "Layer sources"): a generator with FollowMode Optional or
//   Required gets the layer its `follow` member names, rendered alone, in `follow`.
//   S83: with FxInputs::content it also gets that layer before its own effects, and both come
//   with keys its CPU analysis caches readbacks by.
// - Thread-safety: main thread, for the duration of one apply or render call.

#pragma once

#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/render/TitlePasses.h"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    class AssetResolver;

    // a 2D affine map of points: (x', y') = (a x + b y + tx, c x + d y + ty)
    struct TitleAffine {
        double a = 1.0, b = 0.0, c = 0.0, d = 1.0, tx = 0.0, ty = 0.0;

        TitleVec2 apply(const TitleVec2& p) const { return {a * p.x + b * p.y + tx, c * p.x + d * p.y + ty}; }
        TitleAffine inverse() const {
            const double det = a * d - b * c;
            const double k = det != 0.0 ? 1.0 / det : 0.0;
            TitleAffine r;
            r.a = d * k;
            r.b = -b * k;
            r.c = -c * k;
            r.d = a * k;
            r.tx = -(r.a * tx + r.b * ty);
            r.ty = -(r.c * tx + r.d * ty);
            return r;
        }
        // this after `first`
        TitleAffine after(const TitleAffine& first) const {
            TitleAffine r;
            r.a = a * first.a + b * first.c;
            r.b = a * first.b + b * first.d;
            r.c = c * first.a + d * first.c;
            r.d = c * first.b + d * first.d;
            r.tx = a * first.tx + b * first.ty + tx;
            r.ty = c * first.tx + d * first.ty + ty;
            return r;
        }
    };

    struct EffectContext {
        TitlePassRunner& runner;
        int width = 0, height = 0;          // the targets' size, output px
        float pxScale = 1.0f;               // output px per canvas px: every Px param is multiplied by it
        // the whole render in target px (x, y of its top-left relative to the target, its size)
        TitleRectI frame;
        // document space (canvas px, origin at the centre, +y up) -> target px: what canvas-
        // anchored patterns and the global light's screen directions map through
        TitleAffine documentToTarget;
        // the layer's own origin (its transform's position) in target px: layer-anchored patterns
        TitleVec2 layerOrigin;
        uint32_t seed = 1;                  // the document's seed (an element's seed adds to it)
        double lightAngle = 120.0, lightAltitude = 30.0;    // global_light (ParamValues applies it)
        // the layer's (or group's, or part's) pixels before any of its effects; null = input.
        // Effects that take a shape from the silhouette read it, so an earlier glow does not move it
        const TargetRef* content = nullptr;
        // content without light of its own; null = content
        const TargetRef* ink = nullptr;
        // the input without the light earlier effects added (only for `outline` definitions after
        // an `adds_light` one, §6.4); null = input
        const TargetRef* inputInk = nullptr;
        std::string layerId;                // for pass labels and problems
        // an image asset's texture and size (a custom pattern); null (or no resolver) when it is
        // missing: the effect draws without it
        std::function<const TextureRender*(const TitleAssetRef& ref, int& width, int& height)> image;
        // set by apply: what it drew depends on the layer's place on the canvas (above)
        bool usesCanvasPosition = false;
        // what the alpha of the input `alphaKeyed` is a function of (0: not known): results of
        // that alpha alone (fx::blurAlpha, distanceField, layerBounds) are kept across renders
        // under it, so a colour change does not run them again (R42)
        uint64_t inputAlphaKey = 0;
        const TargetRef* alphaKeyed = nullptr;
        // the quality tier S81 renders for: Mobile takes the reductions S83 names (D11, D12)
        FxTier tier = FxTier::Desktop;
        // the input's colour holds light x 1 / inputLight (S83 D14: a group's light accumulator on
        // the RGBA8 path, given to a first effect that `readsLight`); its alpha is plain coverage
        float inputLight = 1.0f;

        EffectContext(TitlePassRunner& passes) : runner(passes) {}
    };

    struct GeneratorContext {
        TitlePassRunner& runner;
        int width = 0, height = 0;
        float pxScale = 1.0f;
        TitleRectI frame;
        int canvasWidth = 0, canvasHeight = 0;
        TitleAffine documentToTarget;
        // the generator layer's space (canvas px from its position, +y up, turned and scaled by
        // its transform) -> target px
        TitleAffine layerToTarget;
        uint32_t seed = 1;
        double lightAngle = 120.0, lightAltitude = 30.0;
        std::string layerId;
        // the layer `follow` names, rendered alone (its effects and masks, its ancestors'
        // transforms), premultiplied, with the target's key; null when the generator follows
        // nothing, the layer is missing, or following it would be a cycle (`follow_cycle`: the
        // generator then draws as it does following nothing)
        const TargetRef* follow = nullptr;
        // the followed layer's pixels before its own effects (its parts' effects kept), for a
        // definition whose FxInputs ask for `content` (S83 R6, R27: flame sources, emitter
        // masks); null as `follow`
        const TargetRef* followContent = nullptr;
        // change whenever `follow` / `followContent` pixels could (the followed layer's node
        // hash, px_scale and the target's place): S83's analysis caches readbacks by them; 0
        // when there is nothing to follow
        uint64_t followKey = 0, followContentKey = 0;
        // the render's fonts, images and SVGs (RenderRequest::assets); null when the request has
        // none. S83's Magic Circle draws its glyph rings with a font from it
        const AssetResolver* assets = nullptr;
        // the device tier it draws for (S83 D12, §6.7's Mobile reductions): Mobile runs each
        // definition's reduced path, the one its estimate for FxTier::Mobile describes. The
        // renderer takes it from RenderRequest::fxTier (S81 sets it per device); tests set it
        FxTier tier = FxTier::Desktop;
        // its output's colour holds light x lightScale, alpha plain (S83 D14: the light of an
        // `encodesLight` generator inside a group whose Light Glow reads it, on the RGBA8 path)
        float lightScale = 1.0f;
        // set by render: what it drew depends on the layer's place on the canvas (above). A
        // generator that fills the canvas from canvas coordinates sets it
        bool usesCanvasPosition = false;
        // set by render: the problems of what it drew (Magic Circle's glyph_missing), which the
        // renderer reports with the render
        std::vector<TitleProblem> problems;

        GeneratorContext(TitlePassRunner& passes) : runner(passes) {}
    };

}
