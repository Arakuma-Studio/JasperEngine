#version 450
// @effect     fluid
// @kind       paint
// @category   interactive
// @summary    Pavel Dobryakov's WebGL fluid: drag to paint glowing, swirling dye with bloom and sunrays
// @source     port WebGL-Fluid-Simulation (Pavel Dobryakov, MIT)
// @cost       stateful
// @state      vel rg16f aspect 128 max 512 linear
// @state      pressure r16f aspect 128 max 512 nearest
// @state      aux r16f aspect 128 max 512 nearest
// @state      dye rgba16f box max 1024 linear
// @state      ptr rgba32f 1x1 nearest
// @state      bloom rgba16f aspect 256 max 1024 linear
// @state      glow rgba16f aspect 256 max 1024 linear
// @state      sunrays r16f aspect 196 max 1024 linear
// @init       start-vel vel
// @init       start-dye dye
// @init       start-ptr ptr
// @step       pointer ptr
// @step       splat-vel vel
// @step       curl aux
// @step       vorticity vel
// @step       divergence aux
// @step       pressure-clear pressure
// @step       jacobi pressure xpressure-iterations
// @step       gradient vel
// @step       advect-vel vel
// @step       advect-dye dye
// @frame      bloom-prefilter bloom
// @frame      bloom-down glow xbloom-iterations
// @frame      bloom-up glow xbloom-iterations
// @frame      bloom-final bloom
// @frame      sunrays sunrays
// @frame      sunrays-blur sunrays x2
// @reseed     seed, splats-on-start, splat-count
// @suggest-for webgl-fluid fluid-simulation navier-stokes dye
// @param      preset enum default default|mobile|smoke|ink|calm-water|psychedelic -- A named look (the native Fluid Simulation's presets): sets the parameters you leave at their defaults
// @param      density-dissipation number 1 0 4 -- How fast the dye fades (1 / (1 + k dt) a step)
// @param      velocity-dissipation number 0.2 0 4 -- How fast the motion dies down
// @param      pressure number 0.8 0 1 -- The previous pressure kept as the solver's warm start
// @param      pressure-iterations integer 20 1 64 -- Jacobi pressure passes a step (more: less compressible, costlier)
// @param      curl number 30 0 50 -- Vorticity confinement: how strongly eddies curl
// @param      gradient-scale number 1 0 2 -- The pressure gradient subtracted (1 Pavel's, 0.5 the textbook projection)
// @param      splat-radius number 0.25 0.01 1 -- Size of a pointer splat (Pavel's SPLAT_RADIUS)
// @param      splat-force number 6000 0 20000 -- How hard the pointer's motion pushes the fluid
// @param      shading boolean on -- Light the dye by its brightness gradient (a 3D look)
// @param      colorful boolean on -- Cycle the pointer's colour while dragging
// @param      color-speed number 10 0 50 -- Pointer colour changes a second with colorful
// @param      hover boolean off -- Paint by moving the pointer without pressing (Pavel's needs a drag)
// @param      splats-on-start boolean on -- A burst of random splats when the simulation starts
// @param      splat-count integer 0 0 50 -- The start burst's splats (0: Pavel's random 5 to 24)
// @param      seed integer 0 0 999999 -- Every random choice (the burst, the colours) with the element's own seed
// @param      bloom boolean on -- Bright dye glows (HDR soft-knee bloom)
// @param      bloom-iterations integer 8 1 8 -- Bloom levels (each half the last): how far the glow spreads
// @param      bloom-intensity number 0.8 0.1 2 -- Strength of the glow
// @param      bloom-threshold number 0.6 0 1 -- Brightness above which dye glows
// @param      bloom-soft-knee number 0.7 0 1 -- Softness of the threshold
// @param      sunrays boolean on -- Light shafts through the dye from the centre
// @param      sunrays-weight number 1 0.3 1 -- Strength of the light shafts
// @preset     mobile shading off, bloom off, sunrays off
// @preset     smoke density-dissipation 0.4, velocity-dissipation 0.1, curl 20, splat-radius 0.5, splat-force 4000, colorful off, bloom off, sunrays-weight 0.6
// @preset     ink density-dissipation 0.15, velocity-dissipation 0.8, pressure-iterations 30, curl 8, splat-radius 0.12, splat-force 3000, colorful off, bloom off, sunrays off
// @preset     calm-water density-dissipation 0.5, velocity-dissipation 0.5, pressure-iterations 30, curl 5, splat-radius 0.4, splat-force 2500, color-speed 2, bloom-iterations 6, bloom-intensity 0.3, bloom-threshold 0.7, sunrays off
// @preset     psychedelic density-dissipation 0.5, velocity-dissipation 0.1, curl 50, splat-radius 0.3, splat-force 8000, color-speed 30, bloom-intensity 1.4, bloom-threshold 0.3, bloom-soft-knee 0.9

