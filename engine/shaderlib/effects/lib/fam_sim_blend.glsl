// HTML UI effects (phase 6): the kit's blend modes with their straight-alpha "over" tail, ported
// from shaders.com's gpu/kit/blend.ts (MIT, Copyright (c) 2026 Shader Effects Inc.; see
// effects/LICENSE.shaders-com.txt): base and overlay straight-alpha linear colours, opacity on the
// overlay; the result straight. Modes (TimeTrail's list): 0 normal, 1 add (linearDodge), 2 screen,
// 3 multiply, 4 overlay, 5 soft-light, 6 hard-light, 7 darken, 8 lighten, 9 color-dodge,
// 10 color-burn, 11 linear-burn, 12 difference, 13 exclusion, 14 hue, 15 saturation, 16 color,
// 17 luminosity.
#ifndef FAM_SIM_BLEND_GLSL
#define FAM_SIM_BLEND_GLSL

float fam_blend_luminance(vec3 rgb){
    return rgb.x * 0.2126 + rgb.y * 0.7152 + rgb.z * 0.0722;
}

// overComposite: alpha = a_o + a_b (1 - a_o), rgb = blended a_o + base a_b (1 - a_o) (premultiplied sum)
vec4 fam_over_composite(vec4 base, vec3 blended, float overlayAlpha){
    float baseWeight = base.w * (1.0 - overlayAlpha);
    float finalAlpha = overlayAlpha + baseWeight;
    vec3 rgb = blended * overlayAlpha + base.xyz * baseWeight;
    return vec4(rgb, finalAlpha);
}

// the kit modes return the premultiplied-like sum of overComposite; the caller unpremultiplies
vec4 fam_blend_mode(vec4 base, vec4 overlay, float opacity, int mode){
    float a = overlay.w * opacity;
    vec3 b = base.xyz;
    vec3 o = overlay.xyz;
    vec3 blended = o;
    if (mode == 1){
        blended = b + o;
    }else if (mode == 2){
        blended = 1.0 - (1.0 - b) * (1.0 - o);
    }else if (mode == 3){
        blended = b * o;
    }else if (mode == 4){
        blended = mix(b * o * 2.0, 1.0 - (1.0 - b) * (1.0 - o) * 2.0, step(vec3(0.5), b));
    }else if (mode == 5){
        vec3 softDark = b * o * 2.0 + b * b * (1.0 - o * 2.0);
        vec3 softLight = b * (1.0 - o) * 2.0 + sqrt(max(b, vec3(0.0))) * (o * 2.0 - 1.0);
        blended = mix(softDark, softLight, step(vec3(0.5), o));
    }else if (mode == 6){
        blended = mix(b * o * 2.0, 1.0 - (1.0 - b) * (1.0 - o) * 2.0, step(vec3(0.5), o));
    }else if (mode == 7){
        blended = min(b, o);
    }else if (mode == 8){
        blended = max(b, o);
    }else if (mode == 9){
        vec3 oneMinus = 1.0 - o;
        blended = mix(b, min(b / max(oneMinus, vec3(1e-6)), vec3(1.0)), step(vec3(0.001), oneMinus));
    }else if (mode == 10){
        vec3 burned = max(1.0 - (1.0 - b) / (o + 0.0001), vec3(0.0));
        blended = mix(vec3(0.0), burned, step(vec3(0.0001), o));
    }else if (mode == 11){
        blended = max(b + o - 1.0, vec3(0.0));
    }else if (mode == 12){
        blended = abs(b - o);
    }else if (mode == 13){
        blended = b + o - b * o * 2.0;
    }else if (mode == 14){
        float baseLum = fam_blend_luminance(b);
        float overlayLum = fam_blend_luminance(o);
        vec3 baseChroma = b - baseLum;
        vec3 overlayChroma = o - overlayLum;
        float overlayLen = length(overlayChroma);
        vec3 dir = overlayLen > 0.0001 ? overlayChroma / overlayLen : vec3(0.0);
        blended = dir * length(baseChroma) + baseLum;
    }else if (mode == 15){
        float baseLum = fam_blend_luminance(b);
        float overlayLum = fam_blend_luminance(o);
        vec3 baseChroma = b - baseLum;
        float baseLen = length(baseChroma);
        vec3 dir = baseLen > 0.0001 ? baseChroma / baseLen : vec3(0.0);
        blended = dir * length(o - overlayLum) + baseLum;
    }else if (mode == 16){
        blended = o + (fam_blend_luminance(b) - fam_blend_luminance(o));
    }else if (mode == 17){
        blended = b * (fam_blend_luminance(o) / (fam_blend_luminance(b) + 0.0001));
    }
    return fam_over_composite(base, blended, a);
}

#endif // FAM_SIM_BLEND_GLSL
