#version 450

// @shader mesh
// @name "Fake Lit"
// @category "Stylized"
// @description "Lighting faked without lights: surfaces facing down and away from the camera blend to a shadow colour, with optional matcap shine, baked shadow texture, vertex AO, emission and axis fog. Cheap; no scene lights needed."
// @lighting none vertex
// @require position normals uv1 uv2 color
// @state blend=opaque
// @property baseColor color (1, 1, 1, 1) "Tint"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @property shadowColor color (0.36, 0.33, 0.52, 1) "Shadow colour"
// @property shadowPower range(0.1, 8) 1.5 "Downward shadow power"
// @property shadowStrength range(0, 2) 0.6 "Downward shadow strength"
// @property rimScale range(0, 2) 0.5 "Edge shadow scale"
// @property rimPower range(0.1, 8) 2.5 "Edge shadow power"
// @property matcapGain range(0, 4) 1 "Matcap gain" feature=MATCAP
// @property matcapPower range(0.1, 8) 1 "Matcap power" feature=MATCAP
// @property shadowTextureStrength range(0, 2) 1 "Shadow texture strength" feature=SHADOW_TEXTURE
// @property aoStrength range(0, 2) 1 "Vertex AO strength" feature=VERTEX_AO
// @property emissionColor hdrcolor (1, 1, 1, 1) "Emission colour" feature=EMISSION
// @property fogColor color (0.62, 0.78, 0.95, 1) "Axis fog colour" feature=AXIS_FOG
// @property fogAxis vector3 (0, 0, -1) "Axis fog direction (world, unit length)" feature=AXIS_FOG
// @property fogStart float 6 "Axis fog start (from the camera)" feature=AXIS_FOG
// @property fogLength float 20 "Axis fog length" feature=AXIS_FOG
// @texture u_baseTexture white "Base map"
// @texture u_matcapTexture black "Matcap" feature=MATCAP
// @texture u_shadowTexture white "Shadow texture (red, 1 = lit; UV2, else UV1)" feature=SHADOW_TEXTURE
// @texture u_emissionTexture white "Emission map" feature=EMISSION
// @feature MATCAP "Matcap shine"
// @feature SHADOW_TEXTURE "Baked shadow texture"
// @feature VERTEX_AO "Vertex colour AO (red)"
// @feature EMISSION "Emission"
// @feature AXIS_FOG "Axis fog"

// The owner's "General" mobile shader family. With n the world normal and v the direction
// to the camera, the shadow amount is
//     s = saturate(pow(1 - n.y, shadowPower) * shadowStrength)
//       + saturate(rimScale * pow(1 - n.v, rimPower))
//       (+ (1 - shadow texture red) * shadowTextureStrength, + saturate((1 - vertex red) * aoStrength))
// and the colour albedo (+ the additive matcap) blends to the shadow colour by saturate(s).
// Vertex lighting adds each vertex light's diffuse times the albedo on top (the shading
// stands in for the ambient, which is left out). Then emission, then the axis fog: a
// linear fog along a world direction, measured from the camera. Without VERTEX_AO the
// albedo takes the instance colour; with it the vertex colour's red is the occlusion.

uniform u_fs_customParams {
    vec4 baseColor;
    vec4 tiling;
    vec4 shadowColor;
    vec4 emissionColor;
    vec4 fogColor;
    vec3 fogAxis;
    float fogStart;
    float fogLength;
    float shadowPower;
    float shadowStrength;
    float rimScale;
    float rimPower;
    float matcapGain;
    float matcapPower;
    float shadowTextureStrength;
    float aoStrength;
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

#ifdef MATCAP
    uniform texture2D u_matcapTexture;
    uniform sampler u_matcap_smp;
#endif
#ifdef SHADOW_TEXTURE
    uniform texture2D u_shadowTexture;
    uniform sampler u_shadow_smp;
#endif
#ifdef EMISSION
    uniform texture2D u_emissionTexture;
    uniform sampler u_emission_smp;
#endif

in vec3 v_position;
in vec3 v_normal;
in vec2 v_uv1;
#if defined(SHADOW_TEXTURE) && defined(HAS_UV_SET2)
    in vec2 v_uv2;
#endif
in vec4 v_color; // the instance colour; times the vertex colour with VERTEX_AO

#ifdef USE_VERTEX_LIGHTS
    // the vertex lights' diffuse, without the ambient (fake_lit.vert)
    in vec3 v_vertexLight;
#endif

out vec4 g_finalColor;

#define FRAME_FS
#include "includes/frame.glsl"
#ifdef MATCAP
    #include "includes/matcap.glsl"
#endif
#include "includes/output.glsl"

void main() {
    vec2 uv = v_uv1 * customParams.tiling.xy + customParams.tiling.zw;
    vec4 base = customParams.baseColor * sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), uv));
    #ifndef VERTEX_AO
        base *= v_color;
    #endif

    vec3 n = normalize(v_normal);
    if (!gl_FrontFacing){
        n = -n;
    }
    vec3 toCamera = (frame.cameraPosition.w > 0.5) ? -frame.cameraDirection.xyz : normalize(frame.cameraPosition.xyz - v_position);

    vec3 color = base.rgb;
    #ifdef MATCAP
        vec3 matcap = sRGBToLinear(texture(sampler2D(u_matcapTexture, u_matcap_smp), matcapUV(n, v_position)).rgb);
        // gain and power held in range: pow of a negative base or to a zero power is NaN or undefined
        color += pow(matcap * max(customParams.matcapGain, 0.0), vec3(max(customParams.matcapPower, 1e-3)));
    #endif

    float shadow = clamp(pow(max(1.0 - n.y, 0.0), max(customParams.shadowPower, 1e-3)) * customParams.shadowStrength, 0.0, 1.0);
    shadow += clamp(customParams.rimScale * pow(max(1.0 - dot(n, toCamera), 0.0), max(customParams.rimPower, 1e-3)), 0.0, 1.0);
    #ifdef SHADOW_TEXTURE
        #ifdef HAS_UV_SET2
            vec2 shadowUV = v_uv2;
        #else
            vec2 shadowUV = v_uv1;
        #endif
        shadow += (1.0 - texture(sampler2D(u_shadowTexture, u_shadow_smp), shadowUV).r) * customParams.shadowTextureStrength;
    #endif
    #ifdef VERTEX_AO
        shadow += clamp((1.0 - v_color.r) * customParams.aoStrength, 0.0, 1.0);
    #endif
    color = mix(color, customParams.shadowColor.rgb, clamp(shadow, 0.0, 1.0));

    #ifdef USE_VERTEX_LIGHTS
        color += base.rgb * v_vertexLight;
    #endif

    #ifdef EMISSION
        color += customParams.emissionColor.rgb * sRGBToLinear(texture(sampler2D(u_emissionTexture, u_emission_smp), uv).rgb);
    #endif

    #ifdef AXIS_FOG
        float along = dot(v_position - frame.cameraPosition.xyz, customParams.fogAxis);
        float fog = clamp((along - customParams.fogStart) / max(customParams.fogLength, 1e-4), 0.0, 1.0);
        color = mix(color, customParams.fogColor.rgb, fog);
    #endif

    g_finalColor = finalizeColor(vec4(color, base.a));
}
