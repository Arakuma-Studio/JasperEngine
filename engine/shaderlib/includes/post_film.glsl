// Film simulation for the uber pass (uber.frag; S13): the Lens and Filmic grain types
// (UBER_GRAIN_LENS, UBER_GRAIN_FILMIC, with UBER_GRAIN), and film damage (UBER_FILM_DAMAGE:
// hairs, scratches, dust, the gate's weave and mask). Adapted from OpenEngine's film-simulation
// package (Packages/film-simulation/Assets/Shaders/film_simulation_grain.frag and
// film_simulation_artifacts.frag):
//
//   MIT License. Copyright (c) 2026 GameEngine contributors
//   The FidelityFX Fast path and the PCG3D16 generator are adapted from AMD FidelityFX Lens 1.1
//   (SDK v1.1.3). MIT License. Copyright (C) 2024 Advanced Micro Devices, Inc.
//   (both notices in full are in NOTICE.md)
//
// Everything is held for a film frame (film.z, the frame index of filmFrameRate the engine
// computes), positions are output pixels from the image's top-left on every backend.
//
// Parameters (u_fs_uberParams): film x grain size (pixels), y 1 coloured grain, z film frame,
// w dust size (pixels); damage x strength, y, z, w hair, scratch and dust candidates (0..8);
// gateWeave xy offset (output pixels), z cos, w sin of the rotation (the engine draws it per
// film frame); gateMask xy half size, z corner radius, w feather (output pixels; x 0: none).

#if defined(UBER_GRAIN_LENS) || defined(UBER_GRAIN_FILMIC)
    highp uvec3 filmPcg3d16(highp uvec3 v){
        v = v * 12829u + 47989u;
        v.x += v.y * v.z;
        v.y += v.z * v.x;
        v.z += v.x * v.y;
        v.x += v.y * v.z;
        v.y += v.z * v.x;
        v.z += v.x * v.y;
        return v >> 16u;
    }
#endif

#ifdef UBER_GRAIN_LENS
    highp vec2 filmSimplexCell(highp vec2 p){
        const highp float F2 = 0.36602540378;
        const highp float G2 = 0.21132486540;
        highp vec2 simplex = floor(p + (p.x + p.y) * F2 + 0.5);
        return p - (simplex - (simplex.x + simplex.y) * G2);
    }

    // FidelityFX Lens's fast grain: simplex cells of about sizePixels, PCG-jittered
    mediump vec3 filmLensGrain(highp vec2 pixel, highp float seed, highp float sizePixels){
        // FidelityFX hashes on scale / 8: the size maps back into that domain
        highp float scale = max(sizePixels * 8.0, 2.0);
        highp uvec3 bits = filmPcg3d16(uvec3(uvec2(pixel / (scale / 8.0)), uint(seed)));
        highp vec2 fine = vec2(bits.xy) * (1.0 / 65536.0) - 0.5;
        highp float grain = 1.0 - 2.0 * exp2(-length(filmSimplexCell(pixel / scale + fine)) * 3.0);
        // its slight dark bias and intensity matched to the filmic grain
        return vec3((grain + 0.10) * 1.60);
    }
#endif

#ifdef UBER_GRAIN_FILMIC
    highp float filmCellHash(highp vec2 p, highp float seed){
        highp uvec3 random = filmPcg3d16(uvec3(uvec2(ivec2(floor(p))), uint(seed)));
        return float(random.x) * (1.0 / 65536.0);
    }

    // a 5x5 neighbourhood of grain particles on a fixed two-pixel lattice, each jittered,
    // round and soft, of the size's footprint (the size changes the grains, not the pattern)
    mediump vec3 filmFilmicGrain(highp vec2 pixel, highp float seed, highp float size, bool colored){
        const highp float SPACING = 2.0;
        highp vec2 base = floor(pixel / SPACING);
        highp float radius = max(size * 0.5, 0.125);
        mediump vec3 sum = vec3(0.0);
        mediump float sumSq = 0.0;
        for (int y = -2; y <= 2; y++){
            for (int x = -2; x <= 2; x++){
                highp vec2 cell = base + vec2(float(x), float(y));
                highp vec2 jitter = vec2(filmCellHash(cell + vec2(5.0, 97.0), seed + 101.0),
                                         filmCellHash(cell + vec2(89.0, 3.0), seed + 127.0)) - 0.5;
                highp vec2 delta = pixel - ((cell + 0.5) * SPACING + jitter);
                mediump float w = 1.0 - smoothstep(0.15, 1.0, length(delta) / radius);
                mediump float mono = filmCellHash(cell + vec2(13.0, 41.0), seed + 17.0) * 2.0 - 1.0;
                mediump vec3 value = colored ? vec3(mono, filmCellHash(cell + vec2(59.0, 7.0), seed + 31.0) * 2.0 - 1.0,
                                                    filmCellHash(cell + vec2(23.0, 73.0), seed + 47.0) * 2.0 - 1.0)
                                             : vec3(mono);
                sum += value * w;
                sumSq += w * w;
            }
        }
        return sum / max(sqrt(sumSq), 1.0);
    }
