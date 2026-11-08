// Based on https://github.com/KhronosGroup/glTF-Sample-Viewer shaders

#include "includes/srgb.glsl"
// (only where a normal map is sampled: the other variants stay the program they were)
#ifdef HAS_NORMAL_MAP
    #include "includes/normal_map.glsl"
#endif

float clampedDot(vec3 x, vec3 y){
    return clamp(dot(x, y), 0.0, 1.0);
}

vec4 getVertexColor(){
   vec4 color = vec4(1.0, 1.0, 1.0, 1.0);

    #ifdef HAS_VERTEX_COLOR_VEC3
        color.rgb = v_color.rgb;
    #endif
    #if defined(HAS_VERTEX_COLOR_VEC4) || defined(HAS_INSTANCING)
        color = v_color;
    #endif

   return color;
}

// Per-texture UV set selection (glTF texCoord). When a second UV set is present
// the renderer supplies u_fs_texCoordSets telling each texture which set to sample
// (0 = v_uv1, 1 = v_uv2). Without a second set everything samples v_uv1.
#ifdef HAS_UV_SET2
    #ifdef PBR_EXT
        // carried by the extended lobes' block (includes/pbr_ext_slots.glsl)
        struct PbrTexCoordSets { ivec4 set0; ivec4 set1; };
        #define texCoordSets PbrTexCoordSets(ivec4(pbrExt.set0), ivec4(pbrExt.set1))
    #else
    uniform u_fs_texCoordSets {
        ivec4 set0; // x=baseColor, y=metallicRoughness, z=occlusion, w=emissive
        ivec4 set1; // x=normal
    } texCoordSets;
    #endif

    vec2 selectUV(int set)        { return (set == 1) ? v_uv2 : v_uv1; }
    vec2 getBaseColorUV()         { return selectUV(texCoordSets.set0.x); }
    vec2 getMetallicRoughnessUV() { return selectUV(texCoordSets.set0.y); }
    vec2 getOcclusionUV()         { return selectUV(texCoordSets.set0.z); }
    vec2 getEmissiveUV()          { return selectUV(texCoordSets.set0.w); }
    vec2 getNormalUV()            { return selectUV(texCoordSets.set1.x); }
    // hex tiling (includes/pbr_ext_slots.glsl): a texture of UV set 0 is hex sampled
    #ifdef PBR_HEX_ACTIVE
        #define PBR_HEX_BASECOLOR (texCoordSets.set0.x == 0)
        #define PBR_HEX_METALLICROUGHNESS (texCoordSets.set0.y == 0)
        #define PBR_HEX_OCCLUSION (texCoordSets.set0.z == 0)
        #define PBR_HEX_EMISSIVE (texCoordSets.set0.w == 0)
        #define PBR_HEX_NORMALMAP (texCoordSets.set1.x == 0)
    #endif
#elif defined(HAS_UV_SET1)
    vec2 getBaseColorUV()         { return v_uv1; }
    vec2 getMetallicRoughnessUV() { return v_uv1; }
    vec2 getOcclusionUV()         { return v_uv1; }
    vec2 getEmissiveUV()          { return v_uv1; }
    vec2 getNormalUV()            { return v_uv1; }
    #ifdef PBR_HEX_ACTIVE
        #define PBR_HEX_BASECOLOR true
        #define PBR_HEX_METALLICROUGHNESS true
        #define PBR_HEX_OCCLUSION true
        #define PBR_HEX_EMISSIVE true
        #define PBR_HEX_NORMALMAP true
    #endif
#endif

