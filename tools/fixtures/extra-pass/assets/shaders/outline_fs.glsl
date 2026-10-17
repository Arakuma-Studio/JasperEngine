// Fragment stage of the fixture's hull shaders: one colour, shaded by the hull's normal when
// it has one (RAW_NORMALS), lit by the pixel lights in a lit variant.

uniform u_fs_customParams {
    vec4 outlineColor;
} customParams;

#ifdef SURFACE_NORMALS
    in vec3 v_normal;
#endif
#ifdef SURFACE_POSITION
    in vec3 v_position;
#endif
#ifdef USE_PUNCTUAL
    #define LIGHTS_FS
    #include "includes/lights.glsl"
#endif

out vec4 g_finalColor;

#include "includes/output.glsl"

void main(){
    vec4 color = customParams.outlineColor;
    #ifdef SURFACE_NORMALS
        vec3 n = normalize(v_normal);
        if (!gl_FrontFacing){
            n = -n;
        }
        #ifdef USE_PUNCTUAL
            vec3 light = ambientLight(n);
            for (int i = 0; i < LIGHT_CAPACITY; i++){
                if (i >= pixelLightCount()) break;
                light += lambert(getLight(i, v_position), n);
            }
            color.rgb *= light;
        #else
            color.rgb *= 0.8 + 0.2 * abs(n.y);
        #endif
    #endif
    g_finalColor = finalizeStraightColor(color);
}
