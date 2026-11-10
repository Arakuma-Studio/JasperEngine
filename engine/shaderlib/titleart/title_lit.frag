#version 450
precision highp float;
precision highp int;

// Title Lit (docs/specs/S86-title-3d.md R18-R23, §6.4): the 3D title shader that replaces the
// thirteen three.js chunks Pachislop patches (src/engine/three/materials.ts, ported by SafEight,
// used with the author's permission). Metallic-roughness with the engine's GGX lobes
// (includes/brdf.glsl) and split-sum IBL (includes/ibl.glsl) on the linear float title
// environment, the layer-space banded ramp with its bend (title_ramp.glsl), clearcoat,
// iridescence (three.js's thin film: title_iridescence.glsl), glass depth, gems' environment refraction
// (title_gem.glsl), surface relief and flakes (title_surface.glsl), env lock, blur, repeat and
// the grade as uniforms (title_env.glsl). The output is premultiplied: opaque writes alpha 1;
// with opacity < 1, rgb = diffuse x a + added light and alpha = a + (1 - a) max(added light).
// TL_HDR_COMPRESS writes the compressed light of the RGBA8 path (hdr_compress.glsl); otherwise
// `frame.z` asks for the tone-map-aware shoulder on MSAA samples (with TL_HDR_COMPRESS: the
// linear encoding of a supersampled render, hdr_compress.glsl). Variants: the TL_* bits of
// TitleTierFeatures.h. A slot's material comes from the slot table, so one draw can cover the
// opaque slots of a variant (v_ids.w low 12 bits). TL_OUTPUT 1 and 2 are render_title's
// debug views: the shading normal (world, 0..1) and the reflected environment, unweighted;
// TL_OUTPUT 3 is the AO prepass (R31): the view normal and the title-relative depth
// (z_view - z_centre) / radius into RGBA16F (a zero normal marks no surface).
// TL_GEM_BEHIND (Bake, High; R21): a gem refracts the layer's own opaque parts from the behind
// copy (premultiplied, box-mipped): the view ray refracted through the gem's thickness and
// projected, at lod log2(behind width x frame span) x roughness x clamp(2 ior - 2, 0, 1) (three.js
// transmission, as Pachislop); where nothing is behind, the environment path, and the alpha
// lets the uncovered rest show through.
// TL_PATTERN (R22): S80's pattern GLSL (patterns.glsl) in layer space: marks over the base colour,
// colour 2 over the marks, a flat share by (1 - shade) after lighting, relief bending the normal
// by finite differences. Caps (the layer normal +-z, unrigged) read a layer position rebuilt
// per pixel from the view ray and their plane, so the earcut slivers' interpolation shows no
// seams (Pachislop's layerSurfacePos); surface relief and flakes read it too.
// TL_FACE_IMAGE (R1a): the layer's own picture projected straight on in layer space: on the face
// (slot role 0) the premultiplied picture over the base colour by face_image x its alpha against
// the traced edge's; on the bevel and side walls the opaque edge-extended copy by edge_image.

const float M_PI = 3.141592653589793;

#include "includes/brdf.glsl"
#include "includes/ibl.glsl"
#include "titleart/title_iridescence.glsl"
#include "includes/specular_aa.glsl"
#include "titleart/hdr_compress.glsl"

#define TL_MAX_MATERIALS 16
#define TL_MATERIAL_VEC4S 14
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
#ifdef TL_GEM_BEHIND
uniform texture2D u_tlBehind;
uniform sampler u_tlBehind_smp;
#endif
#ifdef TL_PATTERN
uniform texture2D u_tlPattern;
uniform sampler u_tlPattern_smp;
#endif
#ifdef TL_FACE_IMAGE
uniform texture2D u_tlFace;
uniform sampler u_tlFace_smp;
uniform texture2D u_tlFaceEdge;
uniform sampler u_tlFaceEdge_smp;
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
    vec4 view0;         // the rows of world -> view (w: translation)
    vec4 view1;
    vec4 ambient;       // rgb: ambient light
    vec4 axisX;         // xyz: the layer's +x in world space (surface relief, flakes)
    vec4 axisY;         // xyz: the layer's +y
    vec4 view2;
    vec4 aoDepth;       // the AO prepass: x the title centre's view depth, y the title's radius
    vec4 behind;        // x: log2(behind width x frame span), y: gem thickness (world), z: 1 = behind bound, w: 1 = origin top left
    mat4 viewProj;      // the main pass's projection x view (the gem's exit point)
    mat4 viewProjInv;   // its inverse (the caps' rebuilt position)
    mat4 layerInv;      // world -> layer space (after the shear)
    vec4 viewport;      // xy: the main target's size (px), zw: the skew
    vec4 faceBox;       // the picture's layer-space box (x0, y0, x1, y1, y up)
    vec4 faceMix;       // x: face_image, y: the traced edge's alpha, z: edge_image
    vec4 lights[TL_MAX_LIGHTS * 2];     // per light: (direction or position, type 0 dir / 1 point), (colour x intensity, 0)
    vec4 slots[16];     // the material index of slot 4i + c
    vec4 materials[TL_MAX_MATERIALS * TL_MATERIAL_VEC4S];
} tl;

