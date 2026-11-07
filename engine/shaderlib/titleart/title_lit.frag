#version 450
precision highp float;
precision highp int;

// Title Lit (docs/specs/S86-title-3d.md R18-R23, §6.4): the 3D title shader that replaces the
// thirteen three.js chunks Pachislop patches (src/engine/three/materials.ts, ported by SafEight,
// used with the author's permission). Metallic-roughness with the engine's GGX lobes
// (includes/brdf.glsl) and split-sum IBL (includes/ibl.glsl) on the linear float title
// environment, the layer-space banded ramp with its bend (title_ramp.glsl), clearcoat,
// iridescence (includes/iridescence.glsl), glass depth, gems' environment refraction
// (title_gem.glsl), surface relief and flakes (title_surface.glsl), env lock, blur, repeat and
// the grade as uniforms (title_env.glsl). The output is premultiplied: opaque writes alpha 1;
// with opacity < 1, rgb = diffuse x a + added light and alpha = a + (1 - a) max(added light).
// TL_HDR_COMPRESS writes the compressed light of the RGBA8 path (hdr_compress.glsl); otherwise
// `frame.z` asks for the tone-map-aware shoulder on MSAA samples. Variants: the TL_* bits of
// TitleTierFeatures.h. A slot's material comes from the slot table, so one draw can cover the
// opaque slots of a variant (v_ids.w low 12 bits).

const float M_PI = 3.141592653589793;

#include "includes/brdf.glsl"
#include "includes/ibl.glsl"
#include "includes/iridescence.glsl"
#include "includes/specular_aa.glsl"
#include "titleart/hdr_compress.glsl"

#define TL_MAX_MATERIALS 16
#define TL_MATERIAL_VEC4S 10
#define TL_MAX_LIGHTS 4

in vec3 v_world;
in vec3 v_normal;
in vec3 v_layerPos;
in vec3 v_layerNormal;
in vec2 v_zRange;
flat in vec4 v_ids;
#ifdef TL_WRAP_UV
in vec2 v_wrap;
#endif

out vec4 frag_color;

#ifdef TL_MATCAP
uniform texture2D u_tlMatcap;
uniform sampler u_tlMatcap_smp;
#else
uniform textureCube u_tlEnv;
uniform sampler u_tlEnv_smp;
#endif
uniform texture2D u_tlRamps;
uniform sampler u_tlRamps_smp;
// @image_sample_type u_tlSpans unfilterable_float
// @sampler_type u_tlSpans_smp nonfiltering
uniform texture2D u_tlSpans;
uniform sampler u_tlSpans_smp;
#ifdef TL_BASE_TEXTURE
uniform texture2D u_tlBase;
uniform sampler u_tlBase_smp;
#endif

uniform u_fs_titleLit {
    vec4 camera;        // xyz: camera position (world), w: the tone map's exposure (shoulder)
    vec4 frame;         // x: env intensity, y: env rotation (rad), z: 1 = shoulder the output, w: size (layer units per %)
    vec4 env;           // x: log2(face / 512), y: the deepest level, z: blur floor, w: 1 when repeat applies
    vec4 grade;         // rgb: tint, a: saturation
    vec4 grade2;        // x: contrast, y: repeat x, z: repeat y, w: lights count
    vec4 lock0;         // env lock: the rows of world -> head-on (identity when off)
    vec4 lock1;
    vec4 lock2;
    vec4 view0;         // matcap: the rows of world -> view
    vec4 view1;
    vec4 ambient;       // rgb: ambient light
    vec4 axisX;         // xyz: the layer's +x in world space (surface relief, flakes)
    vec4 axisY;         // xyz: the layer's +y
    vec4 lights[TL_MAX_LIGHTS * 2];     // per light: (direction or position, type 0 dir / 1 point), (colour x intensity, 0)
    vec4 slots[16];     // the material index of slot 4i + c
    vec4 materials[TL_MAX_MATERIALS * TL_MATERIAL_VEC4S];
} tl;

#include "titleart/title_env.glsl"
#include "titleart/title_ramp.glsl"
#include "titleart/title_surface.glsl"
#include "titleart/title_gem.glsl"

vec4 tlMaterial(int m, int k) {
    return tl.materials[m * TL_MATERIAL_VEC4S + k];
}

