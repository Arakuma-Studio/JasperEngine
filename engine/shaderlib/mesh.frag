#version 450

// A fork may declare a u_fs_customParams block: its members are filled by name from the
// component's shader uniforms (Properties window, setShaderUniform). Two names are
// written by the engine instead: "time" (seconds since startup) and "resolution"
// (xy = render target size, zw = 1 / size). Do not mix int and float members: GL
// uploads the block typed after its first member.
//
// The fork drives the color pass only: to keep shadows (and SSAO, when SSR is off) in
// step with a discard here, fork the depth shader too (Depth Shader in Properties).
//
//   uniform u_fs_customParams {
//       float time;
//       vec4 tint;
//   } customParams;

#if !defined(MATERIAL_UNLIT) || defined(USE_MIRROR) || defined(USE_LIGHT2D)
    in vec3 v_position;
#endif

// declared only when actually sampled (USE_MIRROR + a base texture to sample);
// otherwise the GL driver strips the unused block and its bind slot mismatches
#if defined(USE_MIRROR) && defined(HAS_UV_SET1)
    uniform u_fs_mirror {
        mat4 mirrorVP; // logical view-projection of the reflection camera
    } mirrorParams;
#endif

#ifdef HAS_NORMALS
#ifdef HAS_TANGENTS
    in mat3 v_tbn;
#else
    in vec3 v_normal;
#endif
#endif

#ifdef HAS_UV_SET1
    #ifdef HAS_TEXTURERECT
        centroid in vec2 v_uv1;
    #else
        in vec2 v_uv1;
    #endif
#endif

#ifdef HAS_UV_SET2
    #ifdef HAS_TEXTURERECT
        centroid in vec2 v_uv2;
    #else
        in vec2 v_uv2;
    #endif
#endif

#if defined(HAS_VERTEX_COLOR_VEC3) && !defined(HAS_INSTANCING)
    in vec3 v_color;
#endif
#if defined(HAS_VERTEX_COLOR_VEC4) || defined(HAS_INSTANCING)
    in vec4 v_color;
#endif

out vec4 g_finalColor;

#if defined(HAS_UV_SET1) || defined(HAS_UV_SET2)
    uniform texture2D u_baseColorTexture;
    uniform sampler u_baseColor_smp;
    #ifndef MATERIAL_UNLIT
        uniform texture2D u_metallicRoughnessTexture;
        uniform texture2D u_occlusionTexture;
        uniform sampler u_metallicRoughness_smp;
        uniform sampler u_occlusion_smp;
        // a painted terrain with the post-processing colour leaves its base material's emissive
        // texture out: its slot is the colour LUT's (the variant is at 16 textures without it;
        // the emissive factor still applies)
        #if defined(HAS_TERRAIN_PBR) && defined(USE_POST_COLOR)
            #define NO_EMISSIVE_TEXTURE
        #else
            uniform texture2D u_emissiveTexture;
            uniform sampler u_emissive_smp;
        #endif
    #endif
    // the normal map is sampled by the lit path and by the 2D light path (which
    // otherwise keeps the unlit fast path)
    #if defined(HAS_NORMAL_MAP) && (!defined(MATERIAL_UNLIT) || defined(USE_LIGHT2D))
        uniform texture2D u_normalTexture;
        uniform sampler u_normal_smp;
    #endif
#endif

// the built-in Lit's extended lobes (includes/pbr_ext_slots.glsl): their values end this block
#if defined(PBR_CLEARCOAT) || defined(PBR_SHEEN) || defined(PBR_ANISOTROPY) || defined(PBR_IRIDESCENCE) || \
    defined(PBR_TRANSMISSION) || defined(PBR_SPECULAR) || defined(PBR_HEX_TILING) || defined(PBR_HAIR)
    #define PBR_EXT
