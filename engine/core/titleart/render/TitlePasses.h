// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.3, §6.5)
// - Purpose: one fullscreen pass of ShaderType::TITLE_ART as data (TitlePass), and the runner
//   that executes passes (TitlePassRunner): the seam between the pass code of the effects and
//   generators (effects/, generators/) and whoever runs it. The renderer's job runs them as its
//   GPU steps; tests and tools run them at once through TitlePassKit (render/TitlePassKit.h).
// - Programs: TitleProgram is the TA_PROGRAM number of titleart.frag in property bits 0-6
//   (titleProgramKey); bits 7 and 8 select the RGBA8 encodings of seeds and distances
//   (TitleArtCaps' Fixed12_4 and Fixed16). The numbers are append-only (cached shader keys).
// - The block u_fs_titleart is TITLE_PASS_VEC4S vec4s: p[0] the target size and px_scale, p[1]
//   the whole render in the targets' texture coordinates (both written by the runner from the
//   pass), p[2..] the program's values (titleart/effects.glsl documents each layout).
// - Textures: up to TITLE_PASS_TEXTURES filtered sources (u_taTex0..3: colour, coverage,
//   lookups) and TITLE_PASS_RAW_TEXTURES sources read with texelFetch (u_taRaw0..1: seeds,
//   distances, the bounds reduction), each a pooled target or an uploaded lookup.
// - Thread-safety: the structs are values; a runner is used on the main thread.

#pragma once

#include "Export.h"
#include "render/Render.h"
#include "shader/ShaderBuildTypes.h"
#include "titleart/TitleArtTypes.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleTargetPool.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {
    class TextureRender;
}

namespace jasperengine::titleart {

    // titleart.frag's TA_PROGRAM_* values (append-only)
    enum class TitleProgram : uint8_t {
        Copy = 1,
        Gauss = 2,
        Seed = 3,
        Jfa = 4,
        Distance = 5,
        Bounds = 6,
        OuterGlow = 7,
        InnerGlow = 8,
        DropShadow = 9,
        InnerShadow = 10,
        Outline = 11,
        ColorOverlay = 12,
        GradientOverlay = 13,
        PatternOverlay = 14,
        Adjust = 15,
        Fill = 16,
        PatternFill = 17
    };
    constexpr int TITLE_PROGRAM_LAST = 17;

    // ShaderType::TITLE_ART property bits after the program number
    constexpr uint64_t TITLE_VARIANT_SEEDS_FIXED = 1ull << 7;      // 'Sfx': seeds and bounds in RGBA8
    constexpr uint64_t TITLE_VARIANT_DISTANCE_FIXED = 1ull << 8;   // 'Dfx': distances in RGBA8
    // 'Gen': the program number names one of S83's generator programs (titleart_gen.vert/.frag,
    // generators/TitleGen.h's GenProgram), a number space of its own beside titleart.frag's
    constexpr uint64_t TITLE_VARIANT_GENERATOR = 1ull << 9;

    JASPERENGINE_API ShaderKey titleProgramKey(TitleProgram program, uint64_t variant = 0);
    JASPERENGINE_API const char* titleProgramName(TitleProgram program);    // "outer_glow", "gauss" ...
    // the variant bits a program needs on a device with these caps (fixed seeds or distances)
    JASPERENGINE_API uint64_t titleProgramVariant(TitleProgram program, const TitleArtCaps& caps);

    constexpr int TITLE_PASS_VEC4S = 16;
    constexpr int TITLE_PASS_TEXTURES = 4;
    constexpr int TITLE_PASS_RAW_TEXTURES = 2;
    constexpr int TITLE_LUT_SIZE = 256;

    // A 256 x 1 lookup uploaded once per content (gradient ramps, tone curves): RGBA8 bytes or
    // RGBA16F halves (TitleArtCaps' GradientLut and ToneLut). S83's data textures use the same
    // upload at another size: QuadBatch's instance records and the generators' tables (RGBA32F,
    // nearest, read with texelFetch)
    struct TitleLutData {
        ColorFormat format = ColorFormat::RGBA;
        std::vector<uint8_t> bytes;     // width x height texels in `format`, rows first to last
        int width = TITLE_LUT_SIZE, height = 1;
        TextureFilter filter = TextureFilter::LINEAR;
        uint64_t hash() const;
    };

    // an uploaded lookup, owned by the runner (valid until its clear())
    class JASPERENGINE_API TitleLut {
    public:
        virtual ~TitleLut() = default;
    };

    // what a pass samples: a pooled target, a lookup, or a texture of the asset system (an
    // image an effect reads, EffectContext::image)
    struct TitleSource {
        const TargetRef* target = nullptr;
        const TitleLut* lut = nullptr;
        const TextureRender* texture = nullptr;
        bool empty() const { return !target && !lut && !texture; }
    };

    struct TitlePass {
        TitleProgram program = TitleProgram::Copy;
        uint64_t variant = 0;               // TITLE_VARIANT_* bits
        const char* label = "";             // "outer_glow blur x": traces and pass timing
        float params[TITLE_PASS_VEC4S][4] = {};     // p[2..] are the program's; p[0], p[1] the runner's
        TitleSource textures[TITLE_PASS_TEXTURES];
        TitleSource raw[TITLE_PASS_RAW_TEXTURES];
        TargetRef* output = nullptr;
        // the whole render in texture coordinates of the effect's targets (x0, y0, x1, y1; the
        // same for every pyramid level a pass reads or writes): the runner writes it to p[1]
        float frameUv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        TitleRectI scissor;                 // output pixels; empty = all of it
        float pxScale = 1.0f;
        BlendMode blend = BlendMode::NONE;
        // S83's instanced generator passes (TITLE_VARIANT_GENERATOR programs that pull their
        // vertices): the vertex count drawn (3 = the fullscreen triangle) and the data texture
        // the vertex stage reads with texelFetch (gpu/QuadBatch.h)
        int vertices = 3;
        TitleSource vertexData;

        void set(int index, float x, float y = 0.0f, float z = 0.0f, float w = 0.0f) {
            params[index][0] = x;
            params[index][1] = y;
            params[index][2] = z;
            params[index][3] = w;
        }
    };

    // Runs passes and lends their targets. The renderer's implementation records them as steps of
    // its job; TitlePassKit runs them at once. Every target a pass reads stays valid until the
    // pass has run (acquire's targets are owned by the runner until release()).
    class JASPERENGINE_API TitlePassRunner {
    public:
        virtual ~TitlePassRunner() = default;
        virtual const TitleArtCaps& caps() const = 0;
        // a target of the pool; null (with error()) when it cannot be made. Owned by the runner
        // until release(); its contents are undefined until a pass writes it
        virtual TargetRef* acquire(const TargetKey& key) = 0;
        virtual void release(TargetRef* target) = 0;
        // the lookup of this content (uploaded once and cached by its hash); null on failure
        virtual const TitleLut* lut(const TitleLutData& data) = 0;
        // draws one pass into its output; false (with error()) when it could not
        virtual bool run(const TitlePass& pass) = 0;
        // fills the output (or its scissor) with transparent black
        virtual bool clear(TargetRef& target) = 0;
        // true when a pass since construction drew nothing because its program is still being
        // compiled (the renderer's runner runs the whole definition again later): S83's
        // analysis then skips its readback instead of reading an undrawn target
        virtual bool skippedPasses() const { return false; }
        // the first failure since the last clearError(), else empty
        virtual const std::string& error() const = 0;
        virtual void clearError() = 0;
    };

}
