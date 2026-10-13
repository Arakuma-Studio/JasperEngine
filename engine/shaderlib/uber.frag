#version 450

// The uber pass (shader library phase 4, tier T1): the scene colour (display-encoded RGBA8)
// to the destination in one fullscreen pass, with the effects its variant compiles in
// (RenderSystem::UberFeature, one define each). In order:
//
//   1. UV warp: CRT curvature (UBER_CRT), pixelate (UBER_PIXELATE, UV snapped to blocks of
//      output pixels)
//   2. the scene tap, or FXAA 3.11's console algorithm (UBER_FXAA: luma from green, the 4
//      corner taps, 4 more on edge pixels), then sharpen (UBER_SHARPEN, from the same corners)
//   3. chromatic aberration (UBER_CHROMATIC): red and blue taps shifted along the radius, added
//      as their difference from the centre tap (FXAA and sharpen stay on every channel)
//   4. bloom (UBER_BLOOM: u_bloomTexture, added in linear light with the gamma-2 approximation
//      sqrt(c*c + bloom), exact where there is none) and blur (UBER_BLUR: u_blurTexture mixed in)
//   5. the colour grade's LUT (UBER_LUT: display colour in and out, includes/post_color.glsl's
//      strip layout)
//   6. edge detect (UBER_EDGE: Sobel on luma from the 4 corner taps, which are a Sobel kernel
//      exactly; source texels, so aspect-correct), vignette (UBER_VIGNETTE)
//   7. CRT scanlines, aperture mask, tube vignette (UBER_CRT), film grain (UBER_GRAIN), dither
//      (UBER_DITHER, last)
//
// Colour maths is mediump (fp16 on GLES where the GPU has it); UVs and texel sizes stay highp
// (the parameter block too: GL uploads it as one vec4 array).
// The source is always sampled bilinear (the corner taps and the FXAA search rely on it): the
// engine binds a linear sampler when the fixed-resolution target is NEAREST, and the UV snaps
// to the centre of its source texel (sampling.x 1), so that upscale stays nearest and every
// effect works per source texel. UBER_UPSCALE (none of the effects) is the plain upscale of a
// render scale below 1.
// The bloom and blur textures are the phase 4 bloom chain's (SRGB8A8: linear when sampled, in
// the gamma-2 domain the chain's prefilter decoded the scene to); with none bound the engine
// binds black (bloom adds nothing) and the source with a blur mix of 0.

#if defined(UBER_FXAA) || defined(UBER_SHARPEN) || defined(UBER_EDGE)
    #define UBER_CORNERS
#endif
#if defined(UBER_PIXELATE) || defined(UBER_CRT)
    #define UBER_WARP
#endif

in highp vec2 v_texcoord;
in highp vec2 v_screen;
#if defined(UBER_CORNERS) && !defined(UBER_WARP)
    in highp vec4 v_corners;
#endif

out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;
#ifdef UBER_LUT
    uniform texture2D u_colorLut;
    uniform sampler u_colorLut_smp;
#endif
#ifdef UBER_BLOOM
    uniform texture2D u_bloomTexture;
    uniform sampler u_bloom_smp;
#endif
#ifdef UBER_BLUR
    uniform texture2D u_blurTexture;
    uniform sampler u_blur_smp;
#endif

uniform u_fs_uberParams {
    vec4 uvScaleOffset;       // source UV = screen * xz + yw
    vec4 sourceTexel;         // xy 1 / source size, zw source size
    vec4 outputTexel;         // xy 1 / output viewport size, zw its size
    vec4 effects;                   // x chromatic aberration, y pixel block (output pixels), z grain, w grain response
    vec4 crt;                       // x scanlines, y mask, z curvature, w tube vignette
    vec4 edgeSharpenTime;     // x edge threshold, y edge intensity, z sharpen, w time (seconds)
    vec4 edgeColorDither;           // rgb edge colour (display), a dither amplitude
    vec4 vignetteScaleOffset; // p = gl_FragCoord.xy * xy - zw (includes/post_color.glsl)
    vec4 vignetteColor;             // rgb display colour, a intensity
    vec4 vignetteLutScan;     // x, y vignette falloff, z LUT side, w scanline period (output pixels)
    vec4 bloomBlur;                 // rgb bloom tint * intensity (linear), a blur mix
    vec4 sampling;            // x 1: snap the UV to source texel centres (a NEAREST upscale)
} uber;

mediump vec3 sceneTap(highp vec2 uv){
    return texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv).rgb;
}

