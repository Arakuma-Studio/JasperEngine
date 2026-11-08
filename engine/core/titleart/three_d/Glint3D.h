// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R32, §6.6)
// - Purpose: the glint of a 3D title layer, light added before the tone curve. Ported from
//   Pachislop src/engine/three/glint.ts by SafEight, used with the author's permission.
//   - Full (Bake, High): the whole chain on the layer's linear HDR main image: the bright pass
//     box-averaged onto a grid locked to canvas pixels (level B = round(log2(pxScale)), so a 4x
//     export computes what a 1x export does, at 4 x 4 render px per texel; supersampling only
//     widens the bright pass's box), box mips, the hot coverage that tames broad faces, a narrow
//     and a wide bloom, the point detector (the bright pass minus the larger of its 1D blurs),
//     the pinch that keeps real glints, the star budget, and Kawase streaks per ray direction
//     (a long thin ray on the stars' mip and a short hot one a mip coarser).
//   - Mobile (Medium): the same chain at half resolution, without the star budget, two rays.
//   - Sprites (Low): the bright pass on the stars' mip, its 1D blurs and the point detector (4
//     passes), then one instanced draw of a star per local maximum (no readback).
//   The output is the whole render's level-B lattice covering the region (a preview window
//   gets the texels the whole render has there) with the uv transform the tone map samples it
//   through; compressed light (S86's SRGB8A8 encoding) is decoded and the MSAA shoulder expanded
//   by the bright pass.
// - Thread-safety: main thread (GPU passes).

#pragma once

#include "Export.h"
#include "titleart/TitleArtTypes.h"
#include "titleart/render/TitleTargetPool.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleTierFeatures.h"

#include <memory>
#include <string>

namespace jasperengine {
    class TextureRender;
}

namespace jasperengine::titleart {

    struct Glint3DInput {
        TextureRender* source = nullptr;    // the main HDR image (premultiplied, k x the region)
        int sourceWidth = 0, sourceHeight = 0;
        bool compressed = false;            // the source holds compressed light
        bool shoulder = false;              // the source's MSAA samples were shouldered
        double exposure = 1;
        int k = 1;                          // source texels per output px per axis
        TitleRectI region;                  // output px of the whole render (rows down)
        double frameWidth = 0, frameHeight = 0;     // the whole render (canvas x pxScale)
        double pxScale = 1;                 // output px per canvas px
        double unit = 1;                    // output px per % of the layer's source size
        Glint3DParams params;
        GlintMode mode = GlintMode::Full;
        bool originTopLeft = true;          // the sprites' clip-space rows
    };

    struct Glint3DOutput {
        TargetRef light;                    // RGBA16F added light on the level-B lattice
        float uv[4] = {1, 1, 0, 0};         // region uv -> light uv: x scale, y scale, x offset, y offset
        uint32_t passes = 0;
        // a pass met its program's first draw after another program's this frame
        // (TitleFirstDraws): render() returned false without an error, nothing usable
        bool deferred = false;
    };

    class JASPERENGINE_API Glint3D {
    public:
        Glint3D();
        ~Glint3D();
        Glint3D(const Glint3D&) = delete;
        Glint3D& operator=(const Glint3D&) = delete;

        // false while a program of the mode compiles
        bool ready(GlintMode mode, std::string* error);
        // the glint of `in` into `out`; false with `error`
        bool render(const Glint3DInput& in, TitleTargetPool& pool, Glint3DOutput& out, std::string& error);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    // the point detector's feature size, % of the source size (Pachislop GLINT_DETAIL)
    constexpr double GLINT_DETAIL = 1.5;

}
