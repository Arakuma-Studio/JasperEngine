// The Title Art Renderer's live layers (docs/specs/S81-title-art-runtime.md R33-R38), shared by
// library:title_fx (UI) and library:title_fx_sprite (world): the companion's one draw adds them
// to the title's image, so a live layer costs no pass and no draw. Every input is a uniform the
// system writes from the component (Live Time included): nothing reads engine time, so a clip
// scrubbing liveTime or a --fixed-fps run gives the same pixels every time.
//
// The including shader declares, inside u_fs_customParams (customParams), the members of the
// features it enables (title_fx.frag lists them), and for GLOW / CROSSFADE the textures
// u_glowTexture / u_prevTexture. Colours are linear and straight alpha; `uv` is the frame's
// 0..1, y down (the bake's and the display target's rows: row 0 is the top).
//
// The sweep (Pachislop src/effects/a/stylize.ts, the band of uLen centred at uPos) and the
// highlight twinkle are ported from Pachislop by SafEight, used with the author's permission.

#ifndef TITLE_FX_GLSL
#define TITLE_FX_GLSL

#include "includes/srgb.glsl"

// the light the live layers add over the image: premultiplied colour (rgb) and coverage (a)
struct TitleFxLight {
    vec3 color;
    float coverage;
};

// Hoskins' "Hash without Sine" (MIT): a per-anchor phase from the seed and the index
float titleFxHash(float seed, float index){
    vec2 p = vec2(seed * 0.1031 + 0.123, index * 0.11369 + 0.456);
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// a smooth bump over the first quarter of each period (duty 0.25), 0 the rest of it
float titleFxTwinkle(float t){
    float f = fract(t);
    float x = clamp(f / 0.25, 0.0, 1.0);
    float bump = sin(3.14159265 * x);
    return f < 0.25 ? bump * bump : 0.0;
}

#ifdef SWEEP
    // R34: a band across the frame along the angle (deg clockwise from +x, y down); position
    // 0 and 1 are fully off the frame, the width a fraction of the frame's extent along the
    // angle. It adds light inside the image's alpha only (straight colour: the blend's alpha
    // keeps it inside the shape).
    vec3 titleFxSweep(vec2 uv, vec2 frameSize, vec3 color){
        float position = customParams.sweepParams.x;
        float width = max(customParams.sweepParams.y, 1e-4);
        float angle = radians(customParams.sweepParams.z);
        float softness = clamp(customParams.sweepParams.w, 0.0, 1.0);
        vec2 dir = vec2(cos(angle), sin(angle));
        vec2 p = (uv - 0.5) * frameSize;
        float extent = abs(frameSize.x * dir.x) + abs(frameSize.y * dir.y);
        float s = dot(p, dir) / max(extent, 1e-4);
        float halfWidth = width * 0.5;
        float center = mix(-0.5 - halfWidth, 0.5 + halfWidth, position);
        float inner = min(halfWidth * (1.0 - softness), halfWidth * 0.999);
        float band = 1.0 - smoothstep(inner, halfWidth, abs(s - center));
        return color + customParams.sweepColor.rgb * (customParams.sweepColor.a * band);
    }
#endif

#ifdef TINT
    // the overlay blend ui_fx uses: the tint colours the mid tones, the shading stays
    vec3 titleFxTint(vec3 color){
        vec3 base = linearTosRGB(color);
        vec3 tint = linearTosRGB(customParams.tintColor.rgb);
        vec3 overlay = mix(2.0 * base * tint, 1.0 - 2.0 * (1.0 - base) * (1.0 - tint), step(0.5, base));
        return mix(color, sRGBToLinear(overlay), customParams.tintParams.x * customParams.tintColor.a);
    }
#endif

#ifdef GLOW
    // R35: the bake's (or the render's) glow output, written as light (straight colour, alpha
    // the largest channel), times the glow colour and the pulse
    void titleFxGlow(vec4 glowTexel, inout TitleFxLight light){
        vec3 glow = sRGBToLinear(glowTexel.rgb) * glowTexel.a;
        float pulse = customParams.glowParams.x * customParams.glowColor.a;
        vec3 added = glow * customParams.glowColor.rgb * pulse;
        light.color += added;
        light.coverage = max(light.coverage, clamp(max(added.r, max(added.g, added.b)), 0.0, 1.0));
    }
#endif

#ifdef GLINTS
    // R37: a four-point star at each anchor (x, y, size, strength x fade), twinkling with the
    // Live Time; drawn analytically, at most 16
    void titleFxGlints(vec2 uv, vec2 frameSize, inout TitleFxLight light){
        float count = customParams.glintInfo.x;
        float liveTime = customParams.glintInfo.y;
        float amount = customParams.glintParams.x;
        float sizeFraction = customParams.glintParams.y;
        float rate = customParams.glintParams.z;
        float seed = customParams.glintParams.w;
        vec2 p = uv * frameSize;
        vec3 sum = vec3(0.0);
        for (int i = 0; i < 16; i++){
            if (float(i) >= count) break;
            vec4 anchor = customParams.anchors[i];
            float radius = max(sizeFraction * anchor.z * frameSize.y, 0.5);
            vec2 d = (p - anchor.xy * frameSize) / radius;
            float phase = titleFxHash(seed, float(i));
            float brightness = amount * anchor.w * titleFxTwinkle(liveTime * rate + phase);
            // a soft core and two thin rays
            float core = max(0.0, 1.0 - length(d));
            float rayX = max(0.0, 1.0 - abs(d.x)) * max(0.0, 1.0 - abs(d.y) * 10.0);
            float rayY = max(0.0, 1.0 - abs(d.y)) * max(0.0, 1.0 - abs(d.x) * 10.0);
            float star = core * core * core + rayX * rayX + rayY * rayY;
            sum += customParams.glintColor.rgb * (customParams.glintColor.a * brightness * star);
        }
        light.color += sum;
        light.coverage = max(light.coverage, clamp(max(sum.r, max(sum.g, sum.b)), 0.0, 1.0));
    }
#endif

#ifdef ANIM
    // S85's companion animation: a 2x3 affine in frame space about the pivot (the inverse the
    // system writes: frame uv -> source uv) and the texture rects of a flipbook's two frames
    vec2 titleFxAnimUV(vec2 uv){
        vec3 q = vec3(uv, 1.0);
        return vec2(dot(customParams.animAffine0.xyz, q), dot(customParams.animAffine1.xyz, q));
    }
#endif

// the image with its live layers: straight-alpha linear colour in, out
vec4 titleFxCompose(vec4 color, TitleFxLight light){
    // the light goes over the image as its own coverage (glows and stars reach past the shape)
    vec3 premultiplied = color.rgb * color.a + light.color;
    float alpha = clamp(color.a + light.coverage * (1.0 - color.a), 0.0, 1.0);
    return vec4(premultiplied / max(alpha, 1e-5), alpha);
}

#endif