#ifdef UBER_FXAA
    // FXAA 3.11, console path: the corner lumas decide whether the pixel is on an edge (local
    // contrast over 1/8 of the brightest, at least 0.05); on one, two taps half a texel along
    // the edge direction and two up to two texels further, the wide pair dropped when it
    // leaves the corners' luma range
    mediump vec3 fxaa(highp vec2 uv, mediump vec3 rgbM, mediump float lumaNw, mediump float lumaNe,
                      mediump float lumaSw, mediump float lumaSe){
        const mediump float EDGE_SHARPNESS = 8.0;
        const mediump float EDGE_THRESHOLD = 0.125;
        const mediump float EDGE_THRESHOLD_MIN = 0.05;
        mediump float lumaM = rgbM.g;
        lumaNe += 1.0 / 384.0;
        mediump float lumaMax = max(max(lumaNw, lumaSw), max(lumaNe, lumaSe));
        mediump float lumaMin = min(min(lumaNw, lumaSw), min(lumaNe, lumaSe));
        mediump float range = max(lumaMax, lumaM) - min(lumaMin, lumaM);
        if (range < max(EDGE_THRESHOLD_MIN, lumaMax * EDGE_THRESHOLD))
            return rgbM;

        mediump float swMinusNe = lumaSw - lumaNe;
        mediump float seMinusNw = lumaSe - lumaNw;
        mediump vec2 dir1 = normalize(vec2(swMinusNe + seMinusNw, swMinusNe - seMinusNw) + vec2(1e-5, 0.0));
        highp vec2 near = dir1 * 0.5 * uber.sourceTexel.xy;
        mediump vec3 rgbA = sceneTap(uv - near) + sceneTap(uv + near);

        mediump float dirAbsMin = min(abs(dir1.x), abs(dir1.y)) * EDGE_SHARPNESS;
        mediump vec2 dir2 = clamp(dir1 / max(dirAbsMin, 1e-4), -2.0, 2.0);
        highp vec2 far = dir2 * 2.0 * uber.sourceTexel.xy;
        mediump vec3 rgbB = (sceneTap(uv - far) + sceneTap(uv + far)) * 0.25 + rgbA * 0.25;
        if (rgbB.g < lumaMin || rgbB.g > lumaMax)
            return rgbA * 0.5;
        return rgbB;
    }
#endif

#ifdef UBER_LUT
    // the grade's strip LUT of side n (display colour in and out)
    mediump vec3 gradeLut(mediump vec3 color, highp float n){
        highp float slice = clamp(color.b, 0.0, 1.0) * (n - 1.0);
        highp float base = floor(slice);
        highp vec2 rg = clamp(color.rg, 0.0, 1.0) * (n - 1.0) + 0.5;
        highp vec2 uv = vec2((rg.x + base * n) / (n * n), rg.y / n);
        mediump vec3 a = texture(sampler2D(u_colorLut, u_colorLut_smp), uv).rgb;
        mediump vec3 b = texture(sampler2D(u_colorLut, u_colorLut_smp), uv + vec2(1.0 / n, 0.0)).rgb;
        return mix(a, b, slice - base);
    }
#endif

