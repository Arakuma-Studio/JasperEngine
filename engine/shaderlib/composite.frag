#version 450

// Composites the offscreen scene color with the screen-space reflection buffer
// and writes the result to the real render target.
//
// SSR replaces the environment specular already in the scene colour instead of adding to it
// (the Unity HDRP / Godot model): this pass rebuilds, per pixel, the term the colour pass added
// (the G-buffer's reflection source, includes/ssr_source.glsl: the sky, the local probe in a
// slot blended over the sky, or the scene ambient) and swaps it for the reflection by the SSR
// confidence. The swap is weighed as the colour pass weighs that term: by its occlusion (the
// material's map and a painted terrain's, G-buffer color[3], times SSAO) and by the fog in
// front of the surface. Where SSR reflects nothing the scene colour is returned as it is.
//
//   out = scene + T * ao * (w * ssr - min(w, 1) * env)      w = SSR mask * intensity
//
// in linear light: the scene is decoded as the colour pass stored it (comp.sceneEncoding). Under
// the post-processing colour the colour pass stores linear light (includes/post_color.glsl's
// linear store) and this pass applies the tone curve, the grade, the vignette and the dither to
// the result (its own u_fs_postColor, the scene's), as the colour pass would have; else the
// colour pass's sRGB-encoded colour is swapped in linear light and passed through elsewhere.
//
// A fullscreen pass fragment at v_texcoord t writes destination texel t and samples
// scene/SSR (composite space) at t, flipped by params.w when the destination is the
// GL swapchain (bottom-up) and the sources are not. The G-buffer is in depth space,
// sampled at the flipped composite coordinate when params.y says the spaces differ.

in vec2 v_texcoord;
out vec4 frag_color;

#include "includes/ssr_source.glsl"

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;
uniform texture2D u_ssrTexture;
uniform sampler u_ssr_smp;
uniform texture2D u_depthTexture;     // G-buffer color[0]: packed depth
uniform sampler u_depth_smp;
uniform texture2D u_gbufferTexture;   // G-buffer color[1]: oct normal (8 bits) + roughness + metallic
uniform sampler u_gbuffer_smp;
uniform texture2D u_albedoTexture;    // G-buffer color[2]: base color + reflection source
uniform sampler u_albedo_smp;
uniform texture2D u_occlusionGBufferTexture; // G-buffer color[3]: the colour pass's occlusion, the normal's fine bits
uniform sampler u_occlusionGBuffer_smp;
uniform texture2D u_ssaoTexture;      // the SSAO the colour pass applied (white when off)
uniform sampler u_ssao_smp;
uniform textureCube u_GGXEnvTexture;  // the sky's prefiltered environment (GGX)
uniform sampler u_GGXEnv_smp;
uniform textureCube u_probeTexture0;  // the local reflection probes, by slot
uniform sampler u_probe0_smp;
uniform textureCube u_probeTexture1;
uniform sampler u_probe1_smp;
uniform textureCube u_probeTexture2;
uniform sampler u_probe2_smp;
uniform textureCube u_probeTexture3;
uniform sampler u_probe3_smp;

uniform u_fs_compositeParams {
    mat4 invProjection;
    mat4 invView;
    vec4 params;      // x = intensity, y = flip G-buffer Y, z = debug, w = flip destination Y
    vec4 envColor;    // rgb = sky colour (linear), w = sky rotation (radians)
    vec4 ambient;     // rgb = the scene ambient Lit adds, w = fog: 0 none, 1 distance, 2 height
    vec4 projectionW; // the projection's 4th row: the clip w of a view position
    vec4 probePosition[SSR_PROBE_SLOTS]; // xyz = capture position
    vec4 probeBoxMin[SSR_PROBE_SLOTS];   // xyz = influence AABB min, w = intensity
    vec4 probeBoxMax[SSR_PROBE_SLOTS];   // xyz = influence AABB max, w = max mip LOD (0 = unfiltered)
    vec4 sceneEncoding; // x 1 = the surfaces are stored in linear light, y 1 = the background too
} comp;

#include "includes/depth_util.glsl"
#include "includes/octahedral.glsl"
#include "includes/srgb.glsl"
// the scene's post-processing colour, which a linear store leaves to this pass (neutral else)
#define USE_POST_COLOR
#include "includes/post_color.glsl"
// ENV_GGX_LODS and envBRDFApprox (without USE_IBL nothing else is declared)
#include "includes/ibl.glsl"
// the distance fog (u_fs_fog) and the height fog (u_fs_heightFog), with the colour pass's values
#define HAS_HEIGHT_FOG
#include "includes/fog.glsl"