#endif

#if defined(UBER_GRAIN_LENS) || defined(UBER_GRAIN_FILMIC)
    // the grain on a display colour: banded by luma (the response thins the highlights) and
    // scaled by the distance to the nearest display boundary, so it never clips black or white
    mediump vec3 filmGrain(mediump vec3 color, highp vec2 pixel){
        highp float seed = uber.film.z;
        #ifdef UBER_GRAIN_LENS
            mediump vec3 noise = filmLensGrain(pixel, seed, uber.film.x);
        #else
            mediump vec3 noise = filmFilmicGrain(pixel, seed, uber.film.x, uber.film.y > 0.5);
        #endif
        mediump float luma = clamp(dot(color, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
        mediump float shadows = 1.0 - smoothstep(0.15, 0.45, luma);
        mediump float highlights = smoothstep(0.55, 0.9, luma);
        mediump float midtones = max(1.0 - shadows - highlights, 0.0);
        mediump float r = uber.effects.w;
        mediump float response = shadows + midtones * (1.0 - 0.25 * r) + highlights * (1.0 - 0.8125 * r);
        mediump vec3 envelope = max(min(color, vec3(1.0) - color), vec3(0.0));
        mediump float strength = clamp(uber.effects.z * response * 0.88, 0.0, 1.0);
        return color + clamp(noise, vec3(-1.0), vec3(1.0)) * envelope * strength;
    }
#endif

#ifdef UBER_FILM_DAMAGE
    highp float filmHash11(highp float p){
        p = fract(p * 0.1031);
        p *= p + 33.33;
        p *= p + p;
        return fract(p);
    }

    highp float filmHash21(highp vec2 p){
        highp vec3 p3 = fract(vec3(p.xyx) * 0.1031);
        p3 += dot(p3, p3.yzx + 33.33);
        return fract((p3.x + p3.y) * p3.z);
    }

    // the gate's weave: the frame shifts and turns a little each film frame
    highp vec2 filmGateWeave(highp vec2 uv){
        highp float aspect = uber.outputTexel.z * uber.outputTexel.y;
        highp vec2 p = uv - 0.5;
        p.x *= aspect;
        p = vec2(p.x * uber.gateWeave.z - p.y * uber.gateWeave.w, p.x * uber.gateWeave.w + p.y * uber.gateWeave.z);
        p.x /= aspect;
        return p + 0.5 + uber.gateWeave.xy * uber.outputTexel.xy;
    }

    // candidate i of an amount (0..8) shows when its hash falls under amount - i
    bool filmCandidate(highp float id, mediump float amount, int i){
        return filmHash11(id) < amount - float(i);
    }

    // hairs: curled fibres across the frame (dark)
    mediump float filmHairs(highp vec2 pixel, highp vec2 resolution, highp float seed, mediump float amount){
        mediump float coverage = 0.0;
        for (int i = 0; i < 8; i++){
            if (float(i) >= amount)
                break;
            highp float id = float(i) + seed * 19.0;
            if (!filmCandidate(id + 3.0, amount, i))
                continue;
            highp vec2 centre = vec2(filmHash11(id + 7.0), filmHash11(id + 13.0)) * resolution;
            highp float angle = filmHash11(id + 23.0) * 6.28318530718;
            highp vec2 axis = vec2(cos(angle), sin(angle));
            highp vec2 p = pixel - centre;
            highp vec2 local = vec2(dot(p, axis), dot(p, vec2(-axis.y, axis.x)));
            highp float sizeScale = mix(0.45, 1.0, filmHash11(id + 31.0));
            highp float halfLength = 220.0 * sizeScale * 0.5;
            highp float x = local.x / halfLength;
            highp float curl = 0.25 * filmHash11(id + 47.0) * halfLength * 0.35 * x * x * sign(filmHash11(id + 41.0) - 0.5);
            highp float width = 1.25 * sizeScale;
            mediump float fibre = 1.0 - smoothstep(width, width + 1.25, abs(local.y - curl));
            mediump float ends = 1.0 - smoothstep(0.82, 1.0, abs(x));
            coverage = max(coverage, fibre * ends);
        }
        return coverage;
    }

    // scratches: wandering vertical lines, mostly bright (the base scratched), sometimes dark
    mediump float filmScratches(highp vec2 pixel, highp vec2 resolution, highp float seed, mediump float amount, out mediump float polarity){
        polarity = 1.0;
        mediump float coverage = 0.0;
        for (int i = 0; i < 8; i++){
            if (float(i) >= amount)
                break;
            highp float id = float(i) + seed * 29.0;
            if (!filmCandidate(id + 5.0, amount, i))
                continue;
            highp float x = filmHash11(id + 11.0) * resolution.x;
            highp float localY = pixel.y - filmHash11(id + 17.0) * resolution.y;
            highp float halfLength = resolution.y * 0.65 * mix(0.35, 0.5, filmHash11(id + 19.0));
            highp float wander = x + sin(localY * 0.025 + filmHash11(id + 31.0) * 6.28318530718) * mix(0.15, 1.5, filmHash11(id + 37.0));
            mediump float line = 1.0 - smoothstep(0.8, 1.8, abs(pixel.x - wander));
            mediump float candidate = line * (1.0 - smoothstep(halfLength * 0.82, halfLength, abs(localY)));
            if (candidate > coverage){
                coverage = candidate;
                polarity = filmHash11(id + 43.0) < 0.78 ? 1.0 : -1.0;
            }
        }
        return coverage;
    }

    // dust: irregular specks, mostly dark (on the print), sometimes bright (on the negative)
    mediump float filmDust(highp vec2 pixel, highp vec2 resolution, highp float seed, mediump float amount, out mediump float polarity){
        polarity = 1.0;
        mediump float coverage = 0.0;
        for (int i = 0; i < 8; i++){
            if (float(i) >= amount)
                break;
            highp float id = float(i) + seed * 53.0;
            if (!filmCandidate(id + 9.0, amount, i))
                continue;
            highp vec2 p = pixel - vec2(filmHash11(id + 15.0), filmHash11(id + 21.0)) * resolution;
            highp float radius = max(uber.film.w * mix(0.35, 1.0, filmHash11(id + 27.0)), 0.5);
            // the outline's radius varies by a per-speck hash of the quantised angle: blobs
            highp float lobe = filmHash21(vec2(floor(atan(p.y, p.x) * 1.90985), id));
            highp float wobbled = radius * mix(0.65, 1.0, lobe);
            mediump float speck = 1.0 - smoothstep(wobbled, wobbled + 1.25, length(p));
            if (speck > coverage){
                coverage = speck;
                polarity = filmHash11(id + 33.0) < 0.7 ? 1.0 : -1.0;
            }
        }
        return coverage;
    }

    // hairs, scratches and dust of this film frame on a display colour
    mediump vec3 filmDamage(mediump vec3 color, highp vec2 pixel){
        highp vec2 resolution = uber.outputTexel.zw;
        highp float seed = uber.film.z;
        mediump float strength = uber.damage.x;
        mediump float polarity;
        if (uber.damage.y > 0.0)
            color *= 1.0 - filmHairs(pixel, resolution, seed, uber.damage.y) * clamp(0.3 * strength, 0.0, 1.0);
        if (uber.damage.z > 0.0){
            mediump float scratch = filmScratches(pixel, resolution, seed, uber.damage.z, polarity) * clamp(0.35 * strength, 0.0, 1.0);
            color = polarity > 0.0 ? mix(color, vec3(1.0), scratch) : color * (1.0 - scratch);
        }
        if (uber.damage.w > 0.0){
            mediump float dust = filmDust(pixel, resolution, seed, uber.damage.w, polarity) * clamp(0.25 * strength, 0.0, 1.0);
            color = polarity > 0.0 ? color * (1.0 - dust) : mix(color, vec3(1.0), dust);
        }
        return color;
    }

    // the gate mask: a rounded box of the gate's aspect, feathered (1 inside)
    mediump float filmGateMask(highp vec2 pixel){
        if (uber.gateMask.x <= 0.0)
            return 1.0;
        highp vec2 p = pixel - 0.5 * uber.outputTexel.zw;
        highp vec2 q = abs(p) - uber.gateMask.xy + uber.gateMask.z;
        highp float edge = min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - uber.gateMask.z;
        return 1.0 - smoothstep(-uber.gateMask.w, uber.gateMask.w, edge);
    }
#endif