// Stochastic Tiling of the material's maps (USE_STOCHASTIC_TILING, Material::stochasticTiling;
// includes/stochastic.glsl): every map read by its UV through the hex lattice of its UV set,
// one lattice per set (seed 0 for v_uv1, 1 for v_uv2, so the two do not line up). The base
// colour is read first (getBaseColor, the first thing main does: the lattices take their
// gradients there, in uniform control flow) and its taps' luminance gives the weights every
// other map of the same set blends with, so all of them change cell alike; a map on the other
// set blends with that set's geometric weights. The shader declares the material's four values
// (cell size, blend sharpness, contrast, rotation) and names them with
//
//   #define LIT_STOCHASTIC_PARAMS pbrParams.stochastic
//
// before including this: a fork of mesh.frag made before Stochastic Tiling does not, and keeps
// sampling as it did. The tangent frame's derivatives (getNormalInfo) stay those of the plain UV.
#if defined(USE_STOCHASTIC_TILING) && defined(LIT_STOCHASTIC_PARAMS) && (defined(HAS_UV_SET1) || defined(HAS_UV_SET2))
    #define LIT_STOCHASTIC
    #include "includes/stochastic.glsl"

    StochasticTiling litStochastic1;        // v_uv1
    #ifdef HAS_UV_SET2
        StochasticTiling litStochastic2;    // v_uv2

        StochasticTiling getStochastic(int set){
            if (set == 1)
                return litStochastic2;
            return litStochastic1;
        }
        StochasticTiling getMetallicRoughnessStochastic() { return getStochastic(texCoordSets.set0.y); }
        StochasticTiling getOcclusionStochastic()         { return getStochastic(texCoordSets.set0.z); }
        StochasticTiling getEmissiveStochastic()          { return getStochastic(texCoordSets.set0.w); }
        StochasticTiling getNormalStochastic()            { return getStochastic(texCoordSets.set1.x); }
    #else
        StochasticTiling getMetallicRoughnessStochastic() { return litStochastic1; }
        StochasticTiling getOcclusionStochastic()         { return litStochastic1; }
        StochasticTiling getEmissiveStochastic()          { return litStochastic1; }
        StochasticTiling getNormalStochastic()            { return litStochastic1; }
    #endif

    // The lattices of the UV sets the maps read (uniform control flow: the gradients)
    void setupLitStochastic(){
        vec4 params = stochasticParams(LIT_STOCHASTIC_PARAMS.x, LIT_STOCHASTIC_PARAMS.y,
                                       LIT_STOCHASTIC_PARAMS.z, LIT_STOCHASTIC_PARAMS.w);
        vec2 dUV1dx = dFdx(v_uv1);
        vec2 dUV1dy = dFdy(v_uv1);
        #ifdef HAS_UV_SET2
            vec2 dUV2dx = dFdx(v_uv2);
            vec2 dUV2dy = dFdy(v_uv2);
            bvec4 onSet2 = equal(texCoordSets.set0, ivec4(1));
            if (!all(onSet2) || texCoordSets.set1.x != 1)
                litStochastic1 = stochasticSetup(v_uv1, dUV1dx, dUV1dy, params, 0u);
            if (any(onSet2) || texCoordSets.set1.x == 1)
                litStochastic2 = stochasticSetup(v_uv2, dUV2dx, dUV2dy, params, 1u);
        #else
            litStochastic1 = stochasticSetup(v_uv1, dUV1dx, dUV1dy, params, 0u);
        #endif
    }

    // The base colour (as stored), its taps' luminance becoming its set's weights
    vec4 getStochasticBaseColorTexture(){
        #ifdef HAS_UV_SET2
            if (texCoordSets.set0.x == 1)
                return stochasticTextureLuminance(u_baseColorTexture, u_baseColor_smp, litStochastic2);
        #endif
        return stochasticTextureLuminance(u_baseColorTexture, u_baseColor_smp, litStochastic1);
    }
#endif

