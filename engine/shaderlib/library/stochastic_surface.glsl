// The STOCHASTIC feature of library mesh shaders: Stochastic Tiling of their tiled surface
// maps (includes/stochastic.glsl, Mikkelsen's hex-tiling), off by default. A shader with it
// declares the four parameters (the same names, ranges and defaults everywhere):
//
//   // @property stochasticCellSize range(0.25, 4) 1 "Cell Size" feature=STOCHASTIC
//   // @property stochasticBlendSharpness range(0, 1) 0.4 "Blend Sharpness" feature=STOCHASTIC
//   // @property stochasticContrast range(0, 1) 0.45 "Contrast" feature=STOCHASTIC
//   // @property stochasticRotation range(0, 1) 0 "Rotation" feature=STOCHASTIC
//   // @feature STOCHASTIC "Stochastic Tiling"
//
// as the last members of its u_fs_customParams, under #ifdef STOCHASTIC, and includes this
// after the block (fragment stage, STOCHASTIC variants only). Then, once per UV and at the
// top of main (the setup takes the UV's derivatives):
//
//   StochasticTiling st = stochasticSetup(uv, stochasticCustomParams(), 0u);
//   vec4 base = stochasticTextureLuminance(u_baseTexture, u_base_smp, st); // sets the weights
//   ... stochasticTexture / stochasticNormal for the material's other maps, with st
//
// A depth program that alpha clips samples the base map the same way, so its alpha matches.

#ifndef STOCHASTIC_SURFACE_GLSL
#define STOCHASTIC_SURFACE_GLSL

#include "includes/stochastic.glsl"

vec4 stochasticCustomParams(){
    return stochasticParams(customParams.stochasticCellSize, customParams.stochasticBlendSharpness,
                            customParams.stochasticContrast, customParams.stochasticRotation);
}

#endif
