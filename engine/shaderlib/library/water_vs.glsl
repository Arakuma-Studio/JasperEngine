// The vertex side of library:water's waves (WAVES), shared by its colour (water.vert) and
// depth (water_depth.vert) programs so shadows and the depth pre-pass follow the surface.
// Include after the surface include; the stage passes its own model matrix.

#include "includes/motion.glsl"
#include "library/water.glsl"

// the clock the waves run on: the scene clock (customParams.sceneTime) for this library's
// entry points, which define WATER_SCENE_CLOCK; a fork copied from an older library:water has
// its .frag on frame.time, so its waves stay on that too
#ifdef WATER_SCENE_CLOCK
    #define WATER_VS_TIME customParams.sceneTime
#else
    #define FRAME_VS
    #include "includes/frame.glsl"
    #define WATER_VS_TIME frame.time.x
#endif

uniform u_vs_customParams {
    vec2 waveDirection;
    float waveHeight;
    float waveLength;
    float waveSpeed;
    float waveSteepness;
    // written by the engine: the scene clock (Scene::getSceneTime)
    float sceneTime;
} customParams;

// the world XZ the vertex rests at (GERSTNER moves it sideways): water.vert passes it on
vec2 waterRestXZ = vec2(0.0);

// moved by the waves at its world XZ (heights in world units, whatever the object's scale):
// raised along world up, and with GERSTNER also moved along the waves
vec3 waterWavePosition(vec3 position, mat4 modelMatrix){
    vec2 xz = (modelMatrix * vec4(position, 1.0)).xz;
    waterRestXZ = xz;
    #ifdef GERSTNER
        vec3 dPdx;
        vec3 dPdz;
        vec3 offset = waterGerstner(xz, WATER_VS_TIME, customParams.waveDirection, customParams.waveHeight,
                                    customParams.waveLength, customParams.waveSpeed, customParams.waveSteepness,
                                    vec4(1.0), dPdx, dPdz);
        return position + motionWorldToLocal(modelMatrix, offset);
    #else
        float h = waterWaves(xz, WATER_VS_TIME, customParams.waveDirection, customParams.waveHeight,
                             customParams.waveLength, customParams.waveSpeed).x;
        return position + motionWorldToLocal(modelMatrix, vec3(0.0, h, 0.0));
    #endif
}