// A port of Pavel Dobryakov's WebGL-Fluid-Simulation (MIT, Copyright (c) 2017 Pavel Dobryakov;
// https://github.com/PavelDoGreat/WebGL-Fluid-Simulation; the notice is
// engine/shaderlib/LICENSE.WebGL-Fluid-Simulation.txt, shipped by exports that use this effect):
// his solver and look through the kernels the native Fluid Simulation shares
// (effects/lib/fam_fluid_solver.glsl, effects/lib/fam_fluid_look.glsl).
//
// - Grids: Pavel's getResolution. The velocity, pressure and curl/divergence grids keep the
//   area's aspect with a 128-texel short side (aspect 128), whatever the element's size, so the
//   fluid moves the same on a card and on a full screen; the dye follows the area's CSS px (at
//   most 1024: Pavel's 1024 dye on a page); bloom at 256 and sunrays at 196 on the short side.
//   The presets set the solver and the look, not these sizes (the native ones also change the
//   sim and dye resolutions).
// - A step (60 Hz, the effect clock's fixed step, at most 4 a frame): the pointer colour, splats, curl,
//   vorticity, divergence, pressure clear, Jacobi x pressure-iterations, gradient subtract,
//   advect the velocity, advect the dye (Pavel's step()). His splats (applyInputs before the
//   step) are a velocity pass (splat-vel, all of the step's splats summed: his splats add) and,
//   for the dye, folded into the dye advection (the Gaussian evaluated where the advection
//   samples: one dye-sized pass a step less). curl and divergence share the state `aux` (curl is
//   read only by vorticity, before divergence overwrites it).
// - The look runs once a drawn frame after the steps (@frame): bloom prefilter, bloom-iterations
//   blurs down a chain of halves, back up adding each level, the final blur x intensity; the
//   sunrays (the mask computed from the dye where it is sampled, not stored at the dye's size)
//   and one blur across and along. The bloom levels live in one atlas (`glow`, the base's size):
//   level 1 at the origin, the next ones stacked right of it; each sample is clamped to its
//   level, as his separate textures clamp to their edges.
// - The display (jfx_paint): the dye, shaded, x sunrays, plus the gamma-encoded dithered bloom;
//   alpha = max(r, g, b), premultiplied over the element's background (Pavel's BACK_COLOR is the
//   CSS background; TRANSPARENT is no background). The values are Pavel's display values (no
//   linear-light conversion), clamped as his canvas clamps them.
// - Input: a drag (his mouse) or, with `hover on`, any movement inside the area splats each step
//   at the pointer, force = the step's movement in area uv x splat-force (his aspect
//   corrections), colour HSV(random, 1, 1) x 0.15, a new colour on each press and color-speed
//   times a second (colorful). Scripted splats (effectSplat, MCP set_ui_effect_time splats): x, y,
//   radius in area px (the Gaussian's 1/e distance), dx, dy in px/s (the force a pointer moving
//   so fast would give), the colour's sRGB value x strength added to the dye (Pavel's pointer
//   colours are 0.15, his start burst 1.5).
// - Randomness: integer hashes of `seed`, the element's seed and a counter (no wall clock), so a
//   pinned clock replays the same fluid on every backend.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_state.glsl"

