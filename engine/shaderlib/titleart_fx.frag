#version 450
// the defaults Vulkan GLSL has anyway, stated: distances, seeds, hashes and positions need fp32
precision highp float;
precision highp int;

// S83's effect catalogue (docs/specs/S83-title-effects-generators.md §6.6): one fragment program
// per TFX_PROGRAM value (the low property bits under the 'Efx' bit, effects/TfxPrograms.h), each a
// fullscreen pass over its target (or the scissor of one) in memory rows, with S80's block and
// slots (titleart/common.glsl). TA_SEEDS_FIXED and TA_DISTANCE_FIXED select the RGBA8 encodings
// of the bounds reduction and of distances; TFX_LIVE is the form S81's live compose runs (D10).
// The families' bodies are in titleart/fx/*.glsl.
//
// The raw images are read with texelFetch through non-filtering samplers (titleart/common.glsl):
// @image_sample_type u_taRaw0 unfilterable_float
// @sampler_type u_taRaw0_smp nonfiltering
// @image_sample_type u_taRaw1 unfilterable_float
// @sampler_type u_taRaw1_smp nonfiltering

#define TFX_PROGRAM_LIGHT_EMIT 1
#define TFX_PROGRAM_LIGHT_DOWN 2
#define TFX_PROGRAM_LIGHT_BLUR 3
#define TFX_PROGRAM_LIGHT_ACCUM 4
#define TFX_PROGRAM_LIGHT_COMPOSE 5
#define TFX_PROGRAM_CONTOUR_LINES 6
#define TFX_PROGRAM_CONTOUR_COMPOSE 7
#define TFX_PROGRAM_NEON_TUBE 8
#define TFX_PROGRAM_NEON_COMPOSE 9
#define TFX_PROGRAM_MOTION_BLUR 10
#define TFX_PROGRAM_LENS_DOWN 11
#define TFX_PROGRAM_LENS_GATHER 12
#define TFX_PROGRAM_LENS_FINISH 13
#define TFX_PROGRAM_TILT_FAR 14
#define TFX_PROGRAM_TILT_BLEND 15
#define TFX_PROGRAM_RADIAL_BLUR 16
#define TFX_PROGRAM_EXTRUDE 17
#define TFX_PROGRAM_MOTION_SMEAR 18
#define TFX_PROGRAM_MOTION_ECHO 19
#define TFX_PROGRAM_HALFTONE 20
#define TFX_PROGRAM_PERSPECTIVE_SHADOW 21
#define TFX_PROGRAM_FLOOR_REFLECTION 22
#define TFX_PROGRAM_WAVE_WARP 23
#define TFX_PROGRAM_CHROMATIC 24
#define TFX_PROGRAM_GRAIN 25
#define TFX_PROGRAM_GLITCH 26
#define TFX_PROGRAM_SCANLINES 27
#define TFX_PROGRAM_PIXEL_DOWN 28
#define TFX_PROGRAM_PIXEL_UP 29
#define TFX_PROGRAM_HOLOGRAPHIC 30
#define TFX_PROGRAM_DISTRESS 31
#define TFX_PROGRAM_DRIP_ORIGINS 32
#define TFX_PROGRAM_DRIP_FIELD 33
#define TFX_PROGRAM_DRIP_COMPOSE 34
#define TFX_PROGRAM_BRUSH_STROKE 35
#define TFX_PROGRAM_FROST 36
#define TFX_PROGRAM_CHALK 37
#define TFX_PROGRAM_STITCH_LAYOUT 38
#define TFX_PROGRAM_STITCH 39
#define TFX_PROGRAM_SHATTER 40
#define TFX_PROGRAM_MATERIAL_SURFACE 41
#define TFX_PROGRAM_MATERIAL_DETAIL 42

// the compiler names the program TA_PROGRAM (the low property bits); titleart/effects.glsl
// tests TA_PROGRAM against titleart.frag's numbers, none of which is meant here
#define TFX_PROGRAM TA_PROGRAM
#undef TA_PROGRAM
#define TA_PROGRAM 0

out vec4 frag_color;

#include "titleart/common.glsl"
#include "titleart/blend.glsl"
#include "titleart/patterns.glsl"
#include "titleart/effects.glsl"
#include "titleart/render/noise.glsl"
#include "titleart/fx/tfx_common.glsl"
#include "titleart/fx/light.glsl"
#include "titleart/fx/blur.glsl"
#include "titleart/fx/shadow.glsl"
#include "titleart/fx/distort.glsl"
#include "titleart/fx/stylize.glsl"
#include "titleart/fx/texture.glsl"
#include "titleart/fx/stroke.glsl"
#include "titleart/fx/shatter.glsl"
#include "titleart/fx/material.glsl"

