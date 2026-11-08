// S83's stroke and edge effects (§6.6.22 Ink Brush Stroke, §6.6.26 Stitch): original designs on
// the content's distance field. Slots: TA_TEX0 the input L, TA_TEX1 the content, TA_RAW0 its
// distance (+ outside, frame px).
//
// Positions along the contour: the brush samples its noise at the nearest edge point q = p - n d
// (constant across the stroke, so the noise runs along it: Pachislop Corona's trick). Stitch
// needs real arc length for evenly spaced stitches: the contour (at the stitch offset) is split
// at the extremes of a fixed screen axis into monotone pieces; stitch_layout marches each coarse
// cell's contour point along the piece both ways (re-projected onto the contour every step) to
// the piece's two ends, so every point knows its arc length from the piece's start and the
// piece's length, and the stitches of a piece fit it exactly.

#ifndef TITLEART_FX_STROKE_GLSL
#define TITLEART_FX_STROKE_GLSL

// Ink Brush Stroke: a stroke outside, centred on or inside the silhouette whose width breathes
// with pressure along the contour, split into bristle lanes with dry gaps and ragged edges.
// TA_TEX0 L, TA_RAW0 the content's distance. p2 = (position 0 outside 1 centre 2 inside, width px,
// pressure, bristles), p3 = (dry, roughness, opacity, seed), p4.rgb = the colour (linear),
// p5.xy = the canvas centre (target px)
vec4 tfx_brushStroke() {
    vec2 p = gl_FragCoord.xy;
    vec4 L = texture(TA_TEX0, ta_uv());
    int position = int(ta.p[2].x + 0.5);
    float w = max(ta.p[2].y, 0.5);
    float pressure = ta.p[2].z, bristles = ta.p[2].w;
    float dry = ta.p[3].x, rough = ta.p[3].y, opacity = ta.p[3].z, seed = ta.p[3].w;
    float d = tfx_distanceAt(ivec2(p));
    float lo = position == 0 ? 0.0 : position == 1 ? -0.5 * w : -w;
    if (d < lo - w || d > lo + 2.0 * w) return L;
    vec2 n = tfx_distanceNormal(p);
    vec2 q = tfx_canvasPx(p - n * d, ta.p[5].xy);
    float wc = w / TA_PX;
    // pressure: the stroke's width along the contour
    float press = 1.0 - pressure * (0.7 * vnoise(q / (6.0 * wc) + seed * 1.3) + 0.3 * vnoise(q / (2.0 * wc) + seed * 4.1));
    float width = w * max(press, 0.08);
    float a0 = lo, a1 = lo + width;
    if (position == 1) {
        a0 = -0.5 * width;
        a1 = 0.5 * width;
    } else if (position == 2) {
        a0 = -width;
        a1 = 0.0;
    }
    // ragged band edges
    float r0 = rough * w * 0.35 * (vnoise(q / (0.7 * wc) + seed * 2.9) - 0.5);
    float r1 = rough * w * 0.35 * (vnoise(q / (0.7 * wc) + seed * 7.7 + 3.1) - 0.5);
    float cov = clamp(d - (a0 + r0) + 0.5, 0.0, 1.0) * clamp((a1 + r1) - d + 0.5, 0.0, 1.0);
    if (cov <= 0.0) return L;
    // bristle lanes across the band, each with its own dryness along the contour
    float t = clamp((d - a0) / max(a1 - a0, 0.5), 0.0, 1.0);
    float lane = t * mix(3.0, 14.0, bristles);
    float laneN = vnoise(vec2(lane, 0.0) + q / (3.0 * wc) + seed * 5.3);
    float hair = smoothstep(0.15, 0.35, abs(fract(lane) - 0.5) * 2.0 + (1.0 - bristles));
    float dryN = vnoise(vec2(lane * 0.6, 0.0) + q / (1.2 * wc) * vec2(1.0, 0.4) + seed * 9.7);
    float dryCut = smoothstep(dry * 0.9 - 0.08, dry * 0.9 + 0.08, dryN * (0.6 + 0.4 * laneN));
    float k = cov * mix(1.0, hair, bristles * 0.85) * (dry > 0.0 ? dryCut : 1.0) * opacity;
    vec4 K = vec4(ta.p[4].rgb, 1.0) * k;
    // outside: behind the layer; centred and inside: over it
    return position == 0 ? L + K * (1.0 - L.a) : K + L * (1.0 - K.a);
}

// ---- Stitch ----

// the contour at the stitch offset: the nearest point to p on it (two re-projections), its
// outward normal
vec2 tfx_onContour(vec2 p, float offset, out vec2 n) {
    vec2 q = p;
    n = vec2(0.0, -1.0);
    for (int k = 0; k < 3; k++) {
        vec2 g = vec2(tfx_distanceSmooth(q + vec2(1.0, 0.0)) - tfx_distanceSmooth(q - vec2(1.0, 0.0)),
                      tfx_distanceSmooth(q + vec2(0.0, 1.0)) - tfx_distanceSmooth(q - vec2(0.0, 1.0)));
        float l = length(g);
        if (l < 1e-4) break;
        n = g / l;
        q -= n * (tfx_distanceSmooth(q) - offset);
    }
    return q;
}

