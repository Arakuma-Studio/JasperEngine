// The extended PBR lobes of the built-in Lit: clear coat, sheen, anisotropy, iridescence,
// transmission and volume, specular and IOR (glTF KHR_materials_*), and the High Lit Quality's
// direct-light energy compensation. Included by mesh.frag after includes/ibl.glsl (and
// includes/pbr_ext_slots.glsl before includes/pbr.glsl); without a PBR_* define it declares
// nothing.
//
// The lobe formulas follow the KHR specifications and their reference renderer (Khronos
// glTF-Sample-Renderer, Apache-2.0, as pbr.glsl, brdf.glsl and ibl.glsl), with texture-free
// substitutes for its lookup tables: an analytic sheen albedo (OpenEngine, MIT) instead of the
// sheen E LUT, the GGX prefiltered environment at the sheen roughness instead of a Charlie
// cube, and the scene colour copy (includes/scene.glsl) with at most five taps instead of the
// mip-chained transmission framebuffer. The anisotropic GGX and the Charlie visibility are
// adapted from OpenEngine's standard_pbr.glsl (MIT, Copyright (c) 2026 Gamecrafters Guild
// Limited and contributors).
//
// mesh.frag calls, in order: pbrExtSurface (after the metallic-roughness inputs), pbrExtView
// (once the view vector is known), pbrExtDirect for each pixel light, pbrExtEnvironment (IBL or
// the scene ambient), pbrExtTransmission and pbrExtComposite on the final colour.

#ifndef PBR_EXT_GLSL
#define PBR_EXT_GLSL

#ifdef PBR_EXT
    #include "includes/normal_map.glsl"
#endif

#if defined(PBR_CLEARCOAT) || defined(PBR_SHEEN) || defined(PBR_ANISOTROPY) || defined(PBR_IRIDESCENCE) || \
    defined(PBR_TRANSMISSION) || defined(PBR_SPECULAR) || defined(PBR_HAIR)
    #define PBR_EXT_LOBES
#endif

#ifdef PBR_HAIR
    #include "includes/hair.glsl"
#endif

#ifdef PBR_IRIDESCENCE
    #include "includes/iridescence.glsl"
#endif

#ifdef PBR_TRANSMISSION
    // the opaque scene behind the surface (the engine draws a transmissive submesh after the
    // copy, in the transparent queue) and the camera's view-projection to refract into it
    #define SURFACE_SCENE_COLOR
    #define SCENE_VIEW_PROJECTION
    #include "includes/scene.glsl"
#endif

// ---------------------------------------------------------------- extension maps

// the UV set (0 or 1) of each slot, and the slot's map, hex sampled on UV set 0
#ifdef HAS_UV_SET2
    #define PBR_EXT_SET0 texCoordSets.set1.y
    #define PBR_EXT_SET1 texCoordSets.set1.z
    #define PBR_EXT_SET2 texCoordSets.set1.w
#else
    #define PBR_EXT_SET0 0
    #define PBR_EXT_SET1 0
    #define PBR_EXT_SET2 0
#endif
#ifdef HAS_UV_SET2
    #define PBR_EXT_UV(set) selectUV(set)
#else
    #define PBR_EXT_UV(set) v_uv1
#endif

#ifdef PBR_HEX_ACTIVE
    #define PBR_EXT_SAMPLE(tex, smp, set) PBR_HEX_TEXTURE(tex, smp, PBR_EXT_UV(set), (set) == 0)
    #define PBR_EXT_SAMPLE_NORMAL(tex, smp, set) PBR_HEX_NORMAL(tex, smp, PBR_EXT_UV(set), (set) == 0)
#else
    #define PBR_EXT_SAMPLE(tex, smp, set) texture(sampler2D(tex, smp), PBR_EXT_UV(set))
    #define PBR_EXT_SAMPLE_NORMAL(tex, smp, set) decodeNormalMap(texture(sampler2D(tex, smp), PBR_EXT_UV(set)))
#endif

#ifdef PBR_EXT_MAP0
    vec4 pbrExtMap0(){ return PBR_EXT_SAMPLE(u_pbrExtTexture0, u_pbrExtTexture0_smp, PBR_EXT_SET0); }
    vec3 pbrExtNormal0(){ return PBR_EXT_SAMPLE_NORMAL(u_pbrExtTexture0, u_pbrExtTexture0_smp, PBR_EXT_SET0); }
#endif
#ifdef PBR_EXT_MAP1
    vec4 pbrExtMap1(){ return PBR_EXT_SAMPLE(u_pbrExtTexture1, u_pbrExtTexture1_smp, PBR_EXT_SET1); }
    vec3 pbrExtNormal1(){ return PBR_EXT_SAMPLE_NORMAL(u_pbrExtTexture1, u_pbrExtTexture1_smp, PBR_EXT_SET1); }