void main() {
#if TFX_PROGRAM == TFX_PROGRAM_LIGHT_EMIT
    frag_color = tfx_lightEmit();
#elif TFX_PROGRAM == TFX_PROGRAM_LIGHT_DOWN
    frag_color = tfx_lightDown();
#elif TFX_PROGRAM == TFX_PROGRAM_LIGHT_BLUR
    frag_color = tfx_lightBlur();
#elif TFX_PROGRAM == TFX_PROGRAM_LIGHT_ACCUM
    frag_color = tfx_lightAccum();
#elif TFX_PROGRAM == TFX_PROGRAM_LIGHT_COMPOSE
    frag_color = tfx_lightCompose();
#elif TFX_PROGRAM == TFX_PROGRAM_CONTOUR_LINES
    frag_color = tfx_contourLines();
#elif TFX_PROGRAM == TFX_PROGRAM_CONTOUR_COMPOSE
    frag_color = tfx_contourCompose();
#elif TFX_PROGRAM == TFX_PROGRAM_NEON_TUBE
    frag_color = tfx_neonTube();
#elif TFX_PROGRAM == TFX_PROGRAM_NEON_COMPOSE
    frag_color = tfx_neonCompose();
#elif TFX_PROGRAM == TFX_PROGRAM_MOTION_BLUR
    frag_color = tfx_motionBlur();
#elif TFX_PROGRAM == TFX_PROGRAM_LENS_DOWN
    frag_color = tfx_lensDown();
#elif TFX_PROGRAM == TFX_PROGRAM_LENS_GATHER
    frag_color = tfx_lensGather();
#elif TFX_PROGRAM == TFX_PROGRAM_LENS_FINISH
    frag_color = tfx_lensFinish();
#elif TFX_PROGRAM == TFX_PROGRAM_TILT_FAR
    frag_color = tfx_tiltFar();
#elif TFX_PROGRAM == TFX_PROGRAM_TILT_BLEND
    frag_color = tfx_tiltBlend();
#elif TFX_PROGRAM == TFX_PROGRAM_RADIAL_BLUR
    frag_color = tfx_radialBlur();
#elif TFX_PROGRAM == TFX_PROGRAM_EXTRUDE
    frag_color = tfx_extrude();
#elif TFX_PROGRAM == TFX_PROGRAM_MOTION_SMEAR
    frag_color = tfx_motionSmear();
#elif TFX_PROGRAM == TFX_PROGRAM_MOTION_ECHO
    frag_color = tfx_motionEcho();
#elif TFX_PROGRAM == TFX_PROGRAM_HALFTONE
    frag_color = tfx_halftone();
#elif TFX_PROGRAM == TFX_PROGRAM_PERSPECTIVE_SHADOW
    frag_color = tfx_perspectiveShadow();
#elif TFX_PROGRAM == TFX_PROGRAM_FLOOR_REFLECTION
    frag_color = tfx_floorReflection();
#elif TFX_PROGRAM == TFX_PROGRAM_WAVE_WARP
    frag_color = tfx_waveWarp();
#elif TFX_PROGRAM == TFX_PROGRAM_CHROMATIC
    frag_color = tfx_chromatic();
#elif TFX_PROGRAM == TFX_PROGRAM_GRAIN
    frag_color = tfx_grain();
#elif TFX_PROGRAM == TFX_PROGRAM_GLITCH
    frag_color = tfx_glitch();
#elif TFX_PROGRAM == TFX_PROGRAM_SCANLINES
    frag_color = tfx_scanlines();
#elif TFX_PROGRAM == TFX_PROGRAM_PIXEL_DOWN
    frag_color = tfx_pixelDown();
#elif TFX_PROGRAM == TFX_PROGRAM_PIXEL_UP
    frag_color = tfx_pixelUp();
#elif TFX_PROGRAM == TFX_PROGRAM_HOLOGRAPHIC
    frag_color = tfx_holographic();
#elif TFX_PROGRAM == TFX_PROGRAM_DISTRESS
    frag_color = tfx_distress();
#elif TFX_PROGRAM == TFX_PROGRAM_DRIP_ORIGINS
    frag_color = tfx_dripOrigins();
#elif TFX_PROGRAM == TFX_PROGRAM_DRIP_FIELD
    frag_color = tfx_dripField();
#elif TFX_PROGRAM == TFX_PROGRAM_DRIP_COMPOSE
    frag_color = tfx_dripCompose();
#elif TFX_PROGRAM == TFX_PROGRAM_BRUSH_STROKE
    frag_color = tfx_brushStroke();
#elif TFX_PROGRAM == TFX_PROGRAM_FROST
    frag_color = tfx_frost();
#elif TFX_PROGRAM == TFX_PROGRAM_CHALK
    frag_color = tfx_chalk();
#elif TFX_PROGRAM == TFX_PROGRAM_STITCH_LAYOUT
    frag_color = tfx_stitchLayout();
#elif TFX_PROGRAM == TFX_PROGRAM_STITCH
    frag_color = tfx_stitch();
#elif TFX_PROGRAM == TFX_PROGRAM_SHATTER
    frag_color = tfx_shatter();
#elif TFX_PROGRAM == TFX_PROGRAM_MATERIAL_SURFACE
    frag_color = tfx_materialSurface();
#elif TFX_PROGRAM == TFX_PROGRAM_MATERIAL_DETAIL
    frag_color = tfx_materialDetail();
#else
    #error "titleart_fx.frag: unknown TFX_PROGRAM"
#endif
}
