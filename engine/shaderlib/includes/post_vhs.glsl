// VHS tape for the uber pass (UBER_VHS, uber.frag; S13). Adapted from punikonta's Godot VHS
// shader as OpenEngine ported it (Engine/Modules/Rendering/Shaders/vhs.frag, with the maths of
// vhs_interference.frag and vhs_signal_glitches.frag folded in as two strengths):
//
//   https://github.com/punikonta/godot-shader-crt-vhs, revision 6ba87f95c152ec023fc07c4f77e87bdd70783afa
//   MIT License. Copyright (c) Marcel Jovic <punikonta@protonmail.com>
//   Portions Copyright (c) 2026 Gamecrafters Guild Limited and contributors (MIT)
//   (the notices are in NOTICE.md)
//
// Three stages of the uber pass's order:
//   vhsWarp   step 1: wow and flutter, tracking bands, head switching, a crease, interference
//             slips and glitch slips move the line's source UV
//   vhsTaps   step 2, replacing the scene tap: 9 taps (the undisturbed source, the centre, 4
//             along the line and 2 across it for the colour bleed, the line above for dropout
//             concealment) through YIQ: the narrow chroma bleeds and trails, its phase wanders,
//             streaks lose chroma lock, the head-switch rows lose chroma and gain, dropouts
//             conceal with the line above; mixed with the source by the intensity
//   vhsNoise  step 7, before the film grain: tape noise, gain drift, interference hum and lines,
//             scanlines of a 480-line display
// The folded stages read the source, not each other's output. Frame feedback, field history,
// the composite signal and the on-screen display are not part of it.
//
// Parameters (u_fs_uberParams): vhs0 x intensity, y wobble, z tracking, w colour bleed;
// vhs1 x tape noise, y chroma streaks, z dropouts, w scanlines; vhs2 x interference,
// y glitches, z the tape's time in seconds (speed applied, RenderSystem::renderUber).
// Positions are the source UV, y down from the image's top on every backend.

const highp float VHS_PI = 3.14159265359;

highp float vhsHash(highp vec2 p){
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453123);
}

highp float vhsSmoothNoise(highp float x){
    highp float i = floor(x);
    highp float f = fract(x);
    highp float u = f * f * (3.0 - 2.0 * f);
    return mix(vhsHash(vec2(i)), vhsHash(vec2(i + 1.0)), u);
}

// the glitches' own smooth noise (a second row of the hash)
highp float vhsSmoothNoiseRow(highp float x){
    highp float i = floor(x);
    highp float f = fract(x);
    highp float u = f * f * (3.0 - 2.0 * f);
    return mix(vhsHash(vec2(i, 17.0)), vhsHash(vec2(i + 1.0, 17.0)), u);
}

highp float vhsValueNoise(highp vec2 p){
    highp vec2 cell = floor(p);
    highp vec2 f = fract(p);
    highp vec2 blend = f * f * (3.0 - 2.0 * f);
    highp float bottom = mix(vhsHash(cell), vhsHash(cell + vec2(1.0, 0.0)), blend.x);
    highp float top = mix(vhsHash(cell + vec2(0.0, 1.0)), vhsHash(cell + vec2(1.0, 1.0)), blend.x);
    return mix(bottom, top, blend.y);
}

highp float vhsWrappedDistance(highp float a, highp float b){
    highp float d = abs(a - b);
    return min(d, 1.0 - d);
}

mediump vec3 vhsRgbToYiq(mediump vec3 rgb){
    return vec3(dot(rgb, vec3(0.299, 0.587, 0.114)),
                dot(rgb, vec3(0.596, -0.274, -0.322)),
                dot(rgb, vec3(0.211, -0.523, 0.312)));
}

mediump vec3 vhsYiqToRgb(mediump vec3 yiq){
    return vec3(yiq.x + yiq.y * 0.956 + yiq.z * 0.621,
                yiq.x - yiq.y * 0.272 - yiq.z * 0.647,
                yiq.x - yiq.y * 1.106 + yiq.z * 1.703);
}

