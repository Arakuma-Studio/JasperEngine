// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md §6.1 D1, R29-R31)
// - Purpose: the GPU programs of S83's effect catalogue: one TFX_PROGRAM_* of
//   engine/shaderlib/titleart_fx.frag per pass family member (includes in
//   engine/shaderlib/titleart/fx/), addressed as ShaderType::TITLE_ART programs with the
//   TITLE_VARIANT_CATALOGUE_EFFECT bit ('Efx'), so their numbers never collide with
//   titleart.frag's (S80's effects) or the renderer's. The numbers are append-only (cached
//   shader keys).
// - Variants: a program that reads the bounds reduction or jump-flood seeds takes 'Sfx' on a
//   device whose seeds are RGBA8, one that reads a distance field takes 'Dfx' likewise
//   (TitleArtCaps); `live` adds 'Liv' (TFX_LIVE, the form S81's live compose runs, D10).
// - Thread-safety: pure functions.

#pragma once

#include "Export.h"
#include "shader/ShaderBuildTypes.h"
#include "titleart/render/TitleEffectContext.h"
#include "titleart/render/TitlePasses.h"

#include <cstdint>

namespace jasperengine::titleart::tfx {

    // titleart_fx.frag's TFX_PROGRAM_* values (append-only)
    enum class Program : uint8_t {
        LightEmit = 1,
        LightDown = 2,
        LightBlur = 3,
        LightAccum = 4,
        LightCompose = 5,
        ContourLines = 6,
        ContourCompose = 7,
        NeonTube = 8,
        NeonCompose = 9,
        MotionBlur = 10,
        LensDown = 11,
        LensGather = 12,
        LensFinish = 13,
        TiltFar = 14,
        TiltBlend = 15,
        RadialBlur = 16,
        Extrude = 17,
        MotionSmear = 18,
        MotionEcho = 19,
        Halftone = 20,
        PerspectiveShadow = 21,
        FloorReflection = 22,
        WaveWarp = 23,
        Chromatic = 24,
        Grain = 25,
        Glitch = 26,
        Scanlines = 27,
        PixelDown = 28,
        PixelUp = 29,
        Holographic = 30,
        Distress = 31,
        DripOrigins = 32,
        DripField = 33,
        DripCompose = 34,
        BrushStroke = 35,
        Frost = 36,
        Chalk = 37,
        StitchLayout = 38,
        Stitch = 39,
        Shatter = 40,
        MaterialSurface = 41,
        MaterialDetail = 42
    };
    constexpr int PROGRAM_LAST = 42;

    // "light_emit", "shatter" ... (pass labels, traces, tests)
    JASPERENGINE_API const char* programName(Program program);
    // what the program reads besides colour: the bounds reduction or seeds ('Sfx'), a distance ('Dfx')
    JASPERENGINE_API bool readsSeeds(Program program);
    JASPERENGINE_API bool readsDistance(Program program);
    // the variant bits a device with these caps needs ('Efx' always)
    JASPERENGINE_API uint64_t programVariant(Program program, const TitleArtCaps& caps, bool live = false);
    JASPERENGINE_API ShaderKey programKey(Program program, uint64_t variant);

    // a pass of `program` into `output` with the context's frame and px_scale (fx::makePass's
    // form for S83's programs)
    JASPERENGINE_API TitlePass makePass(const EffectContext& ctx, Program program, const char* label, TargetRef& output);

    // the light headroom of D14: 1 on the float path; 4 where the light targets are SRGB8A8 (they
    // hold light / 4, so values up to 4 survive to the final compose)
    JASPERENGINE_API float lightHeadroom(const TitleArtCaps& caps);
    // tests: the headroom every render takes (> 0), or the caps' again (0): D14's red control
    // renders the RGBA8 path without it
    JASPERENGINE_API void setLightHeadroomForTesting(float headroom);

}
