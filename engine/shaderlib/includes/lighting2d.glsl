// Dedicated 2D light path (USE_LIGHT2D). Lights live in the world XY plane with
// an optional virtual Z height for normal-map response; attenuation is a radial
// falloff pow(1 - d/range, falloff). Shadows (USE_SHADOWS_2D) sample per-light
// 1D polar depth rows (one atlas row per light) rendered from ShadowCaster2D segments.
//
// Declares the engine's 2D lights block (u_fs_lighting2d, filled for every mesh variant
// with USE_LIGHT2D) and, with USE_SHADOWS_2D, the shadow atlas, so mesh.frag and library
// shaders share one declaration. Fragment stage only.
//
// getNormal2D() is the normal the lights shade with; getNormal2DAt(uv) the same with the
// normal map read at uv (a shader that clamps its UV, library/sprite.frag). Normal maps are
// tangent-space, OpenGL convention (green up the image), as the 2D meshes' tangent frame
// reads them. The normal comes from, in order:
//   - a library shader with @require normalmap (SURFACE_NORMAL_MAP, ShaderRequire_NormalMap)
//     whose variant has the material's normal map, tangents and uv1: SURFACE_NORMAL_MAP_ACTIVE
//     (includes/surface_vs.glsl writes v_normal, v_tangent, v_bitangent then). This include
//     declares v_tangent and v_bitangent (unless the shader requires tangents, SURFACE_TANGENTS),
//     v_normal (unless SURFACE_NORMALS or SURFACE_TANGENTS: the shader declares it) and
//     u_normalTexture / u_normal_smp, read at v_uv1, which the shader declares (a shader that
//     declares those two itself defines LIGHT2D_NORMAL_TEXTURE_DECLARED before including);
//   - a library shader requiring tangents without a map: its v_normal;
//   - mesh.frag's surface: v_tbn and the normal map (HAS_NORMAL_MAP, at getNormalUV(), Lit's
//     Stochastic Tiling taps under LIT_STOCHASTIC), v_tbn[2] or v_normal with HAS_NORMALS;
//   - else +Z.
//
// A light's height gives normals a direction to answer (N.L with the light raised above the
// plane) only where they mean something: LIGHT2D_NORMAL_MAP is defined when getNormal2D samples
// a normal map, LIGHT2D_NORMAL_RESPONSE when that, or when the including shader defined
// LIGHT2D_SURFACE_NORMALS before including (mesh.frag's lit path: real normals, the light's
// N.L). Without LIGHT2D_NORMAL_RESPONSE getLight2DIntensity ignores n and the height: a plain
// sprite is lit as a height-0 light lights it, at any height.

#ifndef LIGHTING2D_GLSL
#define LIGHTING2D_GLSL

// the same test as includes/surface_vs.glsl's (both stages see the same defines)
#if defined(SURFACE_NORMAL_MAP) && defined(HAS_NORMAL_MAP) && defined(HAS_TANGENTS) && defined(HAS_UV_SET1)
    #define SURFACE_NORMAL_MAP_ACTIVE
#endif

// getNormal2D samples a normal map
#if defined(HAS_NORMAL_MAP) && (defined(HAS_UV_SET1) || defined(HAS_UV_SET2)) && ((defined(HAS_NORMALS) && defined(HAS_TANGENTS)) || defined(SURFACE_NORMAL_MAP_ACTIVE))
    #define LIGHT2D_NORMAL_MAP
#endif

// the light's height and n shape the light
#if defined(LIGHT2D_NORMAL_MAP) || defined(LIGHT2D_SURFACE_NORMALS)
    #define LIGHT2D_NORMAL_RESPONSE
#endif

#ifdef SURFACE_NORMAL_MAP_ACTIVE
    #ifndef SURFACE_TANGENTS
        in vec3 v_tangent;
        in vec3 v_bitangent;
        #ifndef SURFACE_NORMALS
            in vec3 v_normal;
        #endif
    #endif
    #ifndef LIGHT2D_NORMAL_TEXTURE_DECLARED
        #define LIGHT2D_NORMAL_TEXTURE_DECLARED
        uniform texture2D u_normalTexture;
        uniform sampler u_normal_smp;
    #endif
#endif

#ifdef HAS_NORMAL_MAP
    #include "includes/normal_map.glsl"
#endif

// the 2D shadows, unless the variant leaves them out (SHADOWS_2D_OFF: ShaderVariantCompiler::compile,
// a lit variant its light records leave over the bind limits; its 2D lights light it unshadowed)
#if defined(USE_SHADOWS_2D) && !defined(SHADOWS_2D_OFF)
    #define LIGHT2D_SHADOWS
#endif