void main() {
    int slot = int(v_ids.w) & 4095;
    vec4 slotRow = tl.slots[min(slot / 4, 15)];
    int m = int(slotRow[slot - (slot / 4) * 4]);
    vec4 m0 = tlMaterial(m, 0);     // base colour (linear), opacity
    vec4 m1 = tlMaterial(m, 1);     // metallic, roughness, env multiplier, specular multiplier
    vec4 m2 = tlMaterial(m, 2);     // clearcoat, coat roughness, iridescence, iridescence IOR
    vec4 m3 = tlMaterial(m, 3);     // iridescence thickness (nm), transmission, IOR, dispersion
    vec4 m4 = tlMaterial(m, 4);     // glass depth, flakes, flake cell (layer units), gradient bend
    vec4 m5 = tlMaterial(m, 5);     // emission (linear x intensity), ramp row (-1 none)
    vec4 m8 = tlMaterial(m, 8);     // surface amount, surface scale (layer units), surface seed, base texture front (1)

    vec3 V = normalize(tl.camera.xyz - v_world);
    vec3 N = normalize(v_normal);
    // no face culling (closed solids are depth tested): the normal faces the viewer
    if (dot(N, V) < 0.0) N = -N;
    vec3 Ng = N;

    // ---- base colour: the ramp in layer space ----
    vec3 baseColor = m0.rgb;
#ifdef TL_RAMP
    if (m5.w >= 0.0) baseColor *= tlRamp(m, m5.w, v_layerPos, normalize(v_layerNormal), v_zRange, v_ids, m4.w);
#endif
#ifdef TL_BASE_TEXTURE
    #ifdef TL_WRAP_UV
        vec2 baseUv = m8.w > 0.5 ? v_layerPos.xy / (tl.frame.w * 100.0) : v_wrap;
    #else
        vec2 baseUv = v_layerPos.xy / (tl.frame.w * 100.0);
    #endif
    vec4 tiling = tlMaterial(m, 9);
    vec4 baseTex = texture(sampler2D(u_tlBase, u_tlBase_smp), baseUv * tiling.xy + tiling.zw);
    baseColor *= baseTex.rgb;
#endif

    // ---- normal detail (front on, in layer space) ----
    float roughness = clamp(m1.y, 0.02, 1.0);
#if defined(TL_SURFACE_KIND) || defined(TL_FLAKES)
    vec3 layerPos = v_layerPos;
    vec3 layerTilt = vec3(0.0);
    #ifdef TL_SURFACE_KIND
        layerTilt += tlSurfaceTilt(layerPos.xy / max(m8.y, 1e-4), m8.z) * m8.x;
    #endif
    vec3 coatTilt = layerTilt;
    #ifdef TL_FLAKES
        layerTilt += tlFlakeTilt(layerPos, m4.z) * m4.y;
    #endif
    N = tlBendNormal(N, layerTilt);
    vec3 Nc = tlBendNormal(Ng, coatTilt);
    #ifdef TL_SPECULAR_AA
        roughness = specularAARoughness(roughness, specularAAKernel(N));
    #endif
#else
    vec3 Nc = N;
#endif

    float NdotV = clamp(abs(dot(N, V)), 1e-4, 1.0);
    float metallic = m1.x;
    vec3 diffuseColor = baseColor * (1.0 - metallic);
    float iorF0 = pow((m3.z - 1.0) / (m3.z + 1.0), 2.0);
    vec3 F0 = mix(vec3(iorF0), baseColor, metallic);
#ifdef TL_IRIDESCENCE
    F0 = mix(F0, iridescenceFresnel(NdotV, m2.w, m3.x, F0), m2.z);
#endif
#ifdef TL_GLASS_DEPTH
    // fake absorption: deeper and darker where the surface turns away (a longer path)
    float e = sqrt(max(0.0, 1.0 - NdotV * NdotV));
    float d = m4.x;
    diffuseColor = pow(max(diffuseColor, vec3(0.0)), vec3(1.0 + d * (3.0 * e - 0.4))) * (1.0 - 0.45 * d * e);
#endif

    // ---- image-based light ----
    vec3 Nenv = tlLockNormal(N);
    vec3 R = reflect(-V, N);
    vec3 Renv = tlLockNormal(R);
    vec3 specLight = tlEnvironment(Renv, roughness);
    vec3 diffLight = tlEnvironment(Nenv, 1.0);
    vec3 iblSpec = envRadianceGGX(specLight, NdotV, roughness, F0) * m1.z;
    vec3 iblDiff = envRadianceLambertian(diffLight, NdotV, roughness, diffuseColor, F0) * m1.z;

    // ---- direct light ----
    vec3 directDiff = tl.ambient.rgb * diffuseColor;
    vec3 directSpec = vec3(0.0);
    float alpha = roughness * roughness;
#ifdef TL_LIGHTS
    int lightCount = int(tl.grade2.w);
    for (int i = 0; i < TL_MAX_LIGHTS; i++) {
        if (i >= lightCount) break;
        vec4 l0 = tl.lights[i * 2];
        vec3 radiance = tl.lights[i * 2 + 1].rgb;
        vec3 L = normalize(l0.w > 0.5 ? l0.xyz - v_world : l0.xyz);
        vec3 H = normalize(L + V);
        float NdotL = clamp(dot(N, L), 0.0, 1.0);
        if (NdotL <= 0.0) continue;
        float NdotH = clamp(dot(N, H), 0.0, 1.0);
        float VdotH = clamp(dot(V, H), 0.0, 1.0);
        directDiff += radiance * NdotL * BRDF_lambertian(F0, vec3(1.0), diffuseColor, VdotH) * M_PI;
        directSpec += radiance * NdotL * BRDF_specularGGX(F0, vec3(1.0), alpha, VdotH, NdotL, NdotV, NdotH) * M_PI * m1.w;
    }
#endif

    vec3 diffuse = directDiff + iblDiff;
    vec3 specular = directSpec + iblSpec;

    // ---- gems: the environment refracted in and reflected back ----
#ifdef TL_GEM
    vec3 F = F_Schlick(F0, vec3(1.0), NdotV);
    diffuse = tlGemLight(N, V, m3.z, m3.w, roughness) * (vec3(1.0) - F) * baseColor * m3.y * m1.z + diffuse * (1.0 - m3.y);
#endif

    // ---- clearcoat (the KHR layering) ----
#ifdef TL_CLEARCOAT
    float ccNdotV = clamp(abs(dot(Nc, V)), 1e-4, 1.0);
    float ccRough = clamp(m2.y, 0.02, 1.0);
    vec3 ccF = F_Schlick(vec3(0.04), vec3(1.0), ccNdotV) * m2.x;
    vec3 Rc = tlLockNormal(reflect(-V, Nc));
    vec3 coat = envRadianceGGX(tlEnvironment(Rc, ccRough), ccNdotV, ccRough, vec3(0.04)) * m1.z * m2.x;
    #ifdef TL_LIGHTS
    for (int i = 0; i < TL_MAX_LIGHTS; i++) {
        if (i >= lightCount) break;
        vec4 l0 = tl.lights[i * 2];
        vec3 L = normalize(l0.w > 0.5 ? l0.xyz - v_world : l0.xyz);
        vec3 H = normalize(L + V);
        float NdotL = clamp(dot(Nc, L), 0.0, 1.0);
        coat += tl.lights[i * 2 + 1].rgb * NdotL * BRDF_specularGGX(vec3(0.04), vec3(1.0), ccRough * ccRough, clamp(dot(V, H), 0.0, 1.0), NdotL, ccNdotV,
                                                                     clamp(dot(Nc, H), 0.0, 1.0)) * M_PI * m1.w * m2.x;
    }
    #endif
    vec3 attenuation = vec3(1.0) - ccF;
    diffuse *= attenuation;
    specular = specular * attenuation + coat;
#endif

    vec3 added = specular;
#ifdef TL_EMISSIVE
    // emission is tinted by the same ramp as the base colour
    added += m5.rgb * (m0.rgb == vec3(0.0) ? vec3(1.0) : baseColor / max(m0.rgb, vec3(1e-4)));
#endif

    vec4 color;
#ifdef TL_TRANSPARENT
    float a = clamp(m0.a, 0.0, 1.0);
    color = vec4(diffuse * a + added, a + (1.0 - a) * clamp(max(added.r, max(added.g, added.b)), 0.0, 1.0));
#else
    color = vec4(diffuse + added, 1.0);
#endif

#ifdef TL_HDR_COMPRESS
    frag_color = hd_encodeLight(color);
#else
    if (tl.frame.z > 0.5 && color.a > 0.0) color.rgb = hd_shoulder(color.rgb / color.a, tl.camera.w) * color.a;
    frag_color = color;
#endif
}
