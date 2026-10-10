#version 450

// @shader mesh
// @name "Spin and Bob"
// @category "Animated"
// @description "Pickup animation in the vertex stage: spins about the object's up axis and bobs up and down, each object in its own phase; base map and tint, Lambert lighting in the lit modes. Its shadow moves with it."
// @lighting none vertex pixel
// @require uv1 color
// @state blend=opaque
// @property baseColor color (1, 0.78, 0.2, 1) "Base colour"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @property spinSpeed float 90 "Spin speed (degrees per second)"
// @property bobHeight float 0.15 "Bob height (world units)"
// @property bobFrequency float 0.6 "Bob frequency (cycles per second)"
// @property phaseVariation range(0, 1) 1 "Phase variation (from the world position; 0 for moving objects)"
// @texture u_baseTexture white "Base map"
// @feature VERTEX_COLOR "Vertex colour"
// @depth

// The motion (library/spin_bob.glsl) turns the normals with the positions, so the lighting
// follows the spin. The phase comes from where the object stands: a row of coins does not
// move in lockstep, and an object moving along changes its phase as it goes (set the
// variation to 0 for those). Light units: includes/lights.glsl.

uniform u_fs_customParams {
    vec4 baseColor;
    vec4 tiling;
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

in vec2 v_uv1;
in vec4 v_color; // the instance colour, times the vertex colour with VERTEX_COLOR

#ifdef USE_PUNCTUAL
    in vec3 v_position;
    in vec3 v_normal;

    #define LIGHTS_FS
    #include "includes/lights.glsl"
#endif

#ifdef USE_VERTEX_LIGHTS
    in vec3 v_vertexLight;
#endif

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    vec2 uv = v_uv1 * customParams.tiling.xy + customParams.tiling.zw;
    vec4 base = customParams.baseColor * sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), uv)) * v_color;
    vec3 color = base.rgb;

    #if defined(USE_PUNCTUAL) || defined(USE_VERTEX_LIGHTS)
        vec3 light = vec3(0.0);
        #ifdef USE_PUNCTUAL
            vec3 n = normalize(v_normal);
            if (!gl_FrontFacing){
                n = -n;
            }
            light += ambientLight(n);
            for (int i = 0; i < LIGHT_CAPACITY; i++){
                if (i >= pixelLightCount()) break;
                LightSample lightSample = getLight(i, v_position);
                float shadow = 1.0;
                #ifdef USE_SHADOWS
                    shadow = lightShadow(i, v_position, n);
                #endif
                light += lambert(lightSample, n) * shadow;
            }
        #endif
        #ifdef USE_VERTEX_LIGHTS
            light += v_vertexLight;
        #endif
        color *= light;
    #endif

    g_finalColor = finalizeColor(vec4(color, base.a));
}