#include "titleart/title_env.glsl"
#include "titleart/title_ramp.glsl"
#include "titleart/title_surface.glsl"
#include "titleart/title_gem.glsl"
#ifdef TL_PATTERN
    #define TA_PATTERN_IMAGE sampler2D(u_tlPattern, u_tlPattern_smp)
    #include "titleart/patterns.glsl"
#endif

vec4 tlMaterial(int m, int k) {
    return tl.materials[m * TL_MATERIAL_VEC4S + k];
}

#if defined(TL_PATTERN) || defined(TL_SURFACE_KIND) || defined(TL_FLAKES)
// the layer-space position for screen-derivative work: on caps (layer normal +-z, an exact
// constant across earcut's sliver triangles) rebuilt from the pixel's view ray and the cap's
// plane, so their interpolated varyings' unstable derivatives show no seams
vec3 tlSurfacePos() {
#ifdef TL_RIGS
    return v_layerPos;
#else
    if (abs(v_layerNormal.z) < 0.999) return v_layerPos;
    vec2 f = gl_FragCoord.xy / tl.viewport.xy;
    vec2 ndc = vec2(f.x * 2.0 - 1.0, tl.behind.w > 0.5 ? 1.0 - f.y * 2.0 : f.y * 2.0 - 1.0);
    vec4 far = tl.viewProjInv * vec4(ndc, 0.5, 1.0);
    vec3 dir = far.xyz / far.w - tl.camera.xyz;
    vec3 o = (tl.layerInv * vec4(tl.camera.xyz, 1.0)).xyz;
    vec3 d = mat3(tl.layerInv) * dir;
    // the cap's plane after the shear (a shear moves xy by z only)
    float z = v_layerPos.z;
    if (abs(d.z) < 1e-6) return v_layerPos;
    vec3 p = o + d * ((z - o.z) / d.z);
    p.xy -= tl.viewport.zw * (-z);
    return p;
#endif
}
#endif

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
#if TL_OUTPUT == 3
    vec3 viewN = vec3(dot(tl.view0.xyz, N), dot(tl.view1.xyz, N), dot(tl.view2.xyz, N));
    float viewZ = -(dot(tl.view2.xyz, v_world) + tl.view2.w);
    frag_color = vec4(normalize(viewN), (viewZ - tl.aoDepth.x) / max(tl.aoDepth.y, 1e-4));
    return;
#endif

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

    // ---- the layer's own picture ----
#ifdef TL_FACE_IMAGE
    {
        vec2 faceUv = vec2((v_layerPos.x - tl.faceBox.x) / (tl.faceBox.z - tl.faceBox.x),
                           (tl.faceBox.w - v_layerPos.y) / (tl.faceBox.w - tl.faceBox.y));
        if ((int(v_ids.w) >> 12) == 0) {
            vec4 t = texture(sampler2D(u_tlFace, u_tlFace_smp), faceUv);
            vec3 c = tlSrgbToLinear(min(t.rgb / max(t.a, 1e-4), vec3(1.0)));
            baseColor = mix(baseColor, c, tl.faceMix.x * clamp(t.a / max(tl.faceMix.y, 1e-3), 0.0, 1.0));
        } else {
            vec4 t = texture(sampler2D(u_tlFaceEdge, u_tlFaceEdge_smp), faceUv);
            baseColor = mix(baseColor, tlSrgbToLinear(t.rgb), tl.faceMix.z);
        }
    }
#endif

    // ---- the pattern: marks over the base colour, colour 2 over the marks ----
#if defined(TL_PATTERN) || defined(TL_SURFACE_KIND) || defined(TL_FLAKES)
    vec3 surfL = tlSurfacePos();
#endif
#ifdef TL_PATTERN
    vec4 pk = tlMaterial(m, 10);    // kind, seed, opacity, shade
    vec4 pc = tlMaterial(m, 11);    // colour (linear), relief
    vec4 pc2 = tlMaterial(m, 12);   // colour 2 (linear), motif size (layer units)
    vec4 px = tlMaterial(m, 13);    // layer -> motif units (rows xy, zw)
    vec2 patUv = vec2(dot(px.xy, surfL.xy), dot(px.zw, surfL.xy));
    vec2 patDx = dFdx(patUv), patDy = dFdy(patUv);
    int patKind = int(pk.x + 0.5);
    vec4 patS = patternSampleD(patUv, patDx, patDy, patKind, pk.y);
    float patWx = clamp(patS.x, 0.0, 1.0) * pk.z;
    float patWy = clamp(patS.y, 0.0, 1.0) * pk.z;
    float patW = 1.0 - (1.0 - patWx) * (1.0 - patWy);
    vec3 patCol = (pc.rgb * patWx * (1.0 - patWy) + pc2.rgb * patWy) / max(patW, 1e-5);
    baseColor = mix(baseColor, patCol, patW);
#endif

    // ---- normal detail (front on, in layer space) ----
    float roughness = clamp(m1.y, 0.02, 1.0);
