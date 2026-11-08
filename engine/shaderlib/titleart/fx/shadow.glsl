// S83's shadow and depth effects (§6.6.7 Extrude, §6.6.10 Perspective Shadow, §6.6.11 Floor
// Reflection), ported from Pachislop src/effects/a/stylize.ts (extrude2d) and shadow.ts
// (perspectiveShadow, floorReflection) by SafEight, used with the author's permission.
//
// Perspective Shadow and Floor Reflection keep Pachislop's y-up formulas: they work in
// "up" coordinates (x, height - y) of the target, converted once here, and sample back through
// tfx_upUv.

#ifndef TITLEART_FX_SHADOW_GLSL
#define TITLEART_FX_SHADOW_GLSL

// a point in up coordinates (target px, y up from the bottom edge) as a texture coordinate
vec2 tfx_upUv(vec2 q) {
    return vec2(q.x / TA_SIZE.x, 1.0 - q.y / TA_SIZE.y);
}

// Extrude: the shape stacked toward a direction behind the layer, one march (<= 1,024 steps,
// early exit at coverage 0.996), each copy shaded by its outward edge normal against the light,
// with a dark contact line at the face and at the back; Fade thins the back end.
// TA_TEX0 L, TA_TEX1 the alpha blur the normals come from, TA_TEX2 the side ramp (front -> back).
// p2 = (direction (frame px, y down), length px, step px), p3 = (light direction (y down), shading,
// edge darkening), p4 = (opacity, fade)
vec4 tfx_extrude() {
    vec2 uv = ta_uv();
    vec4 L = texture(TA_TEX0, uv);
    vec2 px = 1.0 / TA_SIZE;
    vec2 dir = ta.p[2].xy;
    float len = ta.p[2].z, stepPx = ta.p[2].w;
    vec2 light = ta.p[3].xy;
    float shade = ta.p[3].z, edge = ta.p[3].w, opacity = ta.p[4].x, fadeAmt = ta.p[4].y;
    int n = int(ceil(len / stepPx));
    vec4 acc = vec4(0.0);
    float faded = 0.0;
    for (int i = 1; i <= 1024; i++) {
        if (i > n || acc.a > 0.996) break;
        float s = min(float(i) * stepPx, len);
        vec2 q = uv - dir * s * px;
        float a = textureLod(TA_TEX0, q, 0.0).a;
        if (a < 0.002) continue;
        vec2 gr = vec2(textureLod(TA_TEX1, q + vec2(px.x, 0.0), 0.0).a - textureLod(TA_TEX1, q - vec2(px.x, 0.0), 0.0).a,
                       textureLod(TA_TEX1, q + vec2(0.0, px.y), 0.0).a - textureLod(TA_TEX1, q - vec2(0.0, px.y), 0.0).a);
        float gl = length(gr);
        vec2 nrm = gl > 1e-4 ? -gr / gl : dir;
        float lit = 0.5 + 0.5 * dot(nrm, light);
        vec3 c = ta_toLinear(textureLod(TA_TEX2, vec2(s / len, 0.5), 0.0).rgb);
        c *= mix(1.0, 0.4 + 0.95 * lit, shade);
        c *= 1.0 - edge * (1.0 - smoothstep(0.0, 3.0 * TA_PX, s - stepPx));
        c *= 1.0 - 0.6 * edge * smoothstep(len - 3.0 * TA_PX, len, s);
        vec4 add = (1.0 - acc.a) * vec4(c, 1.0) * a;
        acc += add;
        faded += add.a * (1.0 - fadeAmt * s / len);
    }
    if (fadeAmt > 0.0 && acc.a > 0.0) acc *= faded / acc.a;
    return L + acc * opacity * (1.0 - L.a);
}

// Perspective Shadow: cast onto a floor at the base of the layer box, sheared, squashed, blurring
// (near -> far) and fading with distance. TA_TEX0 L, TA_TEX1 the near blur, TA_TEX2 the far blur,
// TA_RAW1 the bounds. p2 = (colour linear, opacity), p3 = (tip offset for the top of the object,
// up coordinates: x sideways, y vertical (- in front); perspective, fade), p4.x = floor offset px
vec4 tfx_perspectiveShadow() {
    vec2 S = TA_SIZE;
    vec4 L = texture(TA_TEX0, ta_uv());
    vec4 b = fx_layerBox();
    vec2 lo = vec2(b.x, S.y - b.w), hi = vec2(b.z, S.y - b.y);
    vec2 len = ta.p[3].xy;
    float persp = ta.p[3].z, fade = ta.p[3].w;
    float H = max(hi.y - lo.y, 1.0);
    float ratio = len.y / H, shear = len.x / H;
    float base = lo.y + ta.p[4].x;
    float cx = 0.5 * (lo.x + hi.x);
    vec2 p = vec2(gl_FragCoord.x, S.y - gl_FragCoord.y);
    float h = (p.y - base) / ratio;
    float edgeK = clamp(h * abs(ratio) + 0.5, 0.0, 1.0);
    h = max(h, 0.0);
    float t = clamp(h / H, 0.0, 1.0);
    float k = max(1.0 - persp * t, 0.05);
    float sx = cx + (p.x - cx - h * shear) / k;
    vec2 q = tfx_upUv(vec2(sx, lo.y + h));
    float a = mix(texture(TA_TEX1, q).a, texture(TA_TEX2, q).a, smoothstep(0.0, 1.0, t));
    a *= edgeK * max(1.0 - fade * t, 0.0) * ta.p[2].w;
    return L + vec4(ta.p[2].rgb * a, a) * (1.0 - L.a);
}

// Floor Reflection: a mirrored copy below the layer box's base, as on a glossy floor, fading and
// blurring with distance. TA_TEX0 L, TA_TEX1 the blur, TA_RAW1 the bounds.
// p2 = (gap px, fade length px, opacity, vertical scale), p3.x = falloff
vec4 tfx_floorReflection() {
    vec2 S = TA_SIZE;
    vec4 L = texture(TA_TEX0, ta_uv());
    vec4 b = fx_layerBox();
    float base = S.y - b.w;
    float floorY = base - ta.p[2].x;
    float d = floorY - (S.y - gl_FragCoord.y);
    if (d <= 0.0) return L;
    float t = clamp(d / ta.p[2].y, 0.0, 1.0);
    vec2 q = tfx_upUv(vec2(gl_FragCoord.x, base + d / ta.p[2].w));
    bool inside = q.y >= ta_frameUv().y;
    vec4 sharp = inside ? texture(TA_TEX0, q) : vec4(0.0);
    vec4 soft = inside ? texture(TA_TEX1, q) : vec4(0.0);
    vec4 r = mix(sharp, soft, smoothstep(0.0, 0.85, t));
    r *= pow(1.0 - t, ta.p[3].x) * ta.p[2].z * clamp(d + 0.5, 0.0, 1.0);
    return L + r * (1.0 - L.a);
}

#endif // TITLEART_FX_SHADOW_GLSL
