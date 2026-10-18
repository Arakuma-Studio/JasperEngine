// The wind of library:foliage, shared by its colour (foliage.vert) and depth
// (foliage_depth.vert) programs, so shadows and the depth pre-pass sway exactly as the
// plant does. Include after the surface include; the stage passes its own model matrix.

#define FRAME_VS
#include "includes/frame.glsl"
#include "includes/motion.glsl"

uniform u_vs_customParams {
    vec3 windDirection;
    float windStrength;
    float windSpeed;
    float windScale;
    float gustStrength;
    float gustFrequency;
    float flutter;
    float heightStart;
    float heightRange;
} customParams;

// how much a vertex sways, 0 (rooted) to 1
float foliageWindMask(vec3 position, vec4 color){
    #ifdef WIND_MASK_HEIGHT
        // the height above the object's (instance's) origin, along its up axis
        float height = dot(position - motionLocalOrigin(), motionLocalUp());
        float mask = clamp((height - customParams.heightStart) / max(customParams.heightRange, 1e-4), 0.0, 1.0);
        // a stem bends more towards its tip
        return mask * mask;
    #else
        // WIND_MASK_VERTEX_COLOR: painted in the red channel
        return color.r;
    #endif
}

vec3 foliageSway(vec3 position, vec4 color, mat4 modelMatrix){
    float mask = foliageWindMask(position, color);

    vec3 direction = customParams.windDirection;
    direction /= max(length(direction), 1e-4);
    float time = frame.time.x;

    // the plant sways as one, its phase from where it stands along the wind, so a field
    // ripples with waves travelling downwind; phases in cycles, wrapped before the sine
    float along = dot(motionObjectOrigin(modelMatrix), direction) * customParams.windScale;
    float sway = sin(fract(time * customParams.windSpeed - along) * MOTION_TWO_PI);
    // gusts: slow swells that lean it downwind, 0 to 1
    float gust = 0.5 + 0.5 * sin(fract(time * customParams.gustFrequency - along * 0.37) * MOTION_TWO_PI);
    gust *= gust;
    // leaves flutter on their own, from the vertex's position
    vec3 world = (modelMatrix * vec4(position, 1.0)).xyz;
    float flutter = sin(fract(time * customParams.windSpeed * 3.1 + motionPhase(world) * 3.0) * MOTION_TWO_PI);

    float amount = customParams.windStrength * mask *
        (sway + customParams.gustStrength * gust * 2.0 + customParams.flutter * flutter);
    return position + motionWorldToLocal(modelMatrix, direction * amount);
}
