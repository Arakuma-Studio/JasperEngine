// The engine-written members of Water Excluders and Water Wakes (includes/water_interaction.glsl):
// put WATER_INTERACTION_PARAMS last in the shader's u_fs_customParams. Empty without the engine's
// bits (USE_WATER_EXCLUSION, USE_WATER_WAKE): the block is the shader's own then.

#ifndef WATER_INTERACTION_PARAMS_GLSL
#define WATER_INTERACTION_PARAMS_GLSL

#define WATER_EXCLUDERS_PER_DRAW 8

#if defined(USE_WATER_EXCLUSION) || defined(USE_WATER_WAKE)
    #define WATER_INTERACTION_PARAMS \
        vec4 waterExclusion; \
        vec4 waterExclusionRows[WATER_EXCLUDERS_PER_DRAW * 3]; \
        vec4 waterExclusionShapes[WATER_EXCLUDERS_PER_DRAW]; \
        vec4 waterWakeArea; \
        vec4 waterWakeParams;
#else
    #define WATER_INTERACTION_PARAMS
#endif

#endif // WATER_INTERACTION_PARAMS_GLSL