vec4 getBaseColor(){
    vec4 baseColor = pbrParams.baseColorFactor;
    // (upstream's Stochastic Tiling, MESH key bit 'Sto', and S11's hex tiling, builtin:lit#PBR_HEX_*,
    // both randomize the tiling: a material with both takes Stochastic Tiling)
    #if defined(LIT_STOCHASTIC) && (defined(HAS_UV_SET1) || defined(HAS_UV_SET2))
        setupLitStochastic();
        #ifdef USE_MIRROR
            // main reads the reflection instead (its texture is bound here): the other maps
            // keep the geometric weights
            baseColor *= sRGBToLinear(texture(sampler2D(u_baseColorTexture, u_baseColor_smp), getBaseColorUV()));
        #else
            baseColor *= sRGBToLinear(getStochasticBaseColorTexture());
        #endif
    #elif defined(PBR_HEX_ACTIVE)
        baseColor *= sRGBToLinear(pbrHexBaseColor(getBaseColorUV(), PBR_HEX_BASECOLOR));
    #elif defined(HAS_UV_SET1) || defined(HAS_UV_SET2)
        baseColor *= sRGBToLinear(texture(sampler2D(u_baseColorTexture, u_baseColor_smp), getBaseColorUV()));
    #endif
    return baseColor * getVertexColor();
}
#ifndef MATERIAL_UNLIT
    MaterialInfo getMetallicRoughnessInfo(MaterialInfo info, float f0_ior){
        info.metallic = pbrParams.metallicFactor;
        info.perceptualRoughness = pbrParams.roughnessFactor;
        // Roughness is stored in the 'g' channel, metallic is stored in the 'b' channel.
        // This layout intentionally reserves the 'r' channel for (optional) occlusion map data
        #ifdef LIT_STOCHASTIC
            vec4 mrSample = stochasticTexture(u_metallicRoughnessTexture, u_metallicRoughness_smp, getMetallicRoughnessStochastic());
            info.perceptualRoughness *= mrSample.g;
            info.metallic *= mrSample.b;
        #elif defined(HAS_UV_SET1) || defined(HAS_UV_SET2)
            #ifdef PBR_HEX_ACTIVE
                vec4 mrSample = PBR_HEX_TEXTURE(u_metallicRoughnessTexture, u_metallicRoughness_smp, getMetallicRoughnessUV(), PBR_HEX_METALLICROUGHNESS);
            #else
                vec4 mrSample = texture(sampler2D(u_metallicRoughnessTexture, u_metallicRoughness_smp), getMetallicRoughnessUV());
            #endif
            info.perceptualRoughness *= mrSample.g;
            info.metallic *= mrSample.b;
        #endif
        // Achromatic f0 based on IOR.
        vec3 f0 = vec3(f0_ior);
        info.albedoColor = mix(info.baseColor.rgb * (vec3(1.0) - f0),  vec3(0), info.metallic);
        info.f0 = mix(f0, info.baseColor.rgb, info.metallic);
        return info;
    }

    #if defined(HAS_UV_SET1) || defined(HAS_UV_SET2)
        vec4 getOcclusionTexture(){
            #ifdef TERRAIN_HOLES
                // (mesh.frag leaves the texture out: T3 R21)
                return vec4(1.0);
            #elif defined(LIT_STOCHASTIC)
                return stochasticTexture(u_occlusionTexture, u_occlusion_smp, getOcclusionStochastic());
            #elif defined(PBR_HEX_ACTIVE)
                return PBR_HEX_TEXTURE(u_occlusionTexture, u_occlusion_smp, getOcclusionUV(), PBR_HEX_OCCLUSION);
            #else
                return texture(sampler2D(u_occlusionTexture, u_occlusion_smp), getOcclusionUV());
            #endif
        }

        // (mesh.frag leaves the texture out of a painted terrain with the post-processing colour)
        #ifndef NO_EMISSIVE_TEXTURE
            vec4 getEmissiveTexture(){
                #ifdef LIT_STOCHASTIC
                    return stochasticTexture(u_emissiveTexture, u_emissive_smp, getEmissiveStochastic());
                #elif defined(PBR_HEX_ACTIVE)
                    return PBR_HEX_TEXTURE(u_emissiveTexture, u_emissive_smp, getEmissiveUV(), PBR_HEX_EMISSIVE);
                #else
                    return texture(sampler2D(u_emissiveTexture, u_emissive_smp), getEmissiveUV());
                #endif
            }
        #endif
    #endif
