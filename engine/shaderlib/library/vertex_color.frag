#version 450

// @shader mesh
// @name "Vertex Color"
// @category "Stylized"
// @description "The mesh's vertex colours times a tint, optionally times a texture. Unlit, or Lambert lit per vertex or per pixel."
// @lighting none vertex pixel
// @require color
// @require uv1 feature=TEXTURE
// @state blend=opaque
// @property baseColor color (1, 1, 1, 1) "Tint"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)" feature=TEXTURE
// @texture u_baseTexture white "Texture" feature=TEXTURE
// @feature TEXTURE "Texture"

// colour = vertex colour (white when the mesh has none) * instance colour * tint
// (* texture). Lit modes: Lambert diffuse plus SH ambient, shadows from pixel lights.
// Without scene lights the variant is unlit. Light units: includes/lights.glsl. The UVs
// reach the fragment stage only with TEXTURE (@require uv1 feature=TEXTURE).

uniform u_fs_customParams {
    vec4 baseColor;
    vec4 tiling;
} customParams;

#ifdef TEXTURE
    uniform texture2D u_baseTexture;
    uniform sampler u_base_smp;

    #include "includes/surface_uv.glsl"
    SURFACE_UV_IN vec2 v_uv1;
#endif

in vec4 v_color; // the vertex colour times the instance colour

#ifdef USE_PUNCTUAL
    // pixel lights: the world position and normal (always written for a pixel-lit variant)
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
    vec4 base = customParams.baseColor * v_color;
    #ifdef TEXTURE
        vec2 uv = v_uv1 * customParams.tiling.xy + customParams.tiling.zw;
        base *= sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), uv));
    #endif
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
                LightSample s = getLight(i, v_position);
                float shadow = 1.0;
                #ifdef USE_SHADOWS
                    shadow = lightShadow(i, v_position, n);
                #endif
                light += lambert(s, n) * shadow;
            }
        #endif
        #ifdef USE_VERTEX_LIGHTS
            // the vertex lights' diffuse (and, in vertex mode, the ambient)
            light += v_vertexLight;
        #endif
        color *= light;
    #endif

    g_finalColor = finalizeColor(vec4(color, base.a));
}