#endif
uniform u_fs_pbrParams {
    //Metallic material
    vec4 baseColorFactor;
    #ifndef MATERIAL_UNLIT
        float metallicFactor;
        float roughnessFactor;
    #elif defined(ALPHA_MASK)
        // Preserve the CPU Material prefix offsets used by the lit block.
        float alphaMaskMetallicPadding;
        float alphaMaskRoughnessPadding;
    #endif
    #ifdef ALPHA_MASK
        float alphaCutoff;
    #endif
    #ifndef MATERIAL_UNLIT
        vec3 emissiveFactor;
        float foliageTransmission;
    #endif
    #ifdef USE_STOCHASTIC_TILING
        #ifdef MATERIAL_UNLIT
            // up to the offset of the values in the CPU Material prefix (48 bytes)
            #ifndef ALPHA_MASK
                vec4 stochasticPadding0;
            #endif
            vec4 stochasticPadding1;
        #endif
        // Stochastic Tiling: cell size, blend sharpness, contrast, rotation (includes/pbr.glsl)
        vec4 stochastic;
    #endif
    #ifdef PBR_EXT
        // std140 vec4s (MaterialPbr::getUniformBlock), after the material's (and Stochastic
        // Tiling's vec4: the upload reads the lobes' offset, RenderSystem pbrExtOffset)
        // (the w of clearcoat, anisotropy and transmission: the Specular & IOR lobe's dielectric
        // f0 without a map; of attenuation, the IOR's term of it: MaterialPbr::getUniformBlock)
        vec4 clearcoat;     // factor, roughness, normal scale, f0.r
        vec4 sheen;         // rgb colour (linear), roughness
        vec4 anisotropy;    // strength, cos(rotation), sin(rotation), f0.g
        vec4 iridescence;   // factor, ior, thickness min (nm), thickness max (nm)
        vec4 transmission;  // factor, volume thickness, attenuation distance (0: none), f0.b
        vec4 attenuation;   // rgb attenuation colour (linear), ((ior - 1) / (ior + 1))^2 (1 under 1)
        vec4 specular;      // rgb colour (linear), factor
        vec4 iorHex;        // ior, hex rotation, hex contrast, the sheen visibility's L(0.5)
        #ifdef HAS_UV_SET2
            // the UV set of each texture (includes/pbr.glsl u_fs_texCoordSets), as floats
            vec4 set0;
            vec4 set1;
        #endif
        #ifdef PBR_HAIR
            // the Hair shading model (includes/hair.glsl, MaterialPbr::getHairUniformBlock), last
            // (the upload writes them at their reflected offset, RenderSystem pbrHairOffset)
            vec4 hair0;     // rgb primary highlight colour (linear), w its shift
            vec4 hair1;     // rgb secondary highlight colour (linear), w its shift
            vec4 hair2;     // x primary exponent, y secondary exponent, z cos(rotation), w sin(rotation)
        #endif
    #endif
} pbrParams;

#ifdef USE_STOCHASTIC_TILING
    #define LIT_STOCHASTIC_PARAMS pbrParams.stochastic
#endif

// Lit reads the camera (eye, ambient, environment, viewport, fog) from its own camera block
// (includes/lit_frame.glsl, the part of the engine frame block it needs), and its lights
// from the object's list (includes/lights.glsl): the pixel lights here (USE_PUNCTUAL), the
// vertex lights through v_vertexLight (USE_VERTEX_LIGHTS). A fork written before them may
// declare the legacy u_fs_lighting block instead: the engine still fills it, with the
// object's first MAX_LIGHTS pixel lights.
#ifndef MATERIAL_UNLIT
    #include "includes/lit_frame.glsl"
#endif

#ifdef USE_PUNCTUAL
    #define LIGHTS_FS
    #include "includes/lights.glsl"
#endif

#ifdef USE_VERTEX_LIGHTS
    // MIXED (with USE_PUNCTUAL): the vertex lights' diffuse; VERTEX: plus the SH ambient
    in vec3 v_vertexLight;
#endif

#ifdef USE_SSAO
    uniform texture2D u_ssaoTexture;
    uniform sampler u_ssao_smp;
#endif

#ifdef USE_IBL
    uniform textureCube u_lambertianEnvTexture;
    uniform textureCube u_GGXEnvTexture;
    uniform textureCube u_reflectionProbeTexture;
    uniform sampler u_lambertianEnv_smp;
    uniform sampler u_GGXEnv_smp;
    uniform sampler u_reflectionProbe_smp;

    uniform u_fs_reflectionProbe {
        vec4 position_weight;
        vec4 boxMin_intensity;
        vec4 boxMax_lod;
    } reflectionProbe;
#endif

struct MaterialInfo{
    float perceptualRoughness;      // roughness value, as authored by the model creator (input to shader)
    vec3 f0;                        // full reflectance color (n incidence angle)

    float alphaRoughness;           // roughness mapped to a more linear change in the roughness (proposed by [2])
    vec3 albedoColor;

    vec3 f90;                       // reflectance color at grazing angle
    float metallic;

    vec3 n;
    vec3 baseColor; // getBaseColor()

};