// what the warp hands to the taps and the noise
struct VhsState {
    highp vec2 sourceUv;        // the line's UV before the tape moved it
    highp float time;           // tape time (s)
    highp float frameTime;      // video frame (29.97 Hz)
    highp float fieldTime;      // video field (59.94 Hz)
    mediump float headMask;     // the head-switch rows at the bottom
    mediump float crease;       // the crease travelling through the tape
    mediump float echo;         // the glitches' displaced echo
    mediump float echoSide;     // which side the echo reads (1: before the line)
    mediump float lineFlash;    // the interference's bright lines
};

// step 1: the line's UV through the tape's transport (and the folded slips)
highp vec2 vhsWarp(highp vec2 uv, out VhsState s){
    highp vec2 pixel = uber.sourceTexel.xy;
    highp vec2 sizePx = uber.sourceTexel.zw;
    highp float time = uber.vhs2.z;
    mediump float wobble = uber.vhs0.y;
    mediump float tracking = uber.vhs0.z;
    s.sourceUv = uv;
    s.time = time;
    s.frameTime = floor(time * 29.97);
    s.fieldTime = floor(time * 59.94);
    highp float fieldParity = mod(s.fieldTime, 2.0);

    // independent slow and fast tape motion: wow and flutter
    highp float wow = vhsSmoothNoise(uv.y * 4.0 + time * 2.0) * 2.0 - 1.0;
    highp float flutter = vhsSmoothNoise(uv.y * 12.0 - time * 4.0) * 2.0 - 1.0;
    highp float shift = (wow + flutter * 0.4) * 0.002 * wobble;

    // tracking: a slow bend, a soft rolling band and a narrow frame-held tear
    highp float trackingRow = floor(uv.y * sizePx.y * 0.25);
    highp float trackingTick = floor(time * 12.0);
    highp float rowJitter = vhsHash(vec2(trackingRow, trackingTick)) * 2.0 - 1.0;
    highp float broadJitter = vhsSmoothNoise(uv.y * 7.0 - time * 1.3) * 2.0 - 1.0;
    highp float bandA = smoothstep(0.026, 0.0, vhsWrappedDistance(uv.y, fract(0.84 - time * 0.071)));
    highp float bandB = smoothstep(0.0045, 0.0, vhsWrappedDistance(uv.y, fract(0.31 + time * 0.043)));
    highp float activityA = smoothstep(0.38, 0.72, vhsSmoothNoise(time * 0.41 + 31.0));
    highp float activityB = smoothstep(0.50, 0.80, vhsSmoothNoise(time * 0.29 + 47.0));
    shift += (vhsSmoothNoise(uv.y * 2.8 - time * 0.37) * 2.0 - 1.0) * 0.0007 * tracking;
    shift += (broadJitter * bandA * activityA * 0.0015 + rowJitter * bandB * activityB * 0.0030) * tracking;

    // head switching at the bottom of the picture: frame-held steps, a softly uneven edge
    highp float headEdge = 0.987 + (vhsSmoothNoise(uv.x * 5.0 + trackingTick * 0.11) - 0.5) * 0.004;
    s.headMask = smoothstep(headEdge, min(headEdge + 0.010, 0.999), uv.y);
    highp float headNoise = vhsSmoothNoise(uv.y * 31.0 + floor(time * 9.0) * 0.37);
    highp float headSkew = vhsHash(vec2(floor(time * 2.5), 19.0)) - 0.5;
    shift += ((headNoise - 0.5) * 0.65 + headSkew * 0.35) * 0.028 * s.headMask * tracking;

    highp vec2 tapeUv = uv + vec2(shift, 0.0);
    tapeUv.y += (fieldParity - 0.5) * pixel.y * 0.18 * tracking;
    tapeUv.y -= s.headMask * (0.001 + headNoise * 0.003) * tracking;

    // a narrow crease travelling through the tape displaces and dims its rows
    highp float creaseEnvelope = smoothstep(0.011, 0.0, abs(uv.y - fract(0.18 + time * 0.11)));
    highp float creaseEdge = vhsHash(vec2(floor(uv.y * sizePx.y * 0.35), floor(time * 7.0)));
    highp float creaseActivity = smoothstep(0.42, 0.74, vhsSmoothNoise(time * 0.23 + 8.0));
    s.crease = creaseEnvelope * mix(0.35, 1.0, creaseEdge) * creaseActivity * tracking;
    tapeUv.x += (creaseEdge - 0.5) * pixel.x * 7.0 * s.crease;

    // interference: two thin lines rolling through the picture slip their rows sideways
    s.lineFlash = 0.0;
    mediump float interference = uber.vhs2.x;
    if (interference > 0.0){
        highp float frame = s.frameTime;
        highp float thinA = 1.0 - smoothstep(pixel.y * 0.8, pixel.y * 2.4, abs(uv.y - fract(0.13 + time * 0.083)));
        highp float thinB = 1.0 - smoothstep(pixel.y * 1.2, pixel.y * 3.5, abs(uv.y - fract(0.71 - time * 0.047)));
        highp float gateA = step(0.42, vhsHash(vec2(floor(frame / 3.0), 11.0)));
        highp float gateB = step(0.62, vhsHash(vec2(floor(frame / 5.0), 23.0)));
        s.lineFlash = thinA * gateA + thinB * gateB * 0.65;
        tapeUv.x += (vhsHash(vec2(frame, floor(uv.y * sizePx.y))) - 0.5) * pixel.x * 2.5 * s.lineFlash * interference;
    }

    // glitches: narrow groups of lines slip sideways, and three broad slices stretch out of
    // place for a moment
    s.echo = 0.0;
    s.echoSide = 1.0;
    mediump float glitches = uber.vhs2.y;
    if (glitches > 0.0){
        highp float tick = floor(time * 11.0);
        highp float row = floor(uv.y * sizePx.y / 3.0);
        highp float gate = step(1.0 - min(0.022 * sqrt(glitches), 0.055), vhsHash(vec2(row, tick)));
        highp float centre = (row * 3.0 + 1.5) * pixel.y;
        highp float halfHeight = mix(pixel.y * 1.5, pixel.y * 5.0, vhsHash(vec2(row + 29.0, tick)));
        highp float band = 1.0 - smoothstep(halfHeight * 0.38, halfHeight, abs(uv.y - centre));
        highp float edgeNoise = vhsSmoothNoiseRow(uv.y * sizePx.y * 0.31 + tick * 0.73) - 0.5;
        s.echoSide = vhsHash(vec2(row + 47.0, tick)) < 0.5 ? -1.0 : 1.0;
        highp float slipPixels = mix(2.0, 13.0, vhsHash(vec2(row + 61.0, tick)));
        tapeUv.x += s.echoSide * slipPixels * pixel.x * band * gate * min(glitches, 1.6);
        tapeUv.x += edgeNoise * pixel.x * 2.0 * band * gate * glitches;

        highp float offsetStrength = glitches * 0.5;
        highp vec2 offsetUv = vec2(0.0);
        highp float offsetMask = 0.0;
        for (int i = 0; i < 3; i++){
            highp float id = float(i);
            highp float clock = time * mix(0.34, 0.72, vhsHash(vec2(id + 3.0, 91.0))) + vhsHash(vec2(id + 17.0, 33.0)) * 7.0;
            highp float index = floor(clock);
            highp float phase = fract(clock);
            highp float alive = step(0.68 - min(offsetStrength * 0.18, 0.30), vhsHash(vec2(index, id + 101.0)));
            highp float lifetime = smoothstep(0.0, 0.10, phase) * (1.0 - smoothstep(0.72, 1.0, phase)) * alive;
            highp float vertical = vhsHash(vec2(index + 59.0, id + 19.0)) < 0.5 ? -1.0 : 1.0;
            highp float travel = mix(0.025, 0.12, vhsHash(vec2(index + 71.0, id + 29.0)));
            highp float motion = sin(phase * VHS_PI * 2.0 + vhsHash(vec2(index + 83.0, id)) * VHS_PI);
            highp float sliceCentre = clamp(mix(0.12, 0.88, vhsHash(vec2(index + 43.0, id + 7.0))) + vertical * motion * travel, 0.06, 0.94);
            highp float sliceHalf = mix(4.0, 24.0, vhsHash(vec2(index + 97.0, id + 41.0))) * pixel.y;
            highp float slice = (1.0 - smoothstep(sliceHalf * 0.34, sliceHalf, abs(uv.y - sliceCentre))) * lifetime;
            highp float horizontal = vhsHash(vec2(index + 109.0, id + 53.0)) < 0.5 ? -1.0 : 1.0;
            highp float offsetPixels = mix(9.0, 72.0, vhsHash(vec2(index + 127.0, id + 67.0)));
            highp float stretch = vhsSmoothNoiseRow(uv.x * mix(3.0, 8.0, vhsHash(vec2(index + 131.0, id))) + index * 4.3 + id * 13.0) - 0.5;
            offsetUv += vec2(horizontal * (offsetPixels + stretch * offsetPixels * 0.72) * pixel.x,
                             vertical * mix(1.0, 7.0, vhsHash(vec2(index + 149.0, id + 79.0))) * pixel.y) * offsetStrength * slice;
            offsetMask = max(offsetMask, slice);
        }
        tapeUv += offsetUv;
        s.echo = band * gate * clamp(glitches * 0.16, 0.0, 0.28) + offsetMask * clamp(offsetStrength * 0.055, 0.0, 0.10);
    }
    return clamp(tapeUv, pixel * 0.5, vec2(1.0) - pixel * 0.5);
}