#ifdef LIGHT_DATA_2D
    // the records form of a lit shader (includes/lights.glsl): ambient and atlasInfo at the end of
    // the lights' block, the 2D lights rows of u_lightRecords from lightDataBases.y (three texels
    // each: position_range, color_intensity, falloff_shadow)
    #define LIGHTS2D_AMBIENT objectLights.lights2DAmbient
    #define LIGHTS2D_ATLAS_INFO objectLights.lights2DAtlasInfo
    #define LIGHTS2D_POSITION_RANGE(i) lightRecordTexel(int(objectLights.lightDataBases.y + 0.5) + (i), 0)
    #define LIGHTS2D_COLOR_INTENSITY(i) lightRecordTexel(int(objectLights.lightDataBases.y + 0.5) + (i), 1)
    #define LIGHTS2D_FALLOFF_SHADOW(i) lightRecordTexel(int(objectLights.lightDataBases.y + 0.5) + (i), 2)
#else
uniform u_fs_lighting2d {
    vec4 position_range[MAX_LIGHTS_2D];  //position.xy, height.z, range.w
    vec4 color_intensity[MAX_LIGHTS_2D]; //color.rgb (linear) and intensity.w
    vec4 falloff_shadow[MAX_LIGHTS_2D];  //falloff.x, shadowRow.y (-1.0 if no shadow), softness.z, bias.w
    vec4 ambient;                        //ambient2D.rgb (linear, pre-multiplied by intensity) and numLights2D.w
    vec4 atlasInfo;                      //1/atlasWidth.x, 1/MAX_LIGHTS_2D.y, atlasWidth.z
} lighting2d;
    #define LIGHTS2D_AMBIENT lighting2d.ambient
    #define LIGHTS2D_ATLAS_INFO lighting2d.atlasInfo
    #define LIGHTS2D_POSITION_RANGE(i) lighting2d.position_range[i]
    #define LIGHTS2D_COLOR_INTENSITY(i) lighting2d.color_intensity[i]
    #define LIGHTS2D_FALLOFF_SHADOW(i) lighting2d.falloff_shadow[i]
#endif

#ifdef LIGHT2D_SHADOWS
    uniform texture2D u_shadow2DAtlas;
    uniform sampler u_shadow2DAtlas_smp;

    #include "includes/depth_util.glsl"
#endif

// Normal for the 2D light path, the normal map read at uv. The lit path already has
// getNormalInfo(), but it is compiled out under MATERIAL_UNLIT, so this is a minimal
// standalone version.
vec3 getNormal2DAt(vec2 uv){
#if defined(SURFACE_NORMAL_MAP_ACTIVE)
    // the library path's frame (includes/surface_vs.glsl, mirrored there); two-channel maps
    // decode alike (includes/normal_map.glsl)
    vec3 ntex = decodeNormalMap(texture(sampler2D(u_normalTexture, u_normal_smp), uv));
    return normalize(mat3(normalize(v_tangent), normalize(v_bitangent), normalize(v_normal)) * ntex);
#elif defined(SURFACE_TANGENTS)
    // a library shader requiring tangents, without a map: its normal (it has no v_tbn)
    return normalize(v_normal);
#elif defined(HAS_NORMALS)
    #ifdef HAS_TANGENTS
        #if defined(HAS_NORMAL_MAP) && defined(LIT_STOCHASTIC)
            // the built-in Lit's Stochastic Tiling (includes/pbr.glsl): the same taps and weights,
            // at the material's own UVs
            vec3 ntex = stochasticNormal(u_normalTexture, u_normal_smp, getNormalStochastic(), 1.0, STOCHASTIC_NORMAL_GL);
            return normalize(v_tbn * ntex);
        #elif defined(HAS_NORMAL_MAP) && (defined(HAS_UV_SET1) || defined(HAS_UV_SET2))
            vec3 ntex = decodeNormalMap(texture(sampler2D(u_normalTexture, u_normal_smp), uv));
            return normalize(v_tbn * ntex);
        #else
            return normalize(v_tbn[2]);
        #endif
    #else
        return normalize(v_normal);
    #endif
#else
    return vec3(0.0, 0.0, 1.0);
#endif
}

// Normal for the 2D light path, the normal map read at the surface's UV
vec3 getNormal2D(){
#if defined(SURFACE_NORMAL_MAP_ACTIVE)
    return getNormal2DAt(v_uv1);
#elif defined(HAS_NORMALS) && defined(HAS_TANGENTS) && defined(HAS_NORMAL_MAP) && (defined(HAS_UV_SET1) || defined(HAS_UV_SET2)) && !defined(SURFACE_TANGENTS) && !defined(LIT_STOCHASTIC)
    return getNormal2DAt(getNormalUV());
#else
    return getNormal2DAt(vec2(0.0));
#endif
}