uniform u_fs_effectParams {
    float preset;
    float densityDissipation;
    float velocityDissipation;
    float pressure;
    float pressureIterations;
    float curl;
    float gradientScale;
    float splatRadius;
    float splatForce;
    float shading;
    float colorful;
    float colorSpeed;
    float hover;
    float splatsOnStart;
    float splatCount;
    float seed;
    float bloom;
    float bloomIterations;
    float bloomIntensity;
    float bloomThreshold;
    float bloomSoftKnee;
    float sunrays;
    float sunraysWeight;
} params;

JFX_STATE(vel)
JFX_STATE(pressure)
JFX_STATE(aux)
JFX_STATE(dye)
JFX_STATE(ptr)
JFX_STATE(bloom)
JFX_STATE(glow)
JFX_STATE(sunrays)

// ---- the step's splats (fx_gather_splats): the pointer's, then the scripted ones (jfx_splat)
float g_aspect = 1.0;
vec2 g_size = vec2(1.0);
float g_dt = 0.0;
bool g_pointerSplat = false;
vec2 g_pointerPoint = vec2(0.0);
vec2 g_pointerForce = vec2(0.0);
vec3 g_pointerColor = vec3(0.0);
float g_pointerRadius = 1.0;
int g_splats = 0;
int g_source = 0;                  // FLUID_SOURCE: 0 the velocity, 1 the dye with the step's dye splats

float fx_weight(vec2 uv, vec2 point, float radius){
    vec2 p = (uv - point) * vec2(g_aspect, 1.0);
    return exp(-dot(p, p) / radius);
}

// the sum of the step's splats at uv: their force (velocity) or their colour (the dye)
vec3 fx_splats(vec2 uv, bool velocity){
    vec3 sum = vec3(0.0);
    if (g_pointerSplat)
        sum += fx_weight(uv, g_pointerPoint, g_pointerRadius) * (velocity ? vec3(g_pointerForce, 0.0) : g_pointerColor);
#ifdef JFX_STEP_FORM
    for (int i = 0; i < 8; i++){
        if (i >= g_splats)
            break;
        JfxSplat sp = jfx_splat(i);
        float radius = max(sp.radius, 0.5) / g_size.y;
        float w = fx_weight(uv, sp.pos / g_size, radius * radius);
        if (velocity){
            vec2 move = sp.vel * g_dt / g_size;
            if (g_aspect < 1.0)
                move.x *= g_aspect;
            if (g_aspect > 1.0)
                move.y /= g_aspect;
            sum.xy += w * move * params.splatForce;
        }else{
            sum += w * jfx_linear_to_srgb(clamp(sp.color.rgb, 0.0, 1.0)) * sp.strength;
        }
    }
#endif
    return sum;
}

vec4 fx_source(vec2 uv){
    if (g_source == 0)
        return vec4(vel_sample(uv).xy, 0.0, 1.0);
    vec4 d = dye_sample(uv);
    d.rgb += fx_splats(uv, false);
    return d;
}

#define FLUID_VELOCITY(uv) vel_sample(uv).xy
#define FLUID_CURL(uv) aux_sample(uv).x
#define FLUID_DIVERGENCE(uv) aux_sample(uv).x
#define FLUID_PRESSURE(uv) pressure_sample(uv).x
#define FLUID_SOURCE(uv) fx_source(uv)
#include "effects/lib/fam_fluid_solver.glsl"

// ---- the look's reads: the sunrays mask of the dye, the bloom base, a level of the bloom atlas, the sunrays
int g_look = 0;
int g_level = 1;

ivec2 fx_level_size(int k){
    ivec2 base = glow_size();
    return ivec2(base.x >> k, base.y >> k);
}

