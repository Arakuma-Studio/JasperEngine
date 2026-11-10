// Title Art's generator programs (docs/specs/S83-title-effects-generators.md §6.7): the numbers of
// generators/TitleGen.h's GenProgram, member for member (ShaderType::TITLE_ART with the 'Gen'
// property bit: titleart_gen.vert and titleart_gen.frag compile the one TA_PROGRAM names), and how
// each program's vertices are made: TA_GEN_VERTICES 0 = the fullscreen triangle, 1 = QuadBatch's
// instanced quads, 2 = QuadBatch's meshes (both pulled from u_taVertexData, tfx_quads.glsl).

#ifndef TITLEART_GEN_PROGRAMS_GLSL
#define TITLEART_GEN_PROGRAMS_GLSL

#define TA_GEN_PLACE 1
#define TA_GEN_GLOW_ADD 2
#define TA_GEN_GRID 3
#define TA_GEN_SHOCKWAVE 10
#define TA_GEN_LENS_FLARE 11
#define TA_GEN_SUNBURST 12
#define TA_GEN_FRACTAL_NOISE 13
#define TA_GEN_FIRE 14
#define TA_GEN_SMOKE 15
#define TA_GEN_AURA_TENDRILS 16
#define TA_GEN_AURA_RISING 17
#define TA_GEN_AURA_NORMALS 18
#define TA_GEN_BACKLIGHT_SOURCE 19
#define TA_GEN_BACKLIGHT_RADIAL 20
#define TA_GEN_BACKLIGHT_COMPOSE 21
#define TA_GEN_MAGIC_CIRCLE 22
#define TA_GEN_LIGHTNING_COMPOSE 23
#define TA_GEN_LIGHT_STREAK_COMPOSE 24
#define TA_GEN_SPEED_LINES_BETA 25
#define TA_GEN_GLINTS_COMPOSE 34
#define TA_GEN_UPSTREAM_FIELD 42
#define TA_GEN_FIRE_MOBILE 43
#define TA_GEN_SMOKE_MOBILE 44
#define TA_GEN_QUADS_PARTICLES 64
#define TA_GEN_QUADS_GLINTS 65
#define TA_GEN_QUADS_INK 66
#define TA_GEN_QUADS_SPEED_LINES 67
#define TA_GEN_QUADS_LIGHT_STREAKS 68
#define TA_GEN_QUADS_LIGHTNING 69
#define TA_GEN_QUADS_SPLATTER 70
#define TA_GEN_QUADS_TEST 71
#define TA_GEN_QUADS_LENS_FLARE 78
#define TA_GEN_MESH_BRUSH_SWIRL 96

// 64..95 are QuadBatch quads, 96..127 its meshes, the rest fullscreen
#if TA_PROGRAM >= 96
    #define TA_GEN_VERTICES 2
#elif TA_PROGRAM >= 64
    #define TA_GEN_VERTICES 1
#else
    #define TA_GEN_VERTICES 0
#endif

#endif // TITLEART_GEN_PROGRAMS_GLSL