#endif
#ifdef PBR_EXT_MAP2
    vec4 pbrExtMap2(){ return PBR_EXT_SAMPLE(u_pbrExtTexture2, u_pbrExtTexture2_smp, PBR_EXT_SET2); }
    vec3 pbrExtNormal2(){ return PBR_EXT_SAMPLE_NORMAL(u_pbrExtTexture2, u_pbrExtTexture2_smp, PBR_EXT_SET2); }
#endif

// each lobe's map, by its slot (includes/pbr_ext_slots.glsl)
#if defined(PBR_CLEARCOAT_MAP) && defined(PBR_EXT_MAP0)
    #define pbrClearcoatMap() pbrExtMap0()
#endif
#if defined(PBR_SPECULAR_MAP) && defined(PBR_EXT_MAP0)
    #if PBR_SLOT_SPECULAR == 0
        #define pbrSpecularMap() pbrExtMap0()
    #else
        #define pbrSpecularMap() pbrExtMap1()
    #endif
#endif
#if defined(PBR_TRANSMISSION_MAP) && defined(PBR_EXT_MAP0)
    #if PBR_SLOT_TRANSMISSION == 0
        #define pbrTransmissionMap() pbrExtMap0()
    #elif PBR_SLOT_TRANSMISSION == 1
        #define pbrTransmissionMap() pbrExtMap1()
    #else
        #define pbrTransmissionMap() pbrExtMap2()
    #endif
#endif
#if defined(PBR_SHEEN_MAP) && defined(PBR_EXT_MAP0)
    #if PBR_SLOT_SHEEN == 0
        #define pbrSheenMap() pbrExtMap0()
    #elif PBR_SLOT_SHEEN == 1
        #define pbrSheenMap() pbrExtMap1()
    #else
        #define pbrSheenMap() pbrExtMap2()
    #endif
#endif
#if defined(PBR_ANISOTROPY_MAP) && defined(PBR_EXT_MAP0)
    #if PBR_SLOT_ANISOTROPY == 0
        #define pbrAnisotropyMap() pbrExtMap0()
    #elif PBR_SLOT_ANISOTROPY == 1
        #define pbrAnisotropyMap() pbrExtMap1()
    #else
        #define pbrAnisotropyMap() pbrExtMap2()
    #endif
#endif
#if defined(PBR_IRIDESCENCE_MAP) && defined(PBR_EXT_MAP0)
    #if PBR_SLOT_IRIDESCENCE == 0
        #define pbrIridescenceMap() pbrExtMap0()
    #elif PBR_SLOT_IRIDESCENCE == 1
        #define pbrIridescenceMap() pbrExtMap1()
    #else
        #define pbrIridescenceMap() pbrExtMap2()
    #endif
#endif
#if defined(PBR_HAIR_MAP) && defined(PBR_EXT_MAP0)
    #if PBR_SLOT_HAIR == 0
        #define pbrHairMap() pbrExtMap0()
    #elif PBR_SLOT_HAIR == 1
        #define pbrHairMap() pbrExtMap1()
    #else
        #define pbrHairMap() pbrExtMap2()
    #endif
#endif
#if defined(PBR_CLEARCOAT_NORMAL_MAP) && defined(PBR_EXT_MAP0)
    #if PBR_SLOT_CLEARCOAT_NORMAL == 0
        #define pbrClearcoatNormalMap() pbrExtNormal0()
    #elif PBR_SLOT_CLEARCOAT_NORMAL == 1
        #define pbrClearcoatNormalMap() pbrExtNormal1()
    #else
        #define pbrClearcoatNormalMap() pbrExtNormal2()
    #endif
#endif

#ifdef PBR_EXT_LOBES

// ---------------------------------------------------------------- lobe BRDFs

// anisotropic GGX (Filament's D_GGX_Anisotropic) and its height-correlated Smith visibility
float pbrDGGXAnisotropic(float NdotH, float TdotH, float BdotH, float at, float ab){
    float a2 = at * ab;
    vec3 f = vec3(ab * TdotH, at * BdotH, a2 * NdotH);
    float w2 = a2 / dot(f, f);
    return a2 * w2 * w2 / M_PI;
}

float pbrVGGXAnisotropic(float NdotL, float NdotV, float BdotV, float TdotV, float TdotL, float BdotL, float at, float ab){
    float GGXV = NdotL * length(vec3(at * TdotV, ab * BdotV, NdotV));
    float GGXL = NdotV * length(vec3(at * TdotL, ab * BdotL, NdotL));
    return clamp(0.5 / max(GGXV + GGXL, 1e-5), 0.0, 1.0);
}