// level k (1..) in the atlas: level 1 at the origin, the deeper ones stacked right of it
ivec2 fx_level_origin(int k){
    ivec2 base = glow_size();
    if (k <= 1)
        return ivec2(0);
    int y = 0;
    for (int j = 2; j < 16; j++){
        if (j >= k)
            break;
        y += base.y >> j;
    }
    return ivec2(base.x >> 1, y);
}

// the levels the chain has: at most bloom-iterations, each at least 2 x 2 (his initBloomFramebuffers)
int fx_levels(){
    ivec2 base = glow_size();
    int iterations = int(params.bloomIterations + 0.5);
    int levels = 0;
    for (int k = 1; k <= 8; k++){
        if (k > iterations || (base.x >> k) < 2 || (base.y >> k) < 2)
            break;
        levels = k;
    }
    return levels;
}

vec4 fx_level_sample(int k, vec2 uv){
    vec2 size = vec2(fx_level_size(k));
    vec2 px = clamp(uv * size, vec2(0.5), size - 0.5);
    return glow_sample((vec2(fx_level_origin(k)) + px) / vec2(glow_size()));
}

vec4 fluidSunraysMask(vec4 c);   // fam_fluid_look.glsl

vec4 fx_look(vec2 uv){
    if (g_look == 0)
        return fluidSunraysMask(dye_sample(uv));
    if (g_look == 1)
        return bloom_sample(uv);
    if (g_look == 2)
        return fx_level_sample(g_level, uv);
    return sunrays_sample(uv);
}

#define FLUID_LOOK_SOURCE(uv) fx_look(uv)
#include "effects/lib/fam_fluid_look.glsl"

// ---- the display
vec4 jfx_paint(JfxContext c){
    vec2 uv = c.uv;
    vec3 col = dye_sample(uv).rgb;
    if (params.shading > 0.5){
        // his texelSize: a pixel of the target
        vec2 texel = 1.0 / max(c.size * max(c.dpr, 1e-3), vec2(1.0));
        vec3 l = dye_sample(uv - vec2(texel.x, 0.0)).rgb;
        vec3 r = dye_sample(uv + vec2(texel.x, 0.0)).rgb;
        vec3 t = dye_sample(uv + vec2(0.0, texel.y)).rgb;
        vec3 b = dye_sample(uv - vec2(0.0, texel.y)).rgb;
        col *= fluidShadingDiffuse(l, r, t, b, texel);
    }
    bool useBloom = params.bloom > 0.5;
    bool useSunrays = params.sunrays > 0.5;
    vec3 glowed = useBloom ? bloom_sample(uv).rgb : vec3(0.0);
    float rays = useSunrays ? sunrays_sample(uv).x : 1.0;
    // the dither of the area's device pixel (the same pattern on every backend)
    float noise = fluidDitherNoise(floor(c.px * max(c.dpr, 1e-3)) + 0.5);
    vec4 o = fluidDisplayFinish(col, glowed, rays, noise, useBloom, useSunrays);
    // his premultiplied display values as the canvas clamps them, given to the engine straight and linear
    float a = clamp(o.a, 0.0, 1.0);
    vec3 premultiplied = clamp(o.rgb, 0.0, 1.0);
    vec3 straight = a > 1e-5 ? min(premultiplied / a, vec3(1.0)) : vec3(0.0);
    return vec4(jfx_srgb_to_linear(straight), a);
}

