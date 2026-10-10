#version 450

// @shader mesh
// @name "Triplanar"
// @category "Stylized"
// @description "Projects a texture along the three axes and blends by the normal, so meshes need no UVs and tiling stays even. Optional separate floor and ceiling textures and a dirt overlay. Unlit or Lambert lit."
// @lighting none pixel vertex
// @require position normals color
// @state blend=opaque
// @property baseColor color (1, 1, 1, 1) "Tint"
// @property tiling float 1 "Tiling (repeats per unit)"
// @property sharpness range(1, 16) 4 "Blend sharpness"
// @property noiseTiling float 0.15 "Dirt noise tiling (repeats per unit)" feature=NOISE_MASK
// @property dirtColor color (0.23, 0.17, 0.11, 1) "Dirt colour" feature=NOISE_MASK
// @property dirtStrength range(0, 1) 0.65 "Dirt strength" feature=NOISE_MASK
// @property dirtContrast range(0.5, 8) 2.5 "Dirt contrast" feature=NOISE_MASK
// @texture u_baseTexture white "Texture (walls with TOP_BOTTOM)"
// @texture u_topTexture white "Floor texture (faces up)" feature=TOP_BOTTOM
// @texture u_bottomTexture white "Ceiling texture (faces down)" feature=TOP_BOTTOM
// @texture u_noiseTexture white "Dirt noise (red)" feature=NOISE_MASK
// @keywords SPACE "Space" WORLD OBJECT
// @feature TOP_BOTTOM "Separate floor and ceiling textures"
// @feature NOISE_MASK "Dirt overlay"

// The three projections (YZ for faces along X, XZ along Y, XY along Z, each mirrored by
// the normal's sign so the texture reads the right way round on every face) blend by
// pow(abs(n), sharpness), normalised. WORLD projects the world position (textures line up
// across objects and stay put when an object moves), OBJECT the object position times the
// object's scale (the texture moves with the object, same tiling at any scale). With
// TOP_BOTTOM faces looking up take the floor texture, faces looking down the ceiling one,
// the rest the base texture. NOISE_MASK blends to the dirt colour by
// saturate(pow(1 - noise, dirtContrast) * 3) * dirtStrength, the noise projected the same way.
// Lit modes: Lambert diffuse plus SH ambient, shadows from pixel lights. Without scene
// lights the variant is unlit. Light units: includes/lights.glsl.

uniform u_fs_customParams {
    vec4 baseColor;
    vec4 dirtColor;
    float tiling;
    float sharpness;
    float noiseTiling;
    float dirtStrength;
    float dirtContrast;
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

#ifdef TOP_BOTTOM
    uniform texture2D u_topTexture;
    uniform sampler u_top_smp;
    uniform texture2D u_bottomTexture;
    uniform sampler u_bottom_smp;
#endif
#ifdef NOISE_MASK
    uniform texture2D u_noiseTexture;
    uniform sampler u_noise_smp;
#endif

#if defined(SPACE_WORLD) || defined(USE_PUNCTUAL)
    in vec3 v_position;
    in vec3 v_normal;
#endif
#ifdef SPACE_OBJECT
    in vec3 v_objectPosition;
    in vec3 v_objectNormal;
#endif
in vec4 v_color; // the instance colour

#ifdef USE_PUNCTUAL
    #define LIGHTS_FS
    #include "includes/lights.glsl"
#endif
#ifdef USE_VERTEX_LIGHTS
    in vec3 v_vertexLight;
#endif

out vec4 g_finalColor;

#include "includes/output.glsl"

// the three projections of a position, mirrored by the normal's sign
struct TriplanarUV { vec2 x; vec2 y; vec2 z; };

TriplanarUV triplanarUV(vec3 p, vec3 n){
    vec3 s = vec3(n.x < 0.0 ? -1.0 : 1.0, n.y < 0.0 ? -1.0 : 1.0, n.z < 0.0 ? -1.0 : 1.0);
    TriplanarUV uv;
    uv.x = vec2(-p.z * s.x, -p.y);
    uv.y = vec2(p.x, p.z * s.y);
    uv.z = vec2(p.x * s.z, -p.y);
    return uv;
}

void main() {
    #ifdef SPACE_OBJECT
        vec3 p = v_objectPosition;
        vec3 n = normalize(v_objectNormal);
    #else
        vec3 p = v_position;
        vec3 n = normalize(v_normal);
    #endif

    vec3 weights = pow(abs(n), vec3(customParams.sharpness));
    weights /= max(weights.x + weights.y + weights.z, 1e-5);

    TriplanarUV uv = triplanarUV(p * customParams.tiling, n);
    vec4 sideX = texture(sampler2D(u_baseTexture, u_base_smp), uv.x);
    vec4 sideZ = texture(sampler2D(u_baseTexture, u_base_smp), uv.z);
    #ifdef TOP_BOTTOM
        // both sampled (no texture fetch under divergent control flow), picked by the sign
        vec4 sideY = mix(texture(sampler2D(u_bottomTexture, u_bottom_smp), uv.y),
                         texture(sampler2D(u_topTexture, u_top_smp), uv.y), step(0.0, n.y));
    #else
        vec4 sideY = texture(sampler2D(u_baseTexture, u_base_smp), uv.y);
    #endif
    vec4 albedo = sRGBToLinear(sideX * weights.x + sideY * weights.y + sideZ * weights.z);
    albedo *= customParams.baseColor * v_color;

    #ifdef NOISE_MASK
        TriplanarUV noiseUV = triplanarUV(p * customParams.noiseTiling, n);
        float noise = texture(sampler2D(u_noiseTexture, u_noise_smp), noiseUV.x).r * weights.x
                    + texture(sampler2D(u_noiseTexture, u_noise_smp), noiseUV.y).r * weights.y
                    + texture(sampler2D(u_noiseTexture, u_noise_smp), noiseUV.z).r * weights.z;
        float dirt = clamp(pow(max(1.0 - noise, 0.0), customParams.dirtContrast) * 3.0, 0.0, 1.0) * customParams.dirtStrength;
        albedo.rgb = mix(albedo.rgb, customParams.dirtColor.rgb, dirt);
    #endif

    vec3 color = albedo.rgb;

    #if defined(USE_PUNCTUAL) || defined(USE_VERTEX_LIGHTS)
        vec3 light = vec3(0.0);
        #ifdef USE_PUNCTUAL
            vec3 worldNormal = normalize(v_normal);
            if (!gl_FrontFacing){
                worldNormal = -worldNormal;
            }
            light += ambientLight(worldNormal);
            for (int i = 0; i < LIGHT_CAPACITY; i++){
                if (i >= pixelLightCount()) break;
                LightSample s = getLight(i, v_position);
                float shadow = 1.0;
                #ifdef USE_SHADOWS
                    shadow = lightShadow(i, v_position, worldNormal);
                #endif
                light += lambert(s, worldNormal) * shadow;
            }
        #endif
        #ifdef USE_VERTEX_LIGHTS
            // the vertex lights' diffuse (and, in vertex mode, the ambient)
            light += v_vertexLight;
        #endif
        color *= light;
    #endif

    g_finalColor = finalizeColor(vec4(color, albedo.a));
}