// step 2: the tape's picture at the warped UV, mixed with the source (whose alpha is source.a)
mediump vec3 vhsTaps(highp vec2 uv, VhsState s, out mediump vec4 source){
    highp vec2 pixel = uber.sourceTexel.xy;
    mediump float bleed = uber.vhs0.w;
    mediump float noise = uber.vhs1.x;
    mediump float streaks = uber.vhs1.y;
    mediump float dropouts = uber.vhs1.z;
    mediump float tracking = uber.vhs0.z;
    highp vec2 sourceUv = s.sourceUv;

    source = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), sourceUv);
    // five taps along the line: the narrow chroma of the recording, its blurred centre
    // trailing behind by an offset; two across it soften it vertically
    highp vec2 bleedStep = vec2(pixel.x * mix(0.75, 4.5, clamp(bleed * 0.5, 0.0, 1.0)), 0.0);
    highp vec2 bleedOffset = vec2(pixel.x * 2.0 * bleed, 0.0);
    mediump vec3 centre = sceneTap(uv);
    mediump vec3 positive1 = sceneTap(uv + bleedOffset + bleedStep);
    mediump vec3 negative1 = sceneTap(uv + bleedOffset - bleedStep);
    mediump vec3 positive2 = sceneTap(uv + bleedOffset + bleedStep * 2.0);
    mediump vec3 negative2 = sceneTap(uv + bleedOffset - bleedStep * 2.0);
    mediump vec3 above = sceneTap(uv - vec2(0.0, pixel.y * 1.5));
    mediump vec3 below = sceneTap(uv + vec2(0.0, pixel.y * 1.5));
    // the line above, which the deck repeats over a dropout
    mediump vec3 concealed = sceneTap(clamp(uv - vec2(0.0, pixel.y * 2.0), pixel * 0.5, vec2(1.0) - pixel * 0.5));
    // the glitches' echo: the head briefly reads a neighbouring part of the line
    centre = mix(centre, s.echoSide > 0.0 ? negative1 : positive1, s.echo);

    mediump vec3 yC = vhsRgbToYiq(centre);
    mediump vec3 yP1 = vhsRgbToYiq(positive1);
    mediump vec3 yN1 = vhsRgbToYiq(negative1);
    mediump vec3 yP2 = vhsRgbToYiq(positive2);
    mediump vec3 yN2 = vhsRgbToYiq(negative2);
    mediump vec2 vertical = (vhsRgbToYiq(above).yz + vhsRgbToYiq(below).yz) * 0.5;
    mediump float blurredI = yC.y * 0.28 + (yP1.y + yN1.y) * 0.24 + (yP2.y + yN2.y) * 0.12;
    mediump float blurredQ = yC.z * 0.20 + (yP1.z + yN1.z) * 0.22 + (yP2.z + yN2.z) * 0.18;
    mediump float trailingI = yC.y * 0.18 + yP1.y * 0.42 + yP2.y * 0.28 + yN1.y * 0.08 + yN2.y * 0.04;
    mediump float trailingQ = yC.z * 0.15 + yP1.z * 0.36 + yP2.z * 0.34 + yN1.z * 0.10 + yN2.z * 0.05;
    blurredI = mix(blurredI, vertical.x, 0.18);
    blurredQ = mix(blurredQ, vertical.y, 0.24);
    mediump float filtering = clamp(bleed * 0.92, 0.0, 1.0);
    mediump vec3 yiq = vec3(yC.x, mix(yC.y, mix(blurredI, trailingI, 0.32), filtering), mix(yC.z, mix(blurredQ, trailingQ, 0.38), filtering));

    // colour-under phase recovery: a field-held wander of hue and saturation
    highp float phase = (vhsHash(vec2(s.fieldTime, 73.0)) - 0.5) * (0.055 * bleed + 0.025 * noise);
    yiq.yz = vec2(yiq.y * cos(phase) - yiq.z * sin(phase), yiq.y * sin(phase) + yiq.z * cos(phase));
    yiq.yz *= 1.0 + (vhsHash(vec2(s.fieldTime, 91.0)) - 0.5) * (0.045 * bleed + 0.025 * noise);
    yiq.x *= 1.0 - clamp(s.crease * 0.065, 0.0, 0.14);
    // the head-switch rows lose chroma and gain
    yiq.yz *= 1.0 - s.headMask * 0.42 * tracking;
    yiq.x *= 1.0 - s.headMask * 0.075 * tracking;

    // narrow bands lose chroma lock (in YIQ: the luma stays)
    highp vec2 frameOffset = vec2(vhsHash(vec2(s.frameTime, 0.0)), vhsHash(vec2(s.frameTime, 1.0)));
    highp float streakBand = floor(sourceUv.y / 0.018);
    mediump float streakGate = step(1.0 - min(0.0125 * streaks, 0.2), vhsHash(vec2(streakBand, frameOffset.x * 50.0)));
    highp float phaseError = (vhsHash(vec2(streakBand, s.frameTime)) * 2.0 - 1.0) * 0.42 * streaks;
    mediump vec2 rotated = vec2(yiq.y * cos(phaseError) - yiq.z * sin(phaseError), yiq.y * sin(phaseError) + yiq.z * cos(phaseError));
    yiq.yz = mix(yiq.yz, rotated, streakGate * clamp(streaks * 0.55, 0.0, 0.85));

    // dropouts: missing RF over a few rows and an irregular stretch of the line, concealed
    // with the line above
    if (dropouts > 0.0){
        highp float tick = floor(s.time * 15.0);
        const highp float BAND_HEIGHT = 0.018;
        highp float band = floor(sourceUv.y / BAND_HEIGHT);
        mediump float gate = step(1.0 - min(0.018 * dropouts, 0.20), vhsHash(vec2(band, tick)));
        highp float rowPhase = fract(sourceUv.y / BAND_HEIGHT);
        highp float centreRow = mix(0.28, 0.72, vhsHash(vec2(band + 17.0, tick)));
        highp float halfWidth = mix(0.035, 0.095, vhsHash(vec2(band + 29.0, tick)));
        mediump float envelope = 1.0 - smoothstep(halfWidth * 0.42, halfWidth, abs(rowPhase - centreRow));
        mediump float echo = (1.0 - smoothstep(halfWidth * 0.24, halfWidth * 0.62, abs(rowPhase - centreRow - halfWidth * 1.8))) * 0.24;
        highp float start = vhsHash(vec2(band + 43.0, tick)) * 0.72;
        highp float end = min(start + mix(0.08, 0.28, vhsHash(vec2(band + 61.0, tick))), 0.985);
        mediump float segment = smoothstep(start - 0.012, start + 0.012, sourceUv.x) * (1.0 - smoothstep(end - 0.018, end + 0.018, sourceUv.x));
        segment *= mix(0.45, 1.0, smoothstep(0.18, 0.76, vhsValueNoise(vec2(sourceUv.x * 19.0 + tick * 0.73, band * 0.37))));
        mediump float amount = gate * (envelope + echo) * segment * clamp(dropouts * 0.62, 0.0, 0.88);
        mediump vec3 concealedYiq = vhsRgbToYiq(concealed);
        concealedYiq.yz *= 0.72;
        yiq = mix(yiq, concealedYiq, amount * 0.86);
        yiq.x *= 1.0 - amount * 0.08;
    }
    return mix(source.rgb, clamp(vhsYiqToRgb(yiq), 0.0, 1.0), uber.vhs0.x);
}