#ifdef JFX_STEP_FORM
uint fx_hash(uint x){
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// a random number in [0, 1) of a stream (0 the burst, 1 the pointer colours) and an index
float fx_random(JfxStep s, uint stream, uint index){
    uint seed = fx_hash(uint(params.seed + 0.5) * 0x9e3779b9u ^ uint(s.c.seed * 16777216.0));
    return float(fx_hash(seed ^ fx_hash(stream * 0x85ebca6bu + index * 0xc2b2ae35u + 0x27d4eb2fu)) >> 8) / 16777216.0;
}

// the step's splats: the pointer's (a drag, or any move with hover; none on a press's step: his
// press resets the delta), then the scripted ones
void fx_gather_splats(JfxStep s){
    g_size = max(s.c.size, vec2(1.0));
    g_aspect = g_size.x / g_size.y;
    g_dt = s.dt;
    g_splats = s.splatCount;
    bool painting = s.pointerKnown && s.pointerInside && (s.pointerDown || params.hover > 0.5) && !s.pressed;
    vec2 delta = (s.pointer - s.pointerPrev) / g_size;
    g_pointerSplat = painting && (delta.x != 0.0 || delta.y != 0.0);
    if (g_aspect < 1.0)
        delta.x *= g_aspect;
    if (g_aspect > 1.0)
        delta.y /= g_aspect;
    g_pointerPoint = s.pointer / g_size;
    g_pointerForce = delta * params.splatForce;
    g_pointerColor = fluidGenerateColor(ptr_fetch(ivec2(0)).y);
    g_pointerRadius = fluidSplatRadius(params.splatRadius, g_aspect);
}

// his multipleSplats at the start: velocity (true) or dye
vec4 fx_burst(JfxStep s, bool velocity){
    vec4 sum = vec4(0.0, 0.0, 0.0, 1.0);
    if (params.splatsOnStart < 0.5)
        return sum;
    vec2 size = max(s.c.size, vec2(1.0));
    float aspect = size.x / size.y;
    float radius = fluidSplatRadius(params.splatRadius, aspect);
    int count = int(params.splatCount + 0.5);
    if (count <= 0)
        count = 5 + int(fx_random(s, 0u, 0u) * 20.0);
    for (int i = 0; i < 50; i++){
        if (i >= count)
            break;
        uint k = uint(i) * 8u + 1u;
        vec3 color = fluidGenerateColor(fx_random(s, 0u, k)) * 10.0;
        vec2 point = vec2(fx_random(s, 0u, k + 1u), fx_random(s, 0u, k + 2u));
        vec2 force = 1000.0 * (vec2(fx_random(s, 0u, k + 3u), fx_random(s, 0u, k + 4u)) - 0.5);
        float w = fluidSplatWeight(s.uv, point, radius, aspect);
        if (velocity)
            sum.xy += w * force;
        else
            sum.rgb += w * color;
    }
    return sum;
}

vec4 jfx_step(JfxStep s){
    vec2 uv = s.uv;
    vec2 texel = 1.0 / s.size;
    // ---- @init: the start burst, the first pointer colour
    if (s.pass == JFX_PASS_START_VEL)
        return fx_burst(s, true);
    if (s.pass == JFX_PASS_START_DYE)
        return fx_burst(s, false);
    if (s.pass == JFX_PASS_START_PTR)
        return vec4(0.0, fx_random(s, 1u, 0u), 1.0, 1.0);
    // ---- a step: x the colour timer, y the colour's hue, z the colours drawn
    if (s.pass == JFX_PASS_POINTER){
        vec4 p = ptr_fetch(ivec2(0));
        if (s.pressed){
            p.y = fx_random(s, 1u, uint(p.z));
            p.z += 1.0;
        }
        if (params.colorful > 0.5){
            p.x += s.dt * params.colorSpeed;
            if (p.x >= 1.0){
                p.x = mod(p.x, 1.0);
                p.y = fx_random(s, 1u, uint(p.z));
                p.z += 1.0;
            }
        }
        return p;
    }
    if (s.pass == JFX_PASS_SPLAT_VEL){
        fx_gather_splats(s);
        return vec4(vel_sample(uv).xy + fx_splats(uv, true).xy, 0.0, 1.0);
    }
    if (s.pass == JFX_PASS_CURL)
        return vec4(fluidCurl(uv, texel), 0.0, 0.0, 1.0);
    if (s.pass == JFX_PASS_VORTICITY)
        return vec4(fluidVorticity(uv, texel, params.curl, s.dt), 0.0, 1.0);
    if (s.pass == JFX_PASS_DIVERGENCE)
        return vec4(fluidDivergence(uv, texel), 0.0, 0.0, 1.0);
    if (s.pass == JFX_PASS_PRESSURE_CLEAR)
        return vec4(fluidPressureClear(uv, params.pressure), 0.0, 0.0, 1.0);
    if (s.pass == JFX_PASS_JACOBI)
        return vec4(fluidJacobi(uv, texel), 0.0, 0.0, 1.0);
    if (s.pass == JFX_PASS_GRADIENT)
        return vec4(fluidGradientSubtract(uv, texel, params.gradientScale), 0.0, 1.0);
    if (s.pass == JFX_PASS_ADVECT_VEL){
        g_source = 0;
        return vec4(fluidAdvect(uv, texel, texel, s.dt, params.velocityDissipation).xy, 0.0, 1.0);
    }
    if (s.pass == JFX_PASS_ADVECT_DYE){
        fx_gather_splats(s);
        g_source = 1;
        vec2 velTexel = 1.0 / vec2(vel_size());
        return fluidAdvect(uv, velTexel, texel, s.dt, params.densityDissipation);
    }
    // ---- @frame: the look
    if (s.pass == JFX_PASS_BLOOM_PREFILTER || s.pass == JFX_PASS_BLOOM_FINAL){
        if (params.bloom < 0.5)
            return vec4(0.0);
        int levels = fx_levels();
        // his applyBloom does nothing on a chain shorter than 2
        if (levels < 2)
            return bloom_fetch(s.cell);
        if (s.pass == JFX_PASS_BLOOM_PREFILTER){
            float threshold = params.bloomThreshold;
            vec3 c = dye_sample(uv).rgb;
            return vec4(fluidBloomPrefilter(c, fluidBloomCurve(threshold, params.bloomSoftKnee), threshold), 0.0);
        }
        g_look = 2;
        g_level = 1;
        return fluidBloomBlur(uv, 1.0 / vec2(fx_level_size(1))) * params.bloomIntensity;
    }
    if (s.pass == JFX_PASS_BLOOM_DOWN || s.pass == JFX_PASS_BLOOM_UP){
        if (params.bloom < 0.5)
            return vec4(0.0);
        int levels = fx_levels();
        bool down = s.pass == JFX_PASS_BLOOM_DOWN;
        // down: iteration i writes level i + 1 from the one above it (level 0 the bloom base);
        // up: iteration i adds level L - i, blurred, into level L - 1 - i
        int target = down ? s.iteration + 1 : levels - 1 - s.iteration;
        vec4 kept = glow_fetch(s.cell);
        if (levels < 2 || target < 1 || target > levels)
            return kept;
        ivec2 origin = fx_level_origin(target);
        ivec2 size = fx_level_size(target);
        ivec2 local = s.cell - origin;
        if (local.x < 0 || local.y < 0 || local.x >= size.x || local.y >= size.y)
            return kept;
        vec2 levelUv = (vec2(local) + 0.5) / vec2(size);
        if (down){
            if (target == 1){
                g_look = 1;
                return fluidBloomBlur(levelUv, 1.0 / vec2(bloom_size()));
            }
            g_look = 2;
            g_level = target - 1;
            return fluidBloomBlur(levelUv, 1.0 / vec2(fx_level_size(target - 1)));
        }
        g_look = 2;
        g_level = target + 1;
        return kept + fluidBloomBlur(levelUv, 1.0 / vec2(fx_level_size(target + 1)));
    }
    if (params.sunrays < 0.5)
        return vec4(0.0);
    if (s.pass == JFX_PASS_SUNRAYS){
        g_look = 0;
        return vec4(fluidSunrays(uv, params.sunraysWeight), 0.0, 0.0, 1.0);
    }
    // the blur: across, then along
    g_look = 3;
    return fluidBlur(uv, s.iteration == 0 ? vec2(texel.x, 0.0) : vec2(0.0, texel.y));
}
#endif

#include "effects/lib/main_paint.glsl"
#include "effects/lib/main_step.glsl"