#endif

#ifndef MATERIAL_UNLIT
// Get normal, tangent and bitangent vectors.
NormalInfo getNormalInfo(){
    vec3 n, t, b, ng;
    // Compute geometrical TBN:
    #ifdef HAS_TANGENTS
        // Trivial TBN computation, present as vertex attribute.
        // Normalize eigenvectors as matrix is linearly interpolated.
        t = normalize(v_tbn[0]);
        b = normalize(v_tbn[1]);
        ng = normalize(v_tbn[2]);
    #else
        // Normals are either present as vertex attributes or approximated.
        #ifdef HAS_NORMALS
            ng = normalize(v_normal);
        #else
            ng = normalize(cross(dFdx(v_position), dFdy(v_position)));
            // the derivatives' handedness follows the framebuffer's y, which differs between
            // backends: the derived normal is turned toward the eye, so it needs no back-face flip
            #if defined(LIT_FRAME_DECLARED)
                vec3 flatToEye = litFrame.cameraPosition_fogEnd.xyz - v_position;
            #elif defined(FRAME_FS_DECLARED)
                vec3 flatToEye = frame.cameraPosition.xyz - v_position;
            #else
                vec3 flatToEye = lighting.eyePos.xyz - v_position;
            #endif
            ng = dot(ng, flatToEye) < 0.0 ? -ng : ng;
        #endif
        #if defined(HAS_UV_SET1) || defined(HAS_UV_SET2)
            vec2 normalUV = getNormalUV();
            vec3 uv_dx = dFdx(vec3(normalUV, 0.0));
            vec3 uv_dy = dFdy(vec3(normalUV, 0.0));
            vec3 t_ = (uv_dy.t * dFdx(v_position) - uv_dx.t * dFdy(v_position)) /
                (uv_dx.s * uv_dy.t - uv_dy.s * uv_dx.t);

            t = normalize(t_ - ng * dot(ng, t_));
            b = cross(ng, t);
        #endif
    #endif

    // Compute pertubed normals (a normal map needs a UV set to sample it by: a terrain
    // without blend maps or textures has none)
    #if defined(HAS_NORMAL_MAP) && defined(LIT_STOCHASTIC)
        // blended as slopes with its set's weights, each cell's turn undone (the plain read
        // below for a single tap); the frame above is the plain UV's
        n = stochasticNormal(u_normalTexture, u_normal_smp, getNormalStochastic(), normalScale, STOCHASTIC_NORMAL_GL);
        n = mat3(t, b, ng) * normalize(n);
    #elif defined(HAS_NORMAL_MAP) && (defined(HAS_UV_SET1) || defined(HAS_UV_SET2))
        #ifdef PBR_HEX_ACTIVE
            n = PBR_HEX_NORMAL(u_normalTexture, u_normal_smp, getNormalUV(), PBR_HEX_NORMALMAP);
        #else
            n = decodeNormalMap(texture(sampler2D(u_normalTexture, u_normal_smp), getNormalUV()));
        #endif
        n *= vec3(normalScale, normalScale, 1.0);
        n = mat3(t, b, ng) * normalize(n);
    #else
        n = ng;
    #endif
    // Double-sided glTF materials shade the reverse side with reversed normals.
    // Face culling already removes backfaces for single-sided materials.
    #if defined(HAS_TANGENTS) || defined(HAS_NORMALS)
    if (!gl_FrontFacing) {
        n = -n;
        ng = -ng;
        t = -t;
        b = -b;
    }
    #endif
    n = normalize(n);
    NormalInfo info;
    info.ng = ng;
    info.t = t;
    info.b = b;
    info.n = n;
    return info;
}
#endif