// the Charlie sheen distribution (Estevez and Kulla) and its visibility (KHR_materials_sheen)
float pbrDCharlie(float sheenRoughness, float NdotH){
    float alphaG = max(sheenRoughness * sheenRoughness, 0.000001);
    float invR = 1.0 / alphaG;
    float cos2h = NdotH * NdotH;
    float sin2h = max(1.0 - cos2h, 0.0078125); // 2^-7: no fp16 underflow
    return (2.0 + invR) * pow(sin2h, invR * 0.5) / (2.0 * M_PI);
}

float pbrCharlieL(float x, float alphaG){
    float oneMinusAlphaSq = (1.0 - alphaG) * (1.0 - alphaG);
    float a = mix(21.5473, 25.3245, oneMinusAlphaSq);
    float b = mix(3.82987, 3.32435, oneMinusAlphaSq);
    float c = mix(0.19823, 0.16801, oneMinusAlphaSq);
    float d = mix(-1.97760, -1.27393, oneMinusAlphaSq);
    float e = mix(-4.32054, -4.85967, oneMinusAlphaSq);
    return a / (1.0 + b * pow(x, c)) + d * x + e;
}

// The reference's abs(x) < 0.5 ? exp(L(x)) : exp(2 L(0.5) - L(1 - x)) with L taken at one
// argument (x, or 1 - x from 0.5 on) and L(0.5) given (l05: per draw without a sheen map): two
// L a light instead of five, and a select instead of a branch per lambda, each side of which
// held its own L, pow and exp (GPU campaign perf3)
float pbrCharlieLambda(float cosTheta, float alphaG, float l05){
    bool near = abs(cosTheta) < 0.5;
    float l = pbrCharlieL(near ? cosTheta : 1.0 - cosTheta, alphaG);
    return exp(near ? l : 2.0 * l05 - l);
}

// l05: pbrCharlieL(0.5) at the roughness's alphaG (PbrExtInfo::sheenL05)
float pbrVSheen(float NdotL, float NdotV, float sheenRoughness, float l05){
    float alphaG = max(sheenRoughness * sheenRoughness, 0.000001);
    return clamp(1.0 / ((1.0 + pbrCharlieLambda(NdotV, alphaG, l05) + pbrCharlieLambda(NdotL, alphaG, l05)) * (4.0 * NdotV * NdotL)), 0.0, 1.0);
}

// the share of a uniform environment the sheen lobe reflects toward the viewer (OpenEngine's
// analytic fit of the Charlie directional albedo, no LUT)
float pbrSheenAlbedo(float NdotV, float sheenRoughness){
    float roughness = max(sheenRoughness, 0.07);
    return 1.02 * exp(-(2.15 + 0.31 / roughness) * NdotV);
}

float pbrMax3(vec3 v){
    return max(max(v.x, v.y), v.z);
}

// a rougher look through a denser medium (KHR_materials_transmission)
float pbrApplyIorToRoughness(float roughness, float ior){
    return roughness * clamp(ior * 2.0 - 2.0, 0.0, 1.0);
}

// ---------------------------------------------------------------- per-fragment inputs

struct PbrExtInfo{
    float specularWeight;        // KHR_materials_specular's factor (dielectric share)
    float ior;
    vec3 f0Dielectric;

    float clearcoatFactor;
    float clearcoatRoughness;
    vec3 clearcoatNormal;
    float clearcoatFresnel;      // at the view, set by pbrExtView

    vec3 sheenColor;
    float sheenRoughness;
    float sheenL05;              // pbrCharlieL(0.5) of the roughness: per draw without a map
    float sheenAlbedo;           // pbrSheenAlbedo at the view, set by pbrExtView
    float sheenScaling;          // 1 - max(sheenColor) * albedo, set by pbrExtView

    float anisotropyStrength;
    vec3 anisotropicT;
    vec3 anisotropicB;

    float iridescenceFactor;
    float iridescenceIor;
    float iridescenceThickness;
    vec3 iridescenceFresnel;     // at the view, set by pbrExtView

    float transmissionFactor;
    float thickness;
    vec3 attenuationColor;
    float attenuationDistance;   // 0: no absorption

    #ifdef PBR_HAIR
        vec3 hairT;              // the strand's direction (unit, along the surface)
    #endif
};

PbrExtInfo pbrExtInfo;
// the lobes' own light, added up over the lights and the environment
vec3 pbrExtSheenLight = vec3(0.0);
vec3 pbrExtClearcoatLight = vec3(0.0);
vec3 pbrExtTransmittedLight = vec3(0.0);