#ifdef TL_PATTERN
    // relief: the height's change per motif, by finite differences of 2 % of a motif
    if (pc.w > 0.0) {
        const float patE = 0.02;
        vec2 stepX = patE * pc2.w * vec2(px.x, px.z), stepY = patE * pc2.w * vec2(px.y, px.w);
        vec2 patGL = vec2(patternSampleD(patUv + stepX, patDx, patDy, patKind, pk.y).z - patS.z,
                          patternSampleD(patUv + stepY, patDx, patDy, patKind, pk.y).z - patS.z) / patE;
        vec3 reliefTilt = (tl.axisX.xyz * patGL.x + tl.axisY.xyz * patGL.y) * (pc.w * 0.06);
        N = normalize(N - (reliefTilt - dot(reliefTilt, N) * N));
    }
#endif
#if defined(TL_SURFACE_KIND) || defined(TL_FLAKES)
    vec3 layerPos = surfL;
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
    // three.js: the film's reflectance at the view angle, as the F0 both lobes take
    F0 = mix(F0, tlSchlickToF0(tlIridescenceFresnel(NdotV, m2.w, m3.x, F0), 1.0, NdotV), m2.z);
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
    // three.js's light units (Pachislop's): irradiance x BRDF, the Lambert term diffuse / pi
    vec3 directDiff = tl.ambient.rgb * diffuseColor / M_PI;
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
        directDiff += radiance * NdotL * BRDF_lambertian(F0, vec3(1.0), diffuseColor, VdotH);
        directSpec += radiance * NdotL * BRDF_specularGGX(F0, vec3(1.0), alpha, VdotH, NdotL, NdotV, NdotH) * m1.w;
    }
#endif

    vec3 diffuse = directDiff + iblDiff;
    vec3 specular = directSpec + iblSpec;

    // ---- gems: what is behind refracted through, else the environment refracted in and back ----
#ifdef TL_GEM
    vec3 F = F_Schlick(F0, vec3(1.0), NdotV);
    vec3 gemEnv = tlGemLight(N, V, m3.z, m3.w, roughness) * (vec3(1.0) - F) * baseColor * m1.z;
    float gemAlpha = 1.0;
    #ifdef TL_GEM_BEHIND
    if (tl.behind.z > 0.5) {
        vec4 seen = tlBehind(N, V, m3.z, m3.w, roughness);
        float transmittance = (baseColor.r + baseColor.g + baseColor.b) / 3.0;
        gemEnv = (vec3(1.0) - F) * baseColor * seen.rgb + gemEnv * (1.0 - seen.a);
        gemAlpha = mix(1.0, 1.0 - (1.0 - seen.a) * transmittance, m3.y);
    }
    #endif
    diffuse = gemEnv * m3.y + diffuse * (1.0 - m3.y);
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
                                                                     clamp(dot(Nc, H), 0.0, 1.0)) * m1.w * m2.x;
    }
    #endif
    vec3 attenuation = vec3(1.0) - ccF;
    diffuse *= attenuation;
    specular = specular * attenuation + coat;
#endif

    vec3 added = specular;
#ifdef TL_PATTERN
    // the flat (unlit) share of the marks
    float patFlat = patW * (1.0 - pk.w);
    diffuse = mix(diffuse, patCol, patFlat);
    added *= 1.0 - patFlat;
#endif
#ifdef TL_EMISSIVE
    // emission is tinted by the same ramp as the base colour
    added += m5.rgb * (m0.rgb == vec3(0.0) ? vec3(1.0) : baseColor / max(m0.rgb, vec3(1e-4)));
#endif

    vec4 color;
#ifdef TL_TRANSPARENT
    float a = clamp(m0.a, 0.0, 1.0);
    #ifdef TL_GEM
    a *= gemAlpha;
    #endif
    color = vec4(diffuse * a + added, a + (1.0 - a) * clamp(max(added.r, max(added.g, added.b)), 0.0, 1.0));
#else
    color = vec4(diffuse + added, 1.0);
#endif

#if TL_OUTPUT == 1
    color = vec4(N * 0.5 + 0.5, 1.0);
#elif TL_OUTPUT == 2
    color = vec4(specLight, 1.0);
#endif

#ifdef TL_HDR_COMPRESS
    if (tl.frame.z > 0.5) {
        // supersampled: the linear encoding, dithered by half a code in the target's sRGB coding
        // (the k x k texels of a pixel average the rounding out)
        vec4 encoded = hd_encodeLinear(color);
        float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5;
        vec3 code = mix(encoded.rgb * 12.92, 1.055 * pow(encoded.rgb, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), encoded.rgb));
        code = max(code + ign / 255.0, vec3(0.0));
        encoded.rgb = mix(code / 12.92, pow((code + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), code));
        frag_color = encoded;
    } else {
        frag_color = hd_encodeLight(color);
    }
#else
    if (tl.frame.z > 0.5 && color.a > 0.0) color.rgb = hd_shoulder(color.rgb / color.a, tl.camera.w) * color.a;
    frag_color = color;
#endif
}