// the screen axis whose extremes split the contour into pieces (y, tilted so horizontal edges
// are not flat)
const vec2 TFX_STITCH_AXIS = vec2(0.0499376, 0.9987523);

// the contour's direction at q (n turned a quarter: around every shape the same way)
vec2 tfx_tangentOf(vec2 n) {
    return vec2(-n.y, n.x);
}

// arc length from q along `sign` x the tangent to where the piece ends (the tangent's component
// along the axis changes sign), at most maxLen px
float tfx_marchToEnd(vec2 q, float offset, float dirSign, float stepPx, float maxLen) {
    vec2 n;
    vec2 cur = tfx_onContour(q, offset, n);
    float s0 = sign(dot(tfx_tangentOf(n), TFX_STITCH_AXIS));
    float prevDot = dot(tfx_tangentOf(n), TFX_STITCH_AXIS);
    float len = 0.0;
    for (int i = 0; i < 512; i++) {
        if (len >= maxLen) return maxLen;
        vec2 t = tfx_tangentOf(n) * dirSign;
        vec2 next = tfx_onContour(cur + t * stepPx, offset, n);
        float stepLen = length(next - cur);
        float dt = dot(tfx_tangentOf(n), TFX_STITCH_AXIS);
        if (sign(dt) != s0 || stepLen < 1e-3) {
            // the end lies between the two points: where the axis component crossed zero
            float f = abs(prevDot) / max(abs(prevDot) + abs(dt), 1e-6);
            return len + stepLen * f;
        }
        len += stepLen;
        prevDot = dt;
        cur = next;
    }
    return min(len, maxLen);
}

// The contour pieces: for each coarse cell near the contour, its contour point's arc length from
// the piece's start and the piece's length, written as a seed (x = from the start, y = the
// piece's length, z valid). TA_RAW0 the content's distance. p2 = (offset px, cell px, march step
// px, longest piece px), p3.x = the band px a cell must lie within
vec4 tfx_stitchLayout() {
    float offset = ta.p[2].x, cell = ta.p[2].y, stepPx = ta.p[2].z, maxLen = ta.p[2].w;
    vec2 centre = (floor(gl_FragCoord.xy) + 0.5) * cell;
    if (abs(tfx_distanceSmooth(centre) - offset) > ta.p[3].x) return fx_writeSeed(vec3(0.0));
    vec2 n;
    vec2 q = tfx_onContour(centre, offset, n);
    float back = tfx_marchToEnd(q, offset, -1.0, stepPx, maxLen);
    float fwd = tfx_marchToEnd(q, offset, 1.0, stepPx, maxLen);
    return fx_writeSeed(vec3(back, back + fwd, 1.0));
}

// a seed (fx_writeSeed's encoding) from the second raw slot, invalid outside it
vec3 tfx_readSeed1(ivec2 q) {
    ivec2 size = textureSize(TA_RAW1, 0);
    if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) return vec3(0.0);
    vec4 t = texelFetch(TA_RAW1, q, 0);
#ifdef TA_SEEDS_FIXED
    float x = ta_bytes16(t.r, t.g), y = ta_bytes16(t.b, t.a);
    if (x > 65534.5) return vec3(0.0);
    return vec3(x / 16.0 - 1.0, y / 16.0 - 1.0, 1.0);
#else
    return vec3(t.xy, t.z > 0.5 ? 1.0 : 0.0);
#endif
}

// a capsule from a to b of radius r: (signed distance, position along it 0..1)
vec2 tfx_capsule(vec2 p, vec2 a, vec2 b, float r) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
    return vec2(length(pa - ba * h) - r, h);
}