// the encoded scene colour (0..1) from which the swap fades out towards a clipped channel
const float SSR_CLIP_FADE_START = 0.94;

vec3 rotateEnv(vec3 dir, float angle){
    float c = cos(angle);
    float s = sin(angle);
    return vec3(c * dir.x - s * dir.z, dir.y, s * dir.x + c * dir.z);
}

// See ssr.frag: the packed depth is backend-independent, so no depth-range branch,
// but Y still needs the per-backend mapping.
vec3 reconstructViewPos(vec2 uv, float depth01){
    vec3 ndc = vec3(uv.x * 2.0 - 1.0, depthUVToNDCY(uv.y), depth01 * 2.0 - 1.0);
    vec4 view = comp.invProjection * vec4(ndc, 1.0);
    return view.xyz / view.w;
}

// as ibl.glsl's boxProjectReflection, so the term taken out matches the one the colour pass added
vec3 probeBoxProject(vec3 direction, vec3 worldPosition, int slot){
    vec3 safeDir = vec3(
        abs(direction.x) < 0.0001 ? (direction.x < 0.0 ? -0.0001 : 0.0001) : direction.x,
        abs(direction.y) < 0.0001 ? (direction.y < 0.0 ? -0.0001 : 0.0001) : direction.y,
        abs(direction.z) < 0.0001 ? (direction.z < 0.0 ? -0.0001 : 0.0001) : direction.z);
    vec3 t0 = (comp.probeBoxMin[slot].xyz - worldPosition) / safeDir;
    vec3 t1 = (comp.probeBoxMax[slot].xyz - worldPosition) / safeDir;
    vec3 tFar = max(t0, t1);
    float distanceToBox = min(tFar.x, min(tFar.y, tFar.z));
    vec3 hit = worldPosition + safeDir * max(distanceToBox, 0.0);
    return normalize(hit - comp.probePosition[slot].xyz);
}

// samplers cannot be indexed dynamically on GLES/WebGL, so the slot picks a branch
vec3 probeTap(int slot, vec3 direction, float lod){
    vec4 c;
    if (slot == 0)      c = textureLod(samplerCube(u_probeTexture0, u_probe0_smp), direction, lod);
    else if (slot == 1) c = textureLod(samplerCube(u_probeTexture1, u_probe1_smp), direction, lod);
    else if (slot == 2) c = textureLod(samplerCube(u_probeTexture2, u_probe2_smp), direction, lod);
    else                c = textureLod(samplerCube(u_probeTexture3, u_probe3_smp), direction, lod);
    return sRGBToLinear(c).rgb;
}

// as ibl.glsl's sampleLocalProbe
vec3 sampleProbe(int slot, vec3 direction, float lod){
    float maxLod = comp.probeBoxMax[slot].w;
    if (maxLod > 0.0){
        return probeTap(slot, direction, min(lod, maxLod));
    }

    // a capture not prefiltered yet: the same small angular filter as ibl.glsl
    float roughness = clamp(lod / ENV_GGX_LODS, 0.0, 1.0);
    vec3 helper = abs(direction.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(helper, direction));
    vec3 bitangent = cross(direction, tangent);
    float spread = roughness * roughness * 0.45;
    vec3 color = probeTap(slot, direction, 0.0) * 4.0;
    color += probeTap(slot, normalize(direction + tangent * spread), 0.0);
    color += probeTap(slot, normalize(direction - tangent * spread), 0.0);
    color += probeTap(slot, normalize(direction + bitangent * spread), 0.0);
    color += probeTap(slot, normalize(direction - bitangent * spread), 0.0);
    return color * 0.125;
}

// The fog's transmittance in front of the surface at view position P (G-buffer coordinate d),
// as the colour pass fogged it: the distance fog factor at its fog distance (window depth times
// clip w, includes/fog.glsl), or the height fog's transmittance over the view ray
// (includes/height_fog.glsl heightFogSegment). From the G-buffer, not gl_FragCoord: the
// destination's pixels may be laid out otherwise than the colour pass's.
float fogTransmittance(vec2 d, float depth01, vec3 P){
    float mode = comp.ambient.w;
    if (mode < 0.5)
        return 1.0;
    if (mode < 1.5){
        float clipW = dot(comp.projectionW, vec4(P, 1.0));
        return distanceFogFactor(depth01 * clipW);
    }
    vec3 worldPos = (comp.invView * vec4(P, 1.0)).xyz;
    // the eye in perspective, the near-plane point of the pixel's ray in orthographic cameras
    vec3 originView = vec3(0.0);
    if (comp.projectionW.w > 0.5)
        originView = reconstructViewPos(d, 0.0);
    vec3 origin = (comp.invView * vec4(originView, 1.0)).xyz;
    vec3 toSurface = worldPos - origin;
    float distanceToSurface = length(toSurface);
    if (distanceToSurface <= 1e-6)
        return 1.0;
    float tau = heightFogOpticalDepth(origin, toSurface / distanceToSurface, 0.0, distanceToSurface);
    return max(exp(-tau), 1.0 - heightFog.extinction_base_H_maxOpacity.w);
}

