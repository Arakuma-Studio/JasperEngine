// The generator programs' bodies (docs/specs/S83-title-effects-generators.md §6.7): each program
// includes its family's file and defines gen_program(), which titleart_gen.frag's main returns.
// The library's own programs (Place, GlowAdd, Grid: basics.glsl) have their own entries in main.

#ifndef TITLEART_GEN_GENERATORS_GLSL
#define TITLEART_GEN_GENERATORS_GLSL

#if TA_PROGRAM == TA_GEN_SHOCKWAVE
    #include "titleart/gen/shockwave.glsl"
    vec4 gen_program() { return gen_shockwave(); }
#elif TA_PROGRAM == TA_GEN_FRACTAL_NOISE
    #include "titleart/gen/fractal_noise.glsl"
    vec4 gen_program() { return gen_fractalNoise(); }
#elif TA_PROGRAM == TA_GEN_FIRE
    #include "titleart/gen/fire.glsl"
    vec4 gen_program() { return gen_fire(); }
#elif TA_PROGRAM == TA_GEN_SMOKE
    #include "titleart/gen/smoke.glsl"
    vec4 gen_program() { return gen_smoke(); }
#elif TA_PROGRAM == TA_GEN_AURA_TENDRILS
    #include "titleart/gen/aura.glsl"
    vec4 gen_program() { return gen_auraTendrils(); }
#elif TA_PROGRAM == TA_GEN_AURA_RISING
    #include "titleart/gen/aura.glsl"
    vec4 gen_program() { return gen_auraRising(); }
#elif TA_PROGRAM == TA_GEN_AURA_NORMALS
    #include "titleart/gen/aura.glsl"
    vec4 gen_program() { return gen_auraNormals(); }
#elif TA_PROGRAM == TA_GEN_LIGHTNING_COMPOSE
    #include "titleart/gen/lightning.glsl"
    vec4 gen_program() { return gen_lightningCompose(); }
#elif TA_PROGRAM == TA_GEN_QUADS_LIGHTNING
    #include "titleart/gen/lightning.glsl"
    vec4 gen_program() { return gen_quadsLightning(); }
#elif TA_PROGRAM == TA_GEN_MAGIC_CIRCLE
    #include "titleart/gen/magic_circle.glsl"
    vec4 gen_program() { return gen_magicCircle(); }
#elif TA_PROGRAM == TA_GEN_MESH_BRUSH_SWIRL
    #include "titleart/gen/brush_swirl.glsl"
    vec4 gen_program() { return gen_brushSwirlRibbon(); }
#elif TA_PROGRAM == TA_GEN_QUADS_SPLATTER
    #include "titleart/gen/brush_swirl.glsl"
    vec4 gen_program() { return gen_brushSwirlSplatter(); }
#elif TA_PROGRAM == TA_GEN_QUADS_PARTICLES
    #include "titleart/gen/particles.glsl"
    vec4 gen_program() { return gen_particles(); }
#elif TA_PROGRAM == TA_GEN_QUADS_INK
    #include "titleart/gen/ink_splatter.glsl"
    vec4 gen_program() { return gen_inkSplatter(); }
#elif TA_PROGRAM == TA_GEN_QUADS_SPEED_LINES
    #include "titleart/gen/speed_lines.glsl"
    vec4 gen_program() { return gen_speedLinesQuad(); }
#elif TA_PROGRAM == TA_GEN_SPEED_LINES_BETA
    #include "titleart/gen/speed_lines.glsl"
    vec4 gen_program() { return gen_speedLinesBeta(); }
#elif TA_PROGRAM == TA_GEN_QUADS_LIGHT_STREAKS
    #include "titleart/gen/light_streaks.glsl"
    vec4 gen_program() { return gen_lightStreaksQuad(); }
#elif TA_PROGRAM == TA_GEN_LIGHT_STREAK_COMPOSE
    #include "titleart/gen/light_streaks.glsl"
    vec4 gen_program() { return gen_lightStreakCompose(); }
#elif TA_PROGRAM == TA_GEN_SUNBURST
    #include "titleart/gen/sunburst.glsl"
    vec4 gen_program() { return gen_sunburst(); }
#elif TA_PROGRAM == TA_GEN_QUADS_GLINTS
    #include "titleart/gen/glints.glsl"
    vec4 gen_program() { return gen_glintsStar(); }
#elif TA_PROGRAM == TA_GEN_GLINTS_COMPOSE
    #include "titleart/gen/glints.glsl"
    vec4 gen_program() { return gen_glintsCompose(); }
#elif TA_PROGRAM == TA_GEN_LENS_FLARE
    #include "titleart/gen/lens_flare.glsl"
    vec4 gen_program() { return gen_lensFlare(); }
#elif TA_PROGRAM == TA_GEN_BACKLIGHT_SOURCE
    #include "titleart/gen/backlight.glsl"
    vec4 gen_program() { return gen_backlightSource(); }
#elif TA_PROGRAM == TA_GEN_BACKLIGHT_RADIAL
    #include "titleart/gen/backlight.glsl"
    vec4 gen_program() { return gen_backlightRadial(); }
#elif TA_PROGRAM == TA_GEN_BACKLIGHT_COMPOSE
    #include "titleart/gen/backlight.glsl"
    vec4 gen_program() { return gen_backlightCompose(); }
#elif TA_PROGRAM == TA_GEN_QUADS_TEST
    // QuadBatch's own check (QuadBatchGpu): a flat quad of its colour
    vec4 gen_program() { return v_color; }
#elif TA_PROGRAM > TA_GEN_GRID
    #error "titleart_gen.frag: unknown TA_PROGRAM"
#endif

#endif // TITLEART_GEN_GENERATORS_GLSL