// Stitch: thread stitches along the contour at the offset (running, cross, zigzag or a satin
// border), fitted evenly to each piece, shaded as round threads lit by the light, needle holes
// darkening their ends, with an optional satin or tatami fill of thread lines over the layer.
// TA_TEX0 L, TA_RAW0 the content's distance, TA_RAW1 the layout (seeds, one per layout cell).
// p2 = (style 0 running 1 cross 2 zigzag 3 satin border, offset px, stitch length px, gap px),
// p3 = (thread colour linear, thread width px), p4 = (shade, holes, fill 0 none 1 satin 2 tatami,
// fill density px), p5 = (light direction (y down), fill direction (y down)), p6 = (layout cell px,
// canvas centre target px, seed)
vec4 tfx_stitch() {
    vec2 p = gl_FragCoord.xy;
    vec4 L = texture(TA_TEX0, ta_uv());
    int style = int(ta.p[2].x + 0.5);
    float offset = ta.p[2].y, stitchLen = max(ta.p[2].z, 1.0), gap = ta.p[2].w;
    float tw = max(ta.p[3].w, 0.5);
    float shadeAmt = ta.p[4].x, holes = ta.p[4].y;
    int fill = int(ta.p[4].z + 0.5);
    float density = max(ta.p[4].w, 1.0);
    vec2 Ld2 = ta.p[5].xy;
    float cell = ta.p[6].x;
    float d = tfx_distanceSmooth(p);
    vec4 outC = L;
    // the fill: thread lines inside the stitch line modulating the layer's colour
    if (fill > 0 && d < offset - tw && L.a > 0.0) {
        vec2 fd = ta.p[5].zw;
        vec2 fp = vec2(-fd.y, fd.x);
        vec2 c = tfx_canvasPx(p, ta.p[6].yz) * TA_PX;
        float v = dot(c, fp) / density;
        float u = dot(c, fd);
        if (fill == 2) {
            // tatami: short stitches, each row's breaks offset like bricks
            float row = floor(v);
            u += row * 0.37 * density * 6.0;
            float brk = abs(fract(u / (density * 6.0)) - 0.5) * 2.0;
            v += step(0.94, brk) * 0.5;
        }
        float across = abs(fract(v) - 0.5) * 2.0;
        float thread = 0.72 + 0.28 * sqrt(max(1.0 - across * across, 0.0));
        vec3 rgb = ta_unpremul(L) * mix(1.0, thread, 0.8);
        outC = vec4(rgb * L.a, L.a);
    }
    float band = abs(d - offset);
    float reachAcross = style == 0 ? tw : style == 3 ? 0.5 * stitchLen + tw : 0.5 * stitchLen + tw;
    if (band > reachAcross + 1.0) return outC;
    vec2 n;
    vec2 q = tfx_onContour(p, offset, n);
    vec2 t = tfx_tangentOf(n);
    // the layout of the cell q falls in: its own contour point's arc length, carried to q
    ivec2 ci = ivec2(floor(q / cell));
    vec3 lay = tfx_readSeed1(ci);
    if (lay.z < 0.5) return outC;
    vec2 cn;
    vec2 cq = tfx_onContour((vec2(ci) + 0.5) * cell, offset, cn);
    float s = lay.x + dot(q - cq, t);
    float pieceLen = max(lay.y, 1.0);
    float period = stitchLen + gap;
    float count = max(floor(pieceLen / period + 0.5), 1.0);
    float pp = pieceLen / count;
    float k = pp / period;
    // a hand-sewn jitter: each stitch shifts by up to 6 % of its period
    float jitter = (hash12(vec2(floor(s / pp), ta.p[6].w)) - 0.5) * 0.12 * pp;
    float local = s - floor(s / pp) * pp - jitter;
    float across = d - offset;
    float sl = stitchLen * k;
    float gp = gap * k;
    vec2 lp = vec2(local - 0.5 * pp, across);
    vec2 cap;
    float r = 0.5 * tw;
    if (style == 1) {
        // cross: an X within each stitch cell
        float h = 0.5 * min(sl, stitchLen);
        vec2 c1 = tfx_capsule(lp, vec2(-h, -h), vec2(h, h), r), c2 = tfx_capsule(lp, vec2(-h, h), vec2(h, -h), r);
        cap = c1.x < c2.x ? c1 : c2;
    } else if (style == 2) {
        // zigzag: diagonals alternating up and down, end to end
        float h = 0.5 * stitchLen;
        float flip = mod(floor(s / pp), 2.0) < 0.5 ? 1.0 : -1.0;
        cap = tfx_capsule(lp, vec2(-0.5 * pp, -h * flip), vec2(0.5 * pp, h * flip), r);
    } else if (style == 3) {
        // satin border: dense threads across the band, one per thread width
        float sp = max(tw * 1.05, 1.0);
        float m = s - floor(s / sp) * sp - 0.5 * sp;
        cap = tfx_capsule(vec2(m, across), vec2(0.0, -0.5 * stitchLen), vec2(0.0, 0.5 * stitchLen), r * 0.95);
    } else {
        cap = tfx_capsule(lp, vec2(-0.5 * sl + r, 0.0), vec2(0.5 * sl - r, 0.0), r);
    }
    float cov = clamp(0.5 - cap.x, 0.0, 1.0);
    // needle holes at a stitch's two ends
    float hole = 0.0;
    if (holes > 0.0 && style != 3) {
        float he = abs(abs(lp.x) - 0.5 * (style == 0 ? sl : pp));
        hole = holes * (1.0 - smoothstep(0.0, 0.9 * tw, length(vec2(he, across))));
    }
    if (cov <= 0.0 && hole <= 0.0) return outC;
    // a round thread: brighter along its middle, lit from the light's side
    float x = clamp((cap.x + r) / max(r, 0.25), 0.0, 1.0);
    float nz = sqrt(max(1.0 - x * x, 0.0));
    float side = dot(n, Ld2);
    float shade = mix(1.0, 0.55 + 0.45 * nz + 0.25 * side * (1.0 - nz), shadeAmt);
    // the twist of the thread's plies
    shade *= 1.0 - 0.12 * shadeAmt * step(0.5, fract((cap.y * sl + across) / max(tw * 0.9, 0.5)));
    vec3 rgb = ta.p[3].rgb * shade;
    outC = vec4(outC.rgb * (1.0 - hole * 0.6), outC.a);
    vec4 K = vec4(rgb, 1.0) * cov;
    return K + outC * (1.0 - K.a);
}

#endif // TITLEART_FX_STROKE_GLSL