// interleaved gradient noise in 0..1 (stable per pixel)
highp float uberNoise(highp vec2 pixel){
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main(){
    highp vec2 screen = v_screen;
    highp vec2 uv = v_texcoord;
    #ifdef UBER_CRT
        // barrel curvature of the tube: points move out by the square of the other axis
        highp vec2 p = screen * 2.0 - 1.0;
        p *= 1.0 + uber.crt.z * 0.25 * (p.yx * p.yx);
        screen = p * 0.5 + 0.5;
        mediump float inside = step(0.0, screen.x) * step(screen.x, 1.0) * step(0.0, screen.y) * step(screen.y, 1.0);
    #endif
    #ifdef UBER_PIXELATE
        highp vec2 block = uber.effects.y * uber.outputTexel.xy;
        screen = (floor(screen / block) + 0.5) * block;
    #endif
    #ifdef UBER_WARP
        uv = screen * uber.uvScaleOffset.xz + uber.uvScaleOffset.yw;
    #endif
    // a NEAREST upscale: the centre of the source texel (bilinear there is that texel)
    highp vec2 snapped = (floor(uv * uber.sourceTexel.zw) + 0.5) * uber.sourceTexel.xy;
    highp vec2 snap = (snapped - uv) * uber.sampling.x;
    uv += snap;

    #ifdef UBER_CORNERS
        #ifdef UBER_WARP
            highp vec2 halfTexel = 0.5 * uber.sourceTexel.xy;
            highp vec4 corners = vec4(uv - halfTexel, uv + halfTexel);
        #else
            highp vec4 corners = v_corners + snap.xyxy;
        #endif
        mediump vec3 nw = sceneTap(corners.xy);
        mediump vec3 ne = sceneTap(corners.zy);
        mediump vec3 sw = sceneTap(corners.xw);
        mediump vec3 se = sceneTap(corners.zw);
    #endif

    mediump vec4 sceneColor = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv);
    mediump vec3 color = sceneColor.rgb;

    #ifdef UBER_FXAA
        color = fxaa(uv, color, nw.g, ne.g, sw.g, se.g);
    #endif
    #ifdef UBER_SHARPEN
        // the corners average a 3x3 tent around the pixel: push away from it
        color = max(color + (color - (nw + ne + sw + se) * 0.25) * (2.0 * uber.edgeSharpenTime.z), vec3(0.0));
    #endif

    #ifdef UBER_CHROMATIC
        // red in, blue out along the radius, growing with its square (none at the centre)
        highp vec2 radius = screen - 0.5;
        highp vec2 shift = radius * dot(radius, radius) * (0.06 * uber.effects.x) * uber.uvScaleOffset.xz;
        // the shifted taps' difference from the centre tap, on top of the antialiased and
        // sharpened colour (a raw tap would bring the jaggies back on red and blue)
        color.r = max(color.r + (sceneTap(uv - shift).r - sceneColor.r), 0.0);
        color.b = max(color.b + (sceneTap(uv + shift).b - sceneColor.b), 0.0);
    #endif

    #ifdef UBER_BLOOM
        mediump vec3 bloom = texture(sampler2D(u_bloomTexture, u_bloom_smp), uv).rgb;
        color = sqrt(color * color + bloom * uber.bloomBlur.rgb);
    #endif
    #ifdef UBER_BLUR
        // the chain's colour is linear (gamma 2, as its prefilter decoded it)
        mediump vec3 blurred = sqrt(texture(sampler2D(u_blurTexture, u_blur_smp), uv).rgb);
        color = mix(color, blurred, uber.bloomBlur.a);
    #endif

    #ifdef UBER_LUT
        color = gradeLut(color, uber.vignetteLutScan.z);
    #endif

    #ifdef UBER_EDGE
        // Sobel on green: the corner taps' sums are the 3x3 Sobel kernels over 4
        mediump float gx = (ne.g + se.g) - (nw.g + sw.g);
        mediump float gy = (sw.g + se.g) - (nw.g + ne.g);
        mediump float threshold = uber.edgeSharpenTime.x;
        mediump float edge = smoothstep(threshold, threshold * 2.0 + 0.05, length(vec2(gx, gy)));
        color = mix(color, uber.edgeColorDither.rgb, edge * uber.edgeSharpenTime.y);
    #endif

    #ifdef UBER_VIGNETTE
        highp vec2 vp = gl_FragCoord.xy * uber.vignetteScaleOffset.xy - uber.vignetteScaleOffset.zw;
        mediump float w = clamp(dot(vp, vp) * uber.vignetteLutScan.x + uber.vignetteLutScan.y, 0.0, 1.0);
        color = mix(color, uber.vignetteColor.rgb, w * w * (3.0 - 2.0 * w) * uber.vignetteColor.a);
    #endif

    #ifdef UBER_CRT
        // scanlines: a dark line per source row (or every few output pixels)
        mediump float scan = 0.5 + 0.5 * cos(6.2831853 * gl_FragCoord.y / uber.vignetteLutScan.w);
        color *= 1.0 - uber.crt.x * (1.0 - scan);
        // aperture grille: each output column favours one primary
        mediump float column = mod(floor(gl_FragCoord.x), 3.0);
        mediump vec3 mask = vec3(column < 0.5 ? 1.0 : 0.6, (column > 0.5 && column < 1.5) ? 1.0 : 0.6, column > 1.5 ? 1.0 : 0.6) * 1.25;
        color *= mix(vec3(1.0), mask, uber.crt.y);
        // the tube's edges darken, nothing outside the curved screen
        mediump vec2 t = clamp(screen, 0.0, 1.0);
        mediump float tube = pow(clamp(16.0 * t.x * (1.0 - t.x) * t.y * (1.0 - t.y), 0.0, 1.0), 0.25);
        color *= mix(1.0, tube, uber.crt.w) * inside;
    #endif

    #ifdef UBER_GRAIN
        // a new pattern every frame, less of it on bright pixels by the response
        highp vec2 grainPixel = gl_FragCoord.xy + fract(uber.edgeSharpenTime.w * vec2(13.7, 7.3)) * 1024.0;
        highp float noise = fract(sin(dot(grainPixel, vec2(12.9898, 78.233))) * 43758.5453);
        mediump float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
        color += (noise - 0.5) * uber.effects.z * 0.5 * mix(1.0, 1.0 - luma, uber.effects.w);
    #endif

    #ifdef UBER_DITHER
        color += (uberNoise(gl_FragCoord.xy) - 0.5) * uber.edgeColorDither.a;
    #endif

    frag_color = vec4(color, sceneColor.a);
}