// The lobes' inputs (factors times maps) and what they change of the base: the dielectric
// reflectance (specular and IOR). After getMetallicRoughnessInfo.
MaterialInfo pbrExtSurface(MaterialInfo info, NormalInfo normalInfo){
    pbrExtInfo.ior = pbrExt.iorHex.x;
    pbrExtInfo.specularWeight = 1.0;
    pbrExtInfo.f0Dielectric = vec3(0.04);

    #ifdef PBR_SPECULAR
        // f0 = min(((ior - 1) / (ior + 1))^2 * specularColor, 1) * specular, f90 = specular; the
        // IOR's term and, without a map, f0 come made once a draw (MaterialPbr::getUniformBlock)
        float specularFactor = pbrExt.specular.w;
        pbrExtInfo.specularWeight = specularFactor;
        #ifdef pbrSpecularMap
            vec3 specularColor = pbrExt.specular.rgb;
            vec4 specularSample = pbrSpecularMap();
            specularFactor *= specularSample.a;
            specularColor *= sRGBToLinear(specularSample.rgb);
            pbrExtInfo.specularWeight = specularFactor;
            pbrExtInfo.f0Dielectric = min(pbrExt.attenuation.w * specularColor, vec3(1.0)) * specularFactor;
        #else
            pbrExtInfo.f0Dielectric = vec3(pbrExt.clearcoat.w, pbrExt.anisotropy.w, pbrExt.transmission.w);
        #endif
        info.albedoColor = mix(info.baseColor.rgb * (vec3(1.0) - pbrExtInfo.f0Dielectric), vec3(0.0), info.metallic);
        info.f0 = mix(pbrExtInfo.f0Dielectric, info.baseColor.rgb, info.metallic);
    #endif

    #ifdef PBR_CLEARCOAT
        pbrExtInfo.clearcoatFactor = pbrExt.clearcoat.x;
        pbrExtInfo.clearcoatRoughness = pbrExt.clearcoat.y;
        #ifdef pbrClearcoatMap
            vec4 clearcoatSample = pbrClearcoatMap();
            pbrExtInfo.clearcoatFactor *= clearcoatSample.r;
            pbrExtInfo.clearcoatRoughness *= clearcoatSample.g;
        #endif
        pbrExtInfo.clearcoatRoughness = clamp(pbrExtInfo.clearcoatRoughness, 0.0, 1.0);
        // the coat's own normal map, else the geometric normal (not the base's normal map)
        pbrExtInfo.clearcoatNormal = normalInfo.ng;
        #if defined(pbrClearcoatNormalMap) && (defined(HAS_TANGENTS) || defined(HAS_UV_SET1))
            vec3 coatNormal = pbrClearcoatNormalMap();
            coatNormal *= vec3(pbrExt.clearcoat.z, pbrExt.clearcoat.z, 1.0);
            pbrExtInfo.clearcoatNormal = normalize(mat3(normalInfo.t, normalInfo.b, normalInfo.ng) * normalize(coatNormal));
        #endif
    #endif

    #ifdef PBR_SHEEN
        pbrExtInfo.sheenColor = pbrExt.sheen.rgb;
        pbrExtInfo.sheenRoughness = pbrExt.sheen.w;
        #ifdef pbrSheenMap
            vec4 sheenSample = pbrSheenMap();
            pbrExtInfo.sheenColor *= sRGBToLinear(sheenSample.rgb);
            pbrExtInfo.sheenRoughness *= sheenSample.a;
        #endif
        pbrExtInfo.sheenRoughness = clamp(pbrExtInfo.sheenRoughness, 0.0, 1.0);
        // the visibility's L(0.5): a function of the roughness alone, made once a draw without a
        // map (iorHex.w, MaterialPbr::getUniformBlock)
        #ifdef pbrSheenMap
            pbrExtInfo.sheenL05 = pbrCharlieL(0.5, max(pbrExtInfo.sheenRoughness * pbrExtInfo.sheenRoughness, 0.000001));
        #else
            pbrExtInfo.sheenL05 = pbrExt.iorHex.w;
        #endif
    #endif

    #ifdef PBR_ANISOTROPY
        vec2 direction = vec2(1.0, 0.0);
        float strengthFactor = 1.0;
        #ifdef pbrAnisotropyMap
            vec3 anisotropySample = pbrAnisotropyMap().xyz;
            direction = anisotropySample.xy * 2.0 - vec2(1.0);
            strengthFactor = anisotropySample.z;
        #endif
        // the rotation (cos, sin) from the tangent toward the bitangent
        vec2 cs = pbrExt.anisotropy.yz;
        direction = vec2(cs.x * direction.x - cs.y * direction.y, cs.y * direction.x + cs.x * direction.y);
        // the tangent frame; a mesh without tangents or UVs takes any frame of its normal
        #if defined(HAS_TANGENTS) || defined(HAS_UV_SET1)
            vec3 t = normalInfo.t;
            vec3 b = normalInfo.b;
        #else
            vec3 helper = abs(normalInfo.n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            vec3 t = normalize(cross(helper, normalInfo.n));
            vec3 b = cross(normalInfo.n, t);
        #endif
        pbrExtInfo.anisotropicT = mat3(t, b, normalInfo.n) * normalize(vec3(direction, 0.0));
        pbrExtInfo.anisotropicB = cross(normalInfo.ng, pbrExtInfo.anisotropicT);
        pbrExtInfo.anisotropyStrength = clamp(pbrExt.anisotropy.x * strengthFactor, 0.0, 1.0);
    #endif

    #ifdef PBR_IRIDESCENCE
        pbrExtInfo.iridescenceFactor = pbrExt.iridescence.x;
        pbrExtInfo.iridescenceIor = pbrExt.iridescence.y;
        pbrExtInfo.iridescenceThickness = pbrExt.iridescence.w;
        #ifdef pbrIridescenceMap
            vec4 iridescenceSample = pbrIridescenceMap();
            pbrExtInfo.iridescenceFactor *= iridescenceSample.r;
            pbrExtInfo.iridescenceThickness = mix(pbrExt.iridescence.z, pbrExt.iridescence.w, iridescenceSample.g);
        #endif
        // no film, no bands
        if (pbrExtInfo.iridescenceThickness <= 0.0)
            pbrExtInfo.iridescenceFactor = 0.0;
    #endif

    #ifdef PBR_TRANSMISSION
        pbrExtInfo.transmissionFactor = pbrExt.transmission.x;
        pbrExtInfo.thickness = 0.0;
        pbrExtInfo.attenuationColor = vec3(1.0);
        pbrExtInfo.attenuationDistance = 0.0;
        #ifdef PBR_VOLUME
            pbrExtInfo.thickness = pbrExt.transmission.y;
            pbrExtInfo.attenuationColor = pbrExt.attenuation.rgb;
            pbrExtInfo.attenuationDistance = pbrExt.transmission.z;
        #endif
        #ifdef pbrTransmissionMap
            vec4 transmissionSample = pbrTransmissionMap();
            pbrExtInfo.transmissionFactor *= transmissionSample.r;
            pbrExtInfo.thickness *= transmissionSample.g;
        #endif
        pbrExtInfo.transmissionFactor = clamp(pbrExtInfo.transmissionFactor, 0.0, 1.0);
    #endif

    #ifdef PBR_HAIR
        // the strand: the tangent turned by the rotation (hair2.zw: its cosine and sine), or the
        // flow map's tangent-space direction (RG, [-1, 1] stored as [0, 1])
        vec2 strand = pbrExt.hair2.zw;
        #ifdef pbrHairMap
            vec2 flow = pbrHairMap().rg * 2.0 - 1.0;
            if (dot(flow, flow) > 1e-6)
                strand = normalize(vec2(flow.x * strand.x - flow.y * strand.y, flow.x * strand.y + flow.y * strand.x));
        #endif
        #if defined(HAS_TANGENTS) || defined(HAS_UV_SET1)
            vec3 hairTangent = normalInfo.t;
            vec3 hairBitangent = normalInfo.b;
        #else
            vec3 hairHelper = abs(normalInfo.n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            vec3 hairTangent = normalize(cross(hairHelper, normalInfo.n));
            vec3 hairBitangent = cross(normalInfo.n, hairTangent);
        #endif
        pbrExtInfo.hairT = normalize(hairTangent * strand.x + hairBitangent * strand.y);
    #endif

    return info;
}

#ifdef PBR_HAIR
    // one pixel light on the strands (includes/hair.glsl): hair0 rgb the primary highlight's
    // colour, w its shift; hair1 the secondary's (tinted by the albedo); hair2 xy their exponents
    void pbrExtHairDirect(MaterialInfo info, vec3 intensity, vec3 n, vec3 l, vec3 h, inout vec3 diffuse, inout vec3 specular){
        float wrapped = clamp(mix(0.25, 1.0, dot(n, l)), 0.0, 1.0);
        diffuse += intensity * wrapped * (info.albedoColor / M_PI);
        vec3 t1 = hairShiftTangent(pbrExtInfo.hairT, n, pbrExt.hair0.w);
        vec3 t2 = hairShiftTangent(pbrExtInfo.hairT, n, pbrExt.hair1.w);
        vec3 highlights = pbrExt.hair0.rgb * hairStrandSpecular(t1, h, pbrExt.hair2.x) +
                          pbrExt.hair1.rgb * info.baseColor * hairStrandSpecular(t2, h, pbrExt.hair2.y);
        specular += intensity * wrapped * highlights;
    }
#endif

// f90 of the base with KHR_materials_specular (after the base's own)
#ifdef PBR_SPECULAR
    vec3 pbrExtF90(MaterialInfo info){
        return vec3(mix(pbrExtInfo.specularWeight, 1.0, info.metallic));
    }
#endif

// What depends on the view: the coat's Fresnel, the sheen's albedo, the film's reflectance
void pbrExtView(MaterialInfo info, vec3 v){
    float NdotV = clampedDot(info.n, v);
    #ifdef PBR_CLEARCOAT
        pbrExtInfo.clearcoatFresnel = F_Schlick(vec3(0.04), vec3(1.0), clampedDot(pbrExtInfo.clearcoatNormal, v)).x;
    #endif
    #ifdef PBR_SHEEN
        // once: the environment's sheen takes it too (the same NdotV)
        pbrExtInfo.sheenAlbedo = pbrSheenAlbedo(NdotV, pbrExtInfo.sheenRoughness);
        pbrExtInfo.sheenScaling = 1.0 - pbrMax3(pbrExtInfo.sheenColor) * pbrExtInfo.sheenAlbedo;
    #endif
    #ifdef PBR_IRIDESCENCE
        pbrExtInfo.iridescenceFresnel = iridescenceFresnel(NdotV, pbrExtInfo.iridescenceIor, pbrExtInfo.iridescenceThickness, info.f0);
    #endif
}

// the base's Fresnel at a half vector, with the film's
vec3 pbrExtFresnel(MaterialInfo info, float VdotH){
    vec3 F = F_Schlick(info.f0, info.f90, VdotH);
    #ifdef PBR_IRIDESCENCE
        F = mix(F, pbrExtInfo.iridescenceFresnel, pbrExtInfo.iridescenceFactor);
    #endif
    return F;
}

#ifdef PBR_TRANSMISSION
    // the refracted path through the volume, world space (thin walled without a volume)
    vec3 pbrExtTransmissionRay(vec3 n, vec3 v){
        vec3 refractionVector = refract(-v, normalize(n), 1.0 / max(pbrExtInfo.ior, 1.0));
        return normalize(refractionVector) * pbrExtInfo.thickness;
    }

    // Beer-Lambert absorption over a distance
    vec3 pbrExtAttenuate(vec3 radiance, float distance){
        if (pbrExtInfo.attenuationDistance <= 0.0)
            return radiance;
        vec3 transmittance = pow(max(pbrExtInfo.attenuationColor, vec3(0.0001)), vec3(distance / pbrExtInfo.attenuationDistance));
        return transmittance * radiance;
    }

    // light from a punctual light behind the surface, through it (the KHR reference BTDF)
    vec3 pbrExtPunctualTransmission(MaterialInfo info, vec3 n, vec3 v, vec3 pointToLight){
        float roughness = pbrApplyIorToRoughness(info.alphaRoughness, pbrExtInfo.ior);
        vec3 l = normalize(pointToLight);
        vec3 mirrored = normalize(l + 2.0 * n * dot(-l, n));
        vec3 h = normalize(mirrored + v);
        float D = D_GGX(clampedDot(n, h), roughness);
        vec3 F = pbrExtFresnel(info, clampedDot(v, h));
        float Vis = V_GGX(clampedDot(n, mirrored), clampedDot(n, v), roughness);
        return (1.0 - F) * info.baseColor * D * Vis;
    }
#endif

// One pixel light (intensity: its radiance with the shadow), into the base's diffuse and
// specular sums and the lobes' own
void pbrExtDirect(MaterialInfo info, vec3 intensity, vec3 pointToLight, vec3 n, vec3 v, vec3 l, vec3 h,
                  float NdotL, float NdotV, float NdotH, float VdotH, inout vec3 diffuse, inout vec3 specular){
    #ifdef PBR_HAIR
        // the Hair shading model lights strands, not the base's microfacets (includes/hair.glsl)
        pbrExtHairDirect(info, intensity, n, l, h, diffuse, specular);
        return;
    #endif
    vec3 F = pbrExtFresnel(info, VdotH);
    diffuse += intensity * NdotL * (1.0 - pbrExtInfo.specularWeight * F) * (info.albedoColor / M_PI);

    #ifdef PBR_ANISOTROPY
        float at = mix(info.alphaRoughness, 1.0, pbrExtInfo.anisotropyStrength * pbrExtInfo.anisotropyStrength);
        float ab = clamp(info.alphaRoughness, 0.001, 1.0);
        vec3 t = pbrExtInfo.anisotropicT;
        vec3 b = pbrExtInfo.anisotropicB;
        float D = pbrDGGXAnisotropic(NdotH, dot(t, h), dot(b, h), at, ab);
        float Vis = pbrVGGXAnisotropic(NdotL, NdotV, dot(b, v), dot(t, v), dot(t, l), dot(b, l), at, ab);
    #else
        float D = D_GGX(NdotH, info.alphaRoughness);
        float Vis = V_GGX(NdotL, NdotV, info.alphaRoughness);
    #endif
    vec3 lightSpecular = intensity * NdotL * pbrExtInfo.specularWeight * F * Vis * D;
    #ifdef USE_LIT_QUALITY
        lightSpecular *= litEnergyCompensation;
    #endif
    specular += lightSpecular;

    #ifdef PBR_SHEEN
        pbrExtSheenLight += intensity * NdotL * pbrExtInfo.sheenColor * pbrDCharlie(pbrExtInfo.sheenRoughness, NdotH) *
                            pbrVSheen(NdotL, NdotV, pbrExtInfo.sheenRoughness, pbrExtInfo.sheenL05);
    #endif

    #ifdef PBR_CLEARCOAT
        vec3 cn = pbrExtInfo.clearcoatNormal;
        float coatNdotL = clampedDot(cn, l);
        float coatAlpha = pbrExtInfo.clearcoatRoughness * pbrExtInfo.clearcoatRoughness;
        pbrExtClearcoatLight += intensity * coatNdotL *
            BRDF_specularGGX(vec3(0.04), vec3(1.0), coatAlpha, VdotH, coatNdotL, clampedDot(cn, v), clampedDot(cn, h));
    #endif

    #ifdef PBR_TRANSMISSION
        vec3 transmissionRay = pbrExtTransmissionRay(n, v);
        vec3 throughVolume = pointToLight - transmissionRay;
        pbrExtTransmittedLight += pbrExtAttenuate(intensity * pbrExtPunctualTransmission(info, n, v, throughVolume), length(transmissionRay));
    #endif
}

// the environment's specular (radiance along the reflection, already filtered), with the
// lobes' Fresnel, specular weight and anisotropy, Fdez-Aguera's split sum as envRadianceGGX
vec3 pbrExtEnvSpecular(MaterialInfo info, vec3 radiance, float NdotV){
    vec2 f_ab = envBRDFApprox(NdotV, info.perceptualRoughness);
    vec3 Fr = max(vec3(1.0 - info.perceptualRoughness), info.f0) - info.f0;
    vec3 k_S = info.f0 + Fr * pow(1.0 - NdotV, 5.0);
    #ifdef PBR_IRIDESCENCE
        k_S = mix(k_S, pbrExtInfo.iridescenceFresnel, pbrExtInfo.iridescenceFactor);
    #endif
    return radiance * (pbrExtInfo.specularWeight * k_S * f_ab.x + f_ab.y);
}

// the environment's diffuse (irradiance), as envRadianceLambertian with the specular weight
vec3 pbrExtEnvDiffuse(MaterialInfo info, vec3 irradiance, float NdotV){
    vec2 f_ab = envBRDFApprox(NdotV, info.perceptualRoughness);
    vec3 Fr = max(vec3(1.0 - info.perceptualRoughness), info.f0) - info.f0;
    vec3 k_S = info.f0 + Fr * pow(1.0 - NdotV, 5.0);
    #ifdef PBR_IRIDESCENCE
        k_S = mix(k_S, pbrExtInfo.iridescenceFresnel, pbrExtInfo.iridescenceFactor);
    #endif
    vec3 FssEss = pbrExtInfo.specularWeight * k_S * f_ab.x + f_ab.y;
    float Ems = 1.0 - (f_ab.x + f_ab.y);
    vec3 F_avg = pbrExtInfo.specularWeight * (info.f0 + (1.0 - info.f0) / 21.0);
    vec3 FmsEms = Ems * FssEss * F_avg / (1.0 - F_avg * Ems);
    vec3 k_D = info.albedoColor * (1.0 - FssEss + FmsEms);
    return (FmsEms + k_D) * irradiance;
}

// the direction the environment is looked up along: the reflection, bent along the
// anisotropy's direction
vec3 pbrExtReflection(MaterialInfo info, vec3 n, vec3 v){
    #ifdef PBR_ANISOTROPY
        vec3 anisotropicTangent = cross(pbrExtInfo.anisotropicB, v);
        vec3 anisotropicNormal = cross(anisotropicTangent, pbrExtInfo.anisotropicB);
        float bendFactor = 1.0 - pbrExtInfo.anisotropyStrength * (1.0 - info.perceptualRoughness);
        float bendFactorPow4 = bendFactor * bendFactor * bendFactor * bendFactor;
        vec3 bentNormal = normalize(mix(anisotropicNormal, n, bendFactorPow4));
        return normalize(reflect(-v, bentNormal));
    #else
        return normalize(reflect(-v, n));
    #endif
}

// The environment (the IBL cubes with USE_IBL, else the uniform scene ambient envLight) into
// the base's sums and the lobes' own
void pbrExtEnvironment(MaterialInfo info, vec3 n, vec3 v, vec3 envLight, inout vec3 diffuse, inout vec3 specular){
    float NdotV = clampedDot(n, v);
    #ifdef USE_IBL
        vec3 radiance = getSpecularLight(pbrExtReflection(info, n, v), v_position, info.perceptualRoughness * ENV_GGX_LODS);
        vec3 irradiance = getDiffuseLight(n);
    #else
        vec3 radiance = envLight;
        vec3 irradiance = envLight;
    #endif
    specular += pbrExtEnvSpecular(info, radiance, NdotV);
    diffuse += pbrExtEnvDiffuse(info, irradiance, NdotV);

    #ifdef PBR_SHEEN
        #ifdef USE_IBL
            vec3 sheenRadiance = getSpecularLight(normalize(reflect(-v, n)), v_position, pbrExtInfo.sheenRoughness * ENV_GGX_LODS);
        #else
            vec3 sheenRadiance = envLight;
        #endif
        pbrExtSheenLight += sheenRadiance * pbrExtInfo.sheenColor * pbrExtInfo.sheenAlbedo;
    #endif

    #ifdef PBR_CLEARCOAT
        vec3 cn = pbrExtInfo.clearcoatNormal;
        float coatNdotV = clampedDot(cn, v);
        #ifdef USE_IBL
            vec3 coatRadiance = getSpecularLight(normalize(reflect(-v, cn)), v_position, pbrExtInfo.clearcoatRoughness * ENV_GGX_LODS);
        #else
            vec3 coatRadiance = envLight;
        #endif
        pbrExtClearcoatLight += envRadianceGGX(coatRadiance, coatNdotV, pbrExtInfo.clearcoatRoughness, vec3(0.04));
    #endif
}

#ifdef PBR_TRANSMISSION
    // the opaque scene through the surface (refracted through the volume, blurred by the
    // roughness), absorbed and tinted by the base: what the transmission factor shows instead
    // of the diffuse
    vec3 pbrExtSceneTransmission(MaterialInfo info, vec3 n, vec3 v){
        vec3 transmissionRay = pbrExtTransmissionRay(n, v);
        vec4 clip = sceneInputs.viewProjection * vec4(v_position + transmissionRay, 1.0);
        vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
        // the copy has no mips: at most five taps, spread by the roughness through the medium
        float spread = pbrApplyIorToRoughness(info.perceptualRoughness, pbrExtInfo.ior);
        vec2 radius = sceneInputs.colorParams.zw * (spread * spread * 48.0);
        vec3 light = sceneColor(uv) * 0.4;
        light += sceneColor(uv + vec2(radius.x, radius.y)) * 0.15;
        light += sceneColor(uv + vec2(-radius.x, radius.y)) * 0.15;
        light += sceneColor(uv + vec2(radius.x, -radius.y)) * 0.15;
        light += sceneColor(uv + vec2(-radius.x, -radius.y)) * 0.15;
        light = pbrExtAttenuate(light, length(transmissionRay));
        vec2 brdf = envBRDFApprox(clampedDot(n, v), info.perceptualRoughness);
        vec3 specularColor = info.f0 * brdf.x + info.f90 * brdf.y;
        return (1.0 - specularColor) * light * info.baseColor;
    }

    // the diffuse, with the transmitted share of it replaced
    vec3 pbrExtTransmission(MaterialInfo info, vec3 n, vec3 v, vec3 diffuse){
        vec3 transmitted = pbrExtTransmittedLight + pbrExtSceneTransmission(info, n, v);
        return mix(diffuse, transmitted, pbrExtInfo.transmissionFactor);
    }
#endif

// the layers over the base: sheen (the base scaled by what it does not reflect) and the coat
// (the base attenuated by its Fresnel)
vec3 pbrExtComposite(vec3 color){
    #ifdef PBR_SHEEN
        color = pbrExtSheenLight + color * pbrExtInfo.sheenScaling;
    #endif
    #ifdef PBR_CLEARCOAT
        color = color * (1.0 - pbrExtInfo.clearcoatFactor * pbrExtInfo.clearcoatFresnel) + pbrExtClearcoatLight * pbrExtInfo.clearcoatFactor;
    #endif
    return color;
}

#endif // PBR_EXT_LOBES

#endif