// step 7: the tape's noise and the display's scanlines on the graded colour
mediump vec3 vhsNoise(mediump vec3 color, VhsState s, highp vec2 imageUv){
    highp vec2 sizePx = uber.sourceTexel.zw;
    mediump float intensity = uber.vhs0.x;
    mediump float noise = uber.vhs1.x * intensity;
    highp float fieldParity = mod(s.fieldTime, 2.0);
    highp vec2 frameOffset = vec2(vhsHash(vec2(s.frameTime, 0.0)), vhsHash(vec2(s.frameTime, 1.0)));
    highp vec2 uv = s.sourceUv;

    // tape noise: far less horizontal bandwidth than pixel noise, luma carries most of it
    highp float lineNoise = vhsSmoothNoise(uv.y * sizePx.y * 0.42 + s.frameTime * 1.31);
    if (noise > 0.0){
        mediump float lumaNoise = vhsValueNoise(vec2(uv.x * sizePx.x * 0.09 + frameOffset.x * 37.0, uv.y * sizePx.y * 0.62 + frameOffset.y * 53.0));
        highp vec2 chromaCoord = vec2(uv.x * sizePx.x * 0.028 + frameOffset.x * 11.0, uv.y * sizePx.y * 0.24 + frameOffset.y * 17.0);
        mediump vec2 chromaNoise = vec2(vhsValueNoise(chromaCoord), vhsValueNoise(chromaCoord + vec2(23.7, 9.1)));
        mediump float luminance = dot(color, vec3(0.299, 0.587, 0.114));
        mediump float wear = vhsSmoothNoise(uv.y * 128.0 - s.time * 3.1);
        mediump float volume = mix(0.038, 0.013, luminance) * noise * mix(0.08, 0.62, smoothstep(0.72, 0.91, wear));
        mediump vec3 yiq = vhsRgbToYiq(color);
        yiq.x += (lumaNoise - 0.5) * volume + (lineNoise - 0.5) * volume * 0.22;
        yiq.yz += (chromaNoise - 0.5) * volume * 0.34;
        color = vhsYiqToRgb(yiq);
        // slow gain pumping of the transport and the automatic gain control
        highp float drift = vhsSmoothNoise(s.time * 0.48 + uv.y * 1.35) - 0.5;
        highp float agc = vhsSmoothNoise(s.time * 1.9 - uv.y * 0.7) - 0.5;
        highp float flutter = sin(s.time * 1.37 + uv.y * 3.1) * 0.5;
        color *= 1.0 + (drift + agc * 0.25 + flutter * 0.18) * 0.055 * noise;
    }
    // interference: a broad mains hum bar and the thin lines' flash
    mediump float interference = uber.vhs2.x * intensity;
    if (interference > 0.0){
        highp float hum = smoothstep(0.22, 0.78, sin((uv.y - s.time * 0.34) * 2.0 * VHS_PI) * 0.5 + 0.5) - 0.5;
        color = color * (1.0 + hum * 0.025 * interference) + vec3(s.lineFlash * 0.035 * interference);
    }
    color = max(color, vec3(0.0));
    // the 480-line display the tape plays on
    highp float scanline = sin((imageUv.y * 480.0 + fieldParity * 0.5 + lineNoise * 0.08) * VHS_PI * 2.0) * 0.5 + 0.5;
    return color * mix(1.0 - 0.042 * uber.vhs1.w * intensity, 1.0, scanline);
}
