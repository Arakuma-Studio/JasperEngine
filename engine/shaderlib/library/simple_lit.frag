#version 450

// @shader mesh
// @name "Simple Lit"
// @category "Lit"
// @description "Blinn-Phong lighting: diffuse from every light, specular and shadows from the pixel lights, optional normal map and emission. The mobile tier of Lit."
// @lighting pixel vertex mixed
// @require uv1 color
// @state blend=opaque
// @property baseColor color (1, 1, 1, 1) "Base colour"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @property specularColor color (0.2, 0.2, 0.2, 1) "Specular colour"
// @property gloss range(0, 1) 0.5 "Glossiness"
// @property normalStrength range(0, 2) 1 "Normal strength" feature=NORMAL_MAP
// @property emissionColor hdrcolor (1, 1, 1, 1) "Emission colour" feature=EMISSION
// @texture u_baseTexture white "Base map"
// @texture u_normalTexture normal "Normal map" feature=NORMAL_MAP
// @texture u_emissionTexture white "Emission map" feature=EMISSION
// @feature VERTEX_COLOR "Vertex colour"
// @feature NORMAL_MAP "Normal map"
// @feature EMISSION "Emission"

// Pixel lights (pixel, mixed) get the full model: Lambert diffuse, normalised Blinn-Phong
// specular, shadows and the normal map. Vertex lights (vertex, mixed) are diffuse only,
// from the vertex normal. Without scene lights the variant is unlit (base colour and
// emission), as the built-in Lit. Light units: includes/lights.glsl.

uniform u_fs_customParams {
    vec4 baseColor;
    vec4 tiling;
    vec4 specularColor;
    vec4 emissionColor;
    float gloss;
    float normalStrength;
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

in vec2 v_uv1;
in vec4 v_color; // the instance colour, times the vertex colour with VERTEX_COLOR

#ifdef USE_PUNCTUAL
    // pixel lights: the world position and normal (always written for a pixel-lit variant)
    in vec3 v_position;
    in vec3 v_normal;

    #define FRAME_FS
    #include "includes/frame.glsl"
    #define LIGHTS_FS
    #include "includes/lights.glsl"

    #ifdef NORMAL_MAP
        uniform texture2D u_normalTexture;
        uniform sampler u_normal_smp;
    #endif
#endif

#ifdef USE_VERTEX_LIGHTS
    in vec3 v_vertexLight;
#endif

#ifdef EMISSION
    uniform texture2D u_emissionTexture;
    uniform sampler u_emission_smp;
#endif

out vec4 g_finalColor;

#include "includes/output.glsl"

#if defined(USE_PUNCTUAL) && defined(NORMAL_MAP)
// the tangent frame from the screen-space derivatives of the position and UV (no tangent
// attribute needed), as Lit does for a mesh without tangents
vec3 perturbNormal(vec3 normal, vec2 uv){
    vec3 dpx = dFdx(v_position);
    vec3 dpy = dFdy(v_position);
    vec2 duvx = dFdx(uv);
    vec2 duvy = dFdy(uv);
    float det = duvx.x * duvy.y - duvy.x * duvx.y;
    if (abs(det) < 1e-12){
        return normal;
    }
    vec3 tangent = (duvy.y * dpx - duvx.y * dpy) / det;
    tangent = tangent - normal * dot(normal, tangent);
    float tangentLength = length(tangent);
    if (tangentLength < 1e-8){
        return normal;
    }
    tangent /= tangentLength;
    vec3 bitangent = cross(normal, tangent);

    vec3 mapped = texture(sampler2D(u_normalTexture, u_normal_smp), uv).rgb * 2.0 - vec3(1.0);
    mapped.xy *= customParams.normalStrength;
    return normalize(mat3(tangent, bitangent, normal) * mapped);
}
#endif

void main() {
    vec2 uv = v_uv1 * customParams.tiling.xy + customParams.tiling.zw;
    vec4 base = customParams.baseColor * sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), uv)) * v_color;
    vec3 color = base.rgb;

    #if defined(USE_PUNCTUAL) || defined(USE_VERTEX_LIGHTS)
        vec3 diffuseLight = vec3(0.0);
        vec3 specularLight = vec3(0.0);

        #ifdef USE_PUNCTUAL
            vec3 n = normalize(v_normal);
            #ifdef NORMAL_MAP
                n = perturbNormal(n, uv);
            #endif
            // a double-sided surface shades its back with the reversed normal
            if (!gl_FrontFacing){
                n = -n;
            }
            vec3 v = normalize(frame.cameraPosition.xyz - v_position);
            float shininess = exp2(10.0 * customParams.gloss + 1.0);

            diffuseLight += ambientLight(n);
            for (int i = 0; i < LIGHT_CAPACITY; i++){
                if (i >= pixelLightCount()) break;
                LightSample light = getLight(i, v_position);
                float shadow = 1.0;
                #ifdef USE_SHADOWS
                    shadow = lightShadow(i, v_position, n);
                #endif
                diffuseLight += lambert(light, n) * shadow;
                specularLight += blinnPhong(light, n, v, shininess) * shadow;
            }
        #endif

        #ifdef USE_VERTEX_LIGHTS
            // the vertex lights' diffuse (and, in vertex mode, the ambient)
            diffuseLight += v_vertexLight;
        #endif

        color = base.rgb * diffuseLight + customParams.specularColor.rgb * specularLight;
    #endif

    #ifdef EMISSION
        color += customParams.emissionColor.rgb * sRGBToLinear(texture(sampler2D(u_emissionTexture, u_emission_smp), uv).rgb);
    #endif

    g_finalColor = finalizeColor(vec4(color, base.a));
}