// the colour pass's stored colour of a surface (or, background, of the background) in linear light
vec3 storedToLinear(vec3 stored, bool background){
    if (comp.sceneEncoding.x > 0.5 && (!background || comp.sceneEncoding.y > 0.5))
        return postColorStoredToLinear(stored);
    return sRGBToLinear(stored);
}

// a linear colour encoded for the target: the post-processing colour after a linear store
vec3 linearToTarget(vec3 linearColor){
    if (comp.sceneEncoding.x > 0.5)
        return postColorEncode(linearColor);
    return linearTosRGB(max(linearColor, vec3(0.0)));
}

// a pixel as the colour pass drew it: as stored, or the stored linear light encoded
vec4 asDrawn(vec3 stored, bool background){
    if (comp.sceneEncoding.x > 0.5 && (!background || comp.sceneEncoding.y > 0.5))
        return vec4(postColorEncode(postColorStoredToLinear(stored)), 1.0);
    return vec4(stored, 1.0);
}

// the Reflection Source debug view's colours (docs/rendering/reflections.md)
vec3 sourceColor(int code){
    if (code == SSR_SOURCE_ADD)           return vec3(1.0, 0.0, 1.0);
    if (code == SSR_SOURCE_AMBIENT)       return vec3(0.5);
    if (code == SSR_SOURCE_SKY)           return vec3(0.2, 0.45, 1.0);
    if (code == SSR_SOURCE_PROBE_NO_SLOT) return vec3(1.0, 0.5, 0.0);
    if (ssrSourceIsProbe(code)){
        int slot = ssrSourceProbeSlot(code);
        vec3 c = slot == 0 ? vec3(1.0, 0.0, 0.0) : (slot == 1 ? vec3(0.0, 1.0, 0.0) : (slot == 2 ? vec3(1.0, 1.0, 0.0) : vec3(0.0, 1.0, 1.0)));
        return c * (0.35 + 0.65 * ssrSourceProbeWeight(code));
    }
    return vec3(0.0);
}