struct NormalInfo {
    vec3 ng;   // Geometric normal
    vec3 n;    // Pertubed normal
    vec3 t;    // Pertubed tangent
    vec3 b;    // Pertubed bitangent
};

const float normalScale = 1.0;
const float occlusionStrength = 1.0;

const float M_PI = 3.141592653589793;

// the extended lobes and hex tiling (builtin:lit#PBR_*): their block, maps and hex taps
#include "includes/pbr_ext_slots.glsl"
#include "includes/pbr.glsl"
#include "includes/brdf.glsl"
#ifndef MATERIAL_UNLIT
    #include "includes/ibl.glsl"
    // the High Lit Quality (builtin:lit#USE_LIT_QUALITY): specular anti-aliasing and the
    // direct lights' multiscatter energy compensation
    #ifdef USE_LIT_QUALITY
        #include "includes/specular_aa.glsl"
        vec3 litEnergyCompensation;
    #endif
    #include "includes/pbr_ext.glsl"
#endif
#ifdef USE_LIGHT2D
    // the lit path shades 2D lights with its real normals (N.L at the light's height); the
    // unlit one answers the height only where it samples a normal map
    #ifndef MATERIAL_UNLIT
        #define LIGHT2D_SURFACE_NORMALS
    #endif
    // declares the 2D lights block and shadow atlas (shared with library shaders)
    #include "includes/lighting2d.glsl"
#endif
#ifdef HAS_TERRAIN
    // lets the include adapt the MaterialInfo declared above
    #define TERRAIN_MATERIAL_INFO
    #include "includes/terrain_fs.glsl"
#endif
#ifdef HAS_FOG
    #include "includes/fog.glsl"
#endif
// the post-processing colour (USE_POST_COLOR; after lit_frame.glsl, which holds its parameters)
#include "includes/post_color.glsl"
// the LOD cross-fade's dither (USE_LOD_FADE): LOD_FADE_CLIP() before every colour write (no semicolon)
#include "includes/lod_fade.glsl"

