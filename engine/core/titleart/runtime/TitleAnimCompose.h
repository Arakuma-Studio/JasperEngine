// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R14, R15)
// - Purpose: a Realtime title's frame while layer-live or glyph tracks play: the layers that do not
//   move as cached plates, in draw order, between the layers that do. A layer split is rendered
//   once at its rest pose and drawn every frame with its sampled transform, opacity, tint and
//   flash; a text layer with glyph tracks draws per glyph cell (S81 R26, one atlas per title, the
//   back cells of a layer under its front cells), so any number of text layers can animate.
// - A pose change runs this compose only. Plates, splits and cells render again when the
//   document, a re-render track's value or the scale changes (their keys), time-sliced as any job.
// - Thread-safety: main thread.

#pragma once

#include "Export.h"
#include "render/TextureRender.h"
#include "titleart/anim/TitleAnimRealtime.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/runtime/TitleArtLive.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    class TitleArtRenderer;
    class AssetResolver;

    // one quad over `target`: `uv` of `texture`, its `width` x `height` pixels at (x0, y0) of the
    // target, moved by `move` (p' = (m0 x + m1 y + m2, m3 x + m4 y + m5)), linear tint and flash
    struct JASPERENGINE_API TitlePosedQuad {
        TextureRender* texture = nullptr;
        float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        double x0 = 0.0, y0 = 0.0;
        int width = 0, height = 0;
        double move[6] = {1, 0, 0, 0, 1, 0};
        float opacity = 1.0f;
        float tint[3] = {1.0f, 1.0f, 1.0f};
        float flash[3] = {0.0f, 0.0f, 0.0f};
    };
    // draws them in order (premultiplied, over what is there), several per pass while one texture
    // lasts; `passes` counts them
    JASPERENGINE_API bool drawTitlePosedQuads(TitleGpu& gpu, TitleTarget* target, const std::vector<TitlePosedQuad>& quads, int width, int height,
                                              uint32_t& passes, std::string& error);

    // a glyph's move in output pixels: about its pivot (from the cluster's ink box), to the pivot
    // shifted by the offset (% of the layer's size, layer axes)
    JASPERENGINE_API void titleGlyphMove(const GlyphPose& pose, const TitleRect& inkBox, const TitleVec2& pen, double layerSize,
                                         const std::function<TitleVec2(const TitleVec2&)>& toOutput, double move[6]);

    // the Realtime compose of `spec` (TitleJobSpec::compose set; spec.atlas caches its pieces)
    JASPERENGINE_API std::unique_ptr<TitleJob> makeTitleAnimComposeJob(TitleArtRenderer& renderer, const AssetResolver* assets, const TitleJobSpec& spec);

}
