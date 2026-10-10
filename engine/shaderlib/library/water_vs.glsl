// The vertex side of library:water's waves (WAVES), shared by its colour (water.vert) and
// depth (water_depth.vert) programs so shadows and the depth pre-pass follow the surface.
// Include after the surface include; the stage passes its own model matrix.

#define FRAME_VS
#include "includes/frame.glsl"
#include "includes/motion.glsl"
#include "library/water.glsl"

uniform u_vs_customParams {
    vec2 waveDirection;
    float waveHeight;
    float waveLength;
    float waveSpeed;
} customParams;

// raised along world up by the waves at its world XZ (heights in world units, whatever
// the object's scale)
vec3 waterWavePosition(vec3 position, mat4 modelMatrix){
    vec2 xz = (modelMatrix * vec4(position, 1.0)).xz;
    float h = waterWaves(xz, frame.time.x, customParams.waveDirection, customParams.waveHeight,
                         customParams.waveLength, customParams.waveSpeed).x;
    return position + motionWorldToLocal(modelMatrix, vec3(0.0, h, 0.0));
}