void main() {
    #ifdef PBR_HEX_ACTIVE
        pbrHexSetup();
    #endif
    vec4 baseColor = getBaseColor();

    #if defined(USE_MIRROR) && defined(HAS_UV_SET1)
        // planar reflection: sample the reflection texture by this fragment's
        // screen position (projected through the reflection camera), not mesh UVs.
        // Y is flipped for the top-left framebuffer origin (all backends).
        vec4 mirrorClip = mirrorParams.mirrorVP * vec4(v_position, 1.0);
        vec2 mirrorUV = vec2(mirrorClip.x / mirrorClip.w * 0.5 + 0.5,
                             0.5 - mirrorClip.y / mirrorClip.w * 0.5);
        baseColor = pbrParams.baseColorFactor * sRGBToLinear(texture(sampler2D(u_baseColorTexture, u_baseColor_smp), mirrorUV)) * getVertexColor();
    #endif

    #if defined(HAS_TERRAIN) && !defined(HAS_TERRAIN_PBR)
        baseColor = getTerrainColor(baseColor);
    #elif defined(HAS_TERRAIN_PBR) && defined(MATERIAL_UNLIT)
        // unlit shows the one thing a painted layer still means here
        baseColor = getTerrainSurface(baseColor, vec3(0.0, 1.0, 0.0), 1.0, 0.0).color;
    #endif

    #ifdef ALPHA_MASK
        if (baseColor.a < pbrParams.alphaCutoff) {
            discard;
        }
    #endif
    #if defined(ALPHA_MASK) || defined(ALPHA_OPAQUE)
        // glTF MASK and OPAQUE both render surviving fragments fully opaque.
        baseColor.a = 1.0;
    #endif

    #ifdef MATERIAL_UNLIT
        #ifdef USE_LIGHT2D
            baseColor.rgb *= getLight2DIntensity(v_position, getNormal2D(), true);
        #endif
        #ifdef HAS_FOG
            baseColor.rgb = getFogColor(baseColor.rgb);
        #endif
        LOD_FADE_CLIP()
        #ifdef USE_POST_COLOR
            g_finalColor = postColorEncodeBlended(baseColor);
        #else
            g_finalColor = (vec4(linearTosRGB(baseColor.rgb), baseColor.a));
        #endif
        return;
    #endif

    #ifndef MATERIAL_UNLIT
        NormalInfo normalInfo = getNormalInfo();
        vec3 n = normalInfo.n;

        MaterialInfo materialInfo = {0.0, vec3(0.0), 0.0, vec3(0.0), vec3(0.0), 0.0, vec3(0.0), vec3(0.0)};
        materialInfo.baseColor = baseColor.rgb;

        // The default index of refraction of 1.5 yields a dielectric normal incidence reflectance of 0.04.
        float f0_ior = 0.04;

        materialInfo = getMetallicRoughnessInfo(materialInfo, f0_ior);
        #ifdef PBR_EXT_LOBES
            materialInfo = pbrExtSurface(materialInfo, normalInfo);
        #endif

        #ifdef HAS_TERRAIN_PBR
            // painted layers own the whole surface, so it is evaluated once and applied here
            TerrainSurface terrainSurface = getTerrainSurface(baseColor, n, materialInfo.perceptualRoughness, materialInfo.metallic);
            baseColor = terrainSurface.color;
            materialInfo.baseColor = baseColor.rgb;
            materialInfo = applyTerrainSurface(materialInfo, terrainSurface, f0_ior);
            n = terrainSurface.normal;
        #endif

        materialInfo.perceptualRoughness = clamp(materialInfo.perceptualRoughness, 0.0, 1.0);
        materialInfo.metallic = clamp(materialInfo.metallic, 0.0, 1.0);

        // Roughness is authored as perceptual roughness; as is convention,
        // convert to material roughness by squaring the perceptual roughness.
        materialInfo.alphaRoughness = materialInfo.perceptualRoughness * materialInfo.perceptualRoughness;

        // Compute reflectance.
        float reflectance = max(max(materialInfo.f0.r, materialInfo.f0.g), materialInfo.f0.b);

        // Anything less than 2% is physically impossible and is instead considered to be shadowing. Compare to "Real-Time-Rendering" 4th editon on page 325.
        materialInfo.f90 = vec3(clamp(reflectance * 50.0, 0.0, 1.0));
        #ifdef PBR_SPECULAR
            materialInfo.f90 = pbrExtF90(materialInfo);
        #endif

        materialInfo.n = n;

        // LIGHTING
        vec3 f_specular = vec3(0.0);
        vec3 f_diffuse = vec3(0.0);
        vec3 f_emissive = vec3(0.0);

        vec3 toEye = litFrame.cameraPosition_fogEnd.xyz - v_position;
        vec3 v = normalize(toEye);

        #ifdef USE_LIT_QUALITY
            // geometric specular AA, once, in uniform control flow (derivatives): the base by
            // its shading normal's variance, the coat by its own normal's
            materialInfo.perceptualRoughness = specularAARoughness(materialInfo.perceptualRoughness, specularAAKernel(n));
            materialInfo.alphaRoughness = materialInfo.perceptualRoughness * materialInfo.perceptualRoughness;
            #ifdef PBR_CLEARCOAT
                pbrExtInfo.clearcoatRoughness = specularAARoughness(pbrExtInfo.clearcoatRoughness, specularAAKernel(pbrExtInfo.clearcoatNormal));
            #endif
            // the energy single-scattering GGX loses at this roughness, given back to the
            // direct lights (the environment's split sum has it already)
            vec2 litEss = envBRDFApprox(clampedDot(n, v), materialInfo.perceptualRoughness);
            litEnergyCompensation = 1.0 + materialInfo.f0 * (1.0 / max(litEss.x + litEss.y, 1e-3) - 1.0);
        #endif
        #ifdef PBR_EXT_LOBES
            pbrExtView(materialInfo, v);
        #endif

        #if defined(USE_IBL) && defined(PBR_EXT_LOBES)
            pbrExtEnvironment(materialInfo, n, v, vec3(0.0), f_diffuse, f_specular);
            #ifdef USE_PUNCTUAL
                vec3 overflowLight = max(ambientLight(n) - litFrame.ambient_fogType.rgb, vec3(0.0));
                float overflowNdotV = clampedDot(n, v);
                f_specular += envRadianceGGX(overflowLight, overflowNdotV, materialInfo.perceptualRoughness, materialInfo.f0);
                f_diffuse += envRadianceLambertian(overflowLight, overflowNdotV, materialInfo.perceptualRoughness, materialInfo.albedoColor, materialInfo.f0);
            #endif
        #elif defined(USE_IBL)
            // image based lighting from the environment (sky) cubemaps
            f_specular += getIBLRadianceGGX(n, v, v_position, materialInfo.perceptualRoughness, materialInfo.f0);
            f_diffuse += getIBLRadianceLambertian(n, v, materialInfo.perceptualRoughness, materialInfo.albedoColor, materialInfo.f0);
            #ifdef USE_PUNCTUAL
                // the lights over the object's cap, folded into its SH ambient (zero without any;
                // already divided by PI, see includes/lights.glsl)
                vec3 overflowLight = max(ambientLight(n) - litFrame.ambient_fogType.rgb, vec3(0.0));
                float overflowNdotV = clampedDot(n, v);
                f_specular += envRadianceGGX(overflowLight, overflowNdotV, materialInfo.perceptualRoughness, materialInfo.f0);
                f_diffuse += envRadianceLambertian(overflowLight, overflowNdotV, materialInfo.perceptualRoughness, materialInfo.albedoColor, materialInfo.f0);
            #endif
        #else
            // environment light from the scene ambient (global illumination); with pixel
            // lights, plus the lights over the object's cap from its SH ambient, which is the
            // scene ambient alone when none is over the cap
            #ifdef USE_PUNCTUAL
                vec3 envLight = litFrame.ambient_fogType.rgb + max(ambientLight(n) - litFrame.ambient_fogType.rgb, vec3(0.0));
            #else
                vec3 envLight = litFrame.ambient_fogType.rgb;
            #endif
            #ifdef PBR_EXT_LOBES
                pbrExtEnvironment(materialInfo, n, v, envLight, f_diffuse, f_specular);
            #else
                float ambNdotV = clampedDot(n, v);
                f_specular += envRadianceGGX(envLight, ambNdotV, materialInfo.perceptualRoughness, materialInfo.f0);
                f_diffuse += envRadianceLambertian(envLight, ambNdotV, materialInfo.perceptualRoughness, materialInfo.albedoColor, materialInfo.f0);
            #endif
        #endif

        #if defined(HAS_UV_SET1) || defined(HAS_UV_SET2)
            float ao = getOcclusionTexture().r;
            f_diffuse = mix(f_diffuse, f_diffuse * ao, occlusionStrength);
            // apply ambient occlusion too all lighting that is not punctual
            f_specular = mix(f_specular, f_specular * ao, occlusionStrength);
        #endif

        #ifdef HAS_TERRAIN_PBR
            f_diffuse *= terrainSurface.occlusion;
            f_specular *= terrainSurface.occlusion;
        #endif

        #ifdef USE_SSAO
            // Screen-space AO modulates only the ambient/indirect term (computed
            // above); the direct punctual lighting added below is unaffected.
            // The AO buffer is in depth orientation, so flip Y when that differs
            // from the color target (SSAO flags 2 set).
            vec2 ssaoUV = gl_FragCoord.xy * litFrame.viewport_ssao_envRotation.xy;
            float ssaoFlags = litFrame.viewport_ssao_envRotation.z;
            if (ssaoFlags > 1.5) ssaoUV.y = 1.0 - ssaoUV.y;
            float screenAO = texture(sampler2D(u_ssaoTexture, u_ssao_smp), ssaoUV).r;
            // debug: visualize the raw AO buffer (SSAO flags 1 set)
            if (fract(ssaoFlags * 0.5) > 0.25){
                LOD_FADE_CLIP()
                g_finalColor = vec4(vec3(screenAO), 1.0);
                return;
            }
            f_diffuse *= screenAO;
            f_specular *= screenAO;
        #endif

        // Apply light sources: the object's pixel lights
        #ifdef USE_PUNCTUAL
            for (int i = 0; i < LIGHT_CAPACITY; ++i){
                if (i >= pixelLightCount()) break;

                Light light = getLightData(i);

                if (light.intensity > 0.0){

                    vec3 pointToLight = getLightVector(light, v_position);

                    vec3 l = normalize(pointToLight);   // Direction from surface point to light
                    vec3 h = normalize(l + v);          // Direction of the vector between l and v, called halfway vector
                    float NdotL = clampedDot(n, l);
                    float NdotV = clampedDot(n, v);
                    float NdotH = clampedDot(n, h);
                    float VdotH = clampedDot(v, h);

                    float shadow = 1.0;
                    #ifdef USE_SHADOWS
                        // a surface facing away from the light (n is the shading normal, flipped
                        // on a double-sided back face) only needs it for foliage transmission:
                        // its direct terms below are scaled by NdotL = 0
                        if (NdotL > 0.0 || pbrParams.foliageTransmission > 0.0){
                            shadow = lightShadow(i, v_position, n);
                        }
                    #endif

                    if (pbrParams.foliageTransmission > 0.0) {
                        // Thin-leaf approximation. Use the same shadow visibility as
                        // direct lighting, so closed canopy does not glow in the dark.
                        float backlight = max(dot(-n, l), 0.0);
                        float forwardScatter = 0.35 + 0.65 * pow(max(dot(-l, v), 0.0), 3.0);
                        f_diffuse += shadow * getLighIntensity(light, pointToLight, i)
                            * materialInfo.albedoColor * clamp(pbrParams.foliageTransmission, 0.0, 1.0)
                            * backlight * forwardScatter;
                    }
                    if (NdotL > 0.0 || NdotV > 0.0){
                        // Calculation of analytical light
                        // https://github.com/KhronosGroup/glTF/tree/master/specification/2.0#acknowledgments AppendixB
                        vec3 intensity = getLighIntensity(light, pointToLight, i);
                        #ifdef PBR_EXT_LOBES
                            pbrExtDirect(materialInfo, shadow * intensity, pointToLight, n, v, l, h, NdotL, NdotV, NdotH, VdotH, f_diffuse, f_specular);
                        #elif defined(USE_LIT_QUALITY)
                            f_diffuse += shadow * intensity * NdotL *  BRDF_lambertian(materialInfo.f0, materialInfo.f90, materialInfo.albedoColor, VdotH);
                            f_specular += shadow * intensity * NdotL * BRDF_specularGGX(materialInfo.f0, materialInfo.f90, materialInfo.alphaRoughness, VdotH, NdotL, NdotV, NdotH) * litEnergyCompensation;
                        #else
                        f_diffuse += shadow * intensity * NdotL *  BRDF_lambertian(materialInfo.f0, materialInfo.f90, materialInfo.albedoColor, VdotH);
                        f_specular += shadow * intensity * NdotL * BRDF_specularGGX(materialInfo.f0, materialInfo.f90, materialInfo.alphaRoughness, VdotH, NdotL, NdotV, NdotH);
                        #endif
                    }
                }
            }
        #endif

        #ifdef USE_VERTEX_LIGHTS
            // the vertex lights: Lambert diffuse, as the pixel lights' (BRDF_lambertian); the
            // 1/PI is inside vertexDiffuse (includes/lights.glsl)
            #ifdef USE_PUNCTUAL
                vec3 vertexLight = v_vertexLight;
            #else
                // VERTEX: v_vertexLight carries the SH ambient too (no lights block here),
                // the scene ambient part of which is applied above
                vec3 vertexLight = max(v_vertexLight - litFrame.ambient_fogType.rgb, vec3(0.0));
            #endif
            f_diffuse += vertexLight * materialInfo.albedoColor;
        #endif

        f_emissive = pbrParams.emissiveFactor;
        #if (defined(HAS_UV_SET1) || defined(HAS_UV_SET2)) && !defined(NO_EMISSIVE_TEXTURE)
            f_emissive *= sRGBToLinear(getEmissiveTexture().rgb);
        #endif

        #ifdef PBR_TRANSMISSION
            f_diffuse = pbrExtTransmission(materialInfo, n, v, f_diffuse);
        #endif

        vec3 color = f_emissive + f_diffuse + f_specular;
        #ifdef PBR_EXT_LOBES
            color = pbrExtComposite(color);
        #endif

        #ifdef USE_LIGHT2D
            // mixed scene: 2D lights add on top of the PBR result (scene ambient is
            // already applied above, so skip the 2D ambient here)
            color.rgb += baseColor.rgb * getLight2DIntensity(v_position, n, false);
        #endif

        #if defined(USE_SHADOWS) && defined(USE_PUNCTUAL)
            // the Scene view's shadow debug views replace the shading (includes/lights.glsl)
            vec4 shadowDebug = shadowDebugColor(v_position, n);
            if (shadowDebug.a > 0.5){
                LOD_FADE_CLIP()
                g_finalColor = vec4(linearTosRGB(shadowDebug.rgb), 1.0);
                return;
            }
        #endif

        #ifdef HAS_FOG
            color.rgb = getFogColor(color.rgb);
        #endif

        LOD_FADE_CLIP()
        #ifdef USE_POST_COLOR
            g_finalColor = postColorEncodeBlended(vec4(color.rgb, baseColor.a));
        #else
            g_finalColor = vec4(linearTosRGB(color.rgb), baseColor.a);
        #endif
    #endif
}