void main(){
    // composite space, flipped when writing to the GL swapchain
    vec2 c = v_texcoord;
    if (comp.params.w > 0.5) c.y = 1.0 - c.y;

    vec3 scene = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), c).rgb;
    vec4 refl  = texture(sampler2D(u_ssrTexture, u_ssr_smp), c);

    int mode = int(comp.params.z + 0.5);   // debug mode (0 = off)
    float intensity = comp.params.x;

    // nothing reflected here, as on most pixels: the scene is final
    float w = refl.a * intensity;
    if (mode == 0 && w < 1.0 / 1024.0){
        // (a surface or the background: the G-buffer's depth tells them apart when it matters)
        if (comp.sceneEncoding.x > 0.5 && comp.sceneEncoding.y < 0.5){
            vec2 g = c;
            if (comp.params.y > 0.5) g.y = 1.0 - g.y;
            frag_color = asDrawn(scene, decodeDepth(texture(sampler2D(u_depthTexture, u_depth_smp), g)) >= 1.0);
        }else{
            frag_color = asDrawn(scene, false);
        }
        return;
    }

    // G-buffer is in depth space
    vec2 d = c;
    if (comp.params.y > 0.5) d.y = 1.0 - d.y;

    float depth01 = decodeDepth(texture(sampler2D(u_depthTexture, u_depth_smp), d));

    // background (no geometry): nothing to reflect, the SSR mask is 0 anyway
    if (depth01 >= 1.0){
        if (mode == 0)      frag_color = asDrawn(scene, true);
        else if (mode == 1) frag_color = vec4(refl.rgb * refl.a, 1.0);
        else                frag_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec4 alb = texture(sampler2D(u_albedoTexture, u_albedo_smp), d);
    int code = ssrSourceCode(alb.a);

    // left as drawn (the glossy blur can spread the mask onto these pixels)
    if (mode == 0 && ssrSourceSkips(code)){
        frag_color = asDrawn(scene, false);
        return;
    }

    vec4 g = texture(sampler2D(u_gbufferTexture, u_gbuffer_smp), d);
    vec4 extra = texture(sampler2D(u_occlusionGBufferTexture, u_occlusionGBuffer_smp), d);
    vec3 nV = octDecode16(g.rg, extra.gb);
    float roughness = g.b;
    float metallic = g.a;
    vec3 albedo = alb.rgb;

    vec3 P = reconstructViewPos(d, depth01);
    if (dot(nV, P) > 0.0) nV = -nV;      // orient toward camera
    vec3 vV = normalize(-P);
    float NdotV = clamp(dot(nV, vV), 0.0, 1.0);

    // GGX environment BRDF weight (FssEss): the physical reflectance of this surface,
    // including metal F0. Applied to both the environment specular and the SSR reflection.
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec2 f_ab = envBRDFApprox(NdotV, roughness);
    vec3 Fr = max(vec3(1.0 - roughness), f0) - f0;
    vec3 k_S = f0 + Fr * pow(1.0 - NdotV, 5.0);
    vec3 FssEss = k_S * f_ab.x + f_ab.y;

    // the environment light the colour pass reflected, and its occlusion (none rebuilt for
    // Skip and Add: they swap nothing, and their variants apply no occlusion)
    vec3 envLight = vec3(0.0);
    float ao = 1.0;
    if (code == SSR_SOURCE_AMBIENT){
        envLight = comp.ambient.rgb;
    }else if (code == SSR_SOURCE_SKY || ssrSourceIsProbe(code)){
        vec3 rV = reflect(-vV, nV);                    // view-space reflection
        vec3 rW = normalize(mat3(comp.invView) * rV);  // -> world space for the env lookup
        float lod = roughness * ENV_GGX_LODS;
        float probeWeight = ssrSourceIsProbe(code) ? ssrSourceProbeWeight(code) : 0.0;
        if (probeWeight < 1.0){
            envLight = sRGBToLinear(textureLod(samplerCube(u_GGXEnvTexture, u_GGXEnv_smp), rotateEnv(rW, comp.envColor.w), lod)).rgb * comp.envColor.rgb;
        }
        if (probeWeight > 0.0){
            int slot = ssrSourceProbeSlot(code);
            vec3 worldPos = (comp.invView * vec4(P, 1.0)).xyz;
            vec3 local = sampleProbe(slot, probeBoxProject(rW, worldPos, slot), lod) * comp.probeBoxMin[slot].w;
            envLight = mix(envLight, local, probeWeight);
        }
    }
    if (code == SSR_SOURCE_AMBIENT || code == SSR_SOURCE_SKY || ssrSourceIsProbe(code)){
        ao = extra.r * texture(sampler2D(u_ssaoTexture, u_ssao_smp), d).r;
    }
    vec3 envSpec = envLight * FssEss;

    // debug visualizations of the SSR G-buffer channels
    if (mode != 0){
        if (mode == 1)      frag_color = vec4(refl.rgb * refl.a, 1.0);          // reflection
        else if (mode == 2) frag_color = vec4(nV * 0.5 + 0.5, 1.0);            // view normal
        else if (mode == 3) frag_color = vec4(vec3(roughness), 1.0);          // roughness
        else if (mode == 4) frag_color = vec4(vec3(metallic), 1.0);           // metallic
        else if (mode == 5) frag_color = vec4(linearTosRGB(albedo), 1.0);     // base color
        else if (mode == 7) frag_color = vec4(sourceColor(code), 1.0);        // reflection source
        else if (mode == 8) frag_color = vec4(vec3(ao), 1.0);                 // occlusion
        else                frag_color = vec4(linearTosRGB(envSpec), 1.0);    // environment specular
        return;
    }

    // SSR reflection: un-premultiply, linearize, apply the same BRDF weight
    vec3 ssrSpec = storedToLinear(refl.rgb / max(refl.a, 1e-4), false) * FssEss;

    // the swap: the reflection by its confidence (intensity a strength multiplier), never more
    // of the environment taken out than there is, weighed by occlusion and fog
    float T = fogTransmittance(d, depth01, P);
    // The scene colour is clipped at 1: a pixel there lost what its colour pass added beyond it, so
    // taking the rebuilt term out would leave a dark hole (a bright probe or the sun on chrome). The
    // swap fades out as the pixel reaches white; a clipped pixel stays as drawn.
    float clipFade = 1.0 - smoothstep(SSR_CLIP_FADE_START, 1.0, max(scene.r, max(scene.g, scene.b)));
    vec3 sceneLin = storedToLinear(scene, false);
    vec3 outLin = sceneLin + clipFade * T * ao * (w * ssrSpec - min(w, 1.0) * envSpec);

    frag_color = vec4(linearToTarget(max(outLin, vec3(0.0))), 1.0);
}