#ifdef LIGHT2D_SHADOWS
// Bound clamped on every backend, as the 3D shadows' (includes/shadows.glsl): every lookup takes
// explicit LOD 0 (the atlas has one level), since the per-light loop that calls this has
// per-fragment exits, where implicit derivatives are undefined (an Intel GL hang, Doriax
// 1251c3139's pattern), and no backend has to unroll the loop
const int MAX_SHADOW2D_RADIUS = 6;  // 13 taps, Shadow2DQuality::HIGH

// 1D polar shadow map lookup: the row stores, per angle around the light, the
// normalized distance (dist/range) of the nearest occluder. PCF along the row
// with 2*radius+1 taps (radius from the scene's Shadow2DQuality, uniform-driven
// so quality changes need no shader rebuild); the taps span the penumbra
// half-width set by the light's softness, so more taps smooth the same width.
// u wraps with fract() so the seam at theta = +-pi is continuous.
float shadow2DCalculation(float row, vec2 lightToFrag, float dist01, float softness, float bias){
    float theta = atan(lightToFrag.y, lightToFrag.x); // [-pi, pi]
    float u = theta * 0.15915494309189535 + 0.5; // theta / (2 pi)
    float v = (row + 0.5) * LIGHTS2D_ATLAS_INFO.y;
    #ifndef IS_GLSL
        // rows are placed with sokol's bottom-left viewport convention, same as
        // the 3D shadow atlas (see getShadowAtlasUV)
        v = 1.0 - v;
    #endif

    int radius = clamp(int(LIGHTS2D_ATLAS_INFO.w), 0, MAX_SHADOW2D_RADIUS);
    if (radius <= 0){
        float occ = decodeDepth(textureLod(sampler2D(u_shadow2DAtlas, u_shadow2DAtlas_smp), vec2(u, v), 0.0));
        return (dist01 - bias <= occ) ? 1.0 : 0.0;
    }

    float halfWidth = LIGHTS2D_ATLAS_INFO.x * max(softness, 0.5); // in texels
    float tapStep = halfWidth / float(radius);
    float lit = 0.0;
    // The clamp leaves radius at MAX_SHADOW2D_RADIUS
    for (int t = -radius; t <= radius; ++t){
        float su = fract(u + float(t) * tapStep);
        float occ = decodeDepth(textureLod(sampler2D(u_shadow2DAtlas, u_shadow2DAtlas_smp), vec2(su, v), 0.0));
        lit += (dist01 - bias <= occ) ? 1.0 : 0.0;
    }
    return lit / float(2 * radius + 1); // 1.0 = fully lit
}
#endif

// Accumulated 2D light at fragPos: ambient2D (optional) + sum of light
// contributions. Each light's shadow attenuates only that light's term. n and the lights'
// heights count only with LIGHT2D_NORMAL_RESPONSE (see the top of this file).
vec3 getLight2DIntensity(vec3 fragPos, vec3 n, bool includeAmbient){
    vec3 acc = includeAmbient ? LIGHTS2D_AMBIENT.rgb : vec3(0.0);
    int count = int(LIGHTS2D_AMBIENT.w);
    for (int i = 0; i < MAX_LIGHTS_2D; ++i){
        if (i >= count) break;
        vec4 pr = LIGHTS2D_POSITION_RANGE(i);
        vec4 falloffShadow = LIGHTS2D_FALLOFF_SHADOW(i);
        vec2 toLight = pr.xy - fragPos.xy;
        float d = length(toLight);
        if (d >= pr.w) continue;
        float atten = pow(clamp(1.0 - d / pr.w, 0.0, 1.0), falloffShadow.x);
        float ndotl = 1.0;
        #ifdef LIGHT2D_NORMAL_RESPONSE
            if (pr.z > 0.0){
                // virtual light height above the sprite plane: gives normal maps a
                // direction to respond to (height 0 = pure radial, ignores normals)
                vec3 l = normalize(vec3(toLight, pr.z));
                ndotl = clamp(dot(n, l), 0.0, 1.0);
            }
        #endif
        float lit = 1.0;
        #ifdef LIGHT2D_SHADOWS
            float rowIdx = falloffShadow.y;
            if (rowIdx >= 0.0){
                lit = shadow2DCalculation(rowIdx, -toLight, d / pr.w,
                        falloffShadow.z, falloffShadow.w);
            }
        #endif
        vec4 colorIntensity = LIGHTS2D_COLOR_INTENSITY(i);
        acc += colorIntensity.rgb * colorIntensity.w * atten * ndotl * lit;
    }
    return acc;
}

#endif
