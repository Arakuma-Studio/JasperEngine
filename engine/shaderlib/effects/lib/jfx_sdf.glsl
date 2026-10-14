// HTML UI effects: signed distances, in CSS px (negative inside). jfx_rounded_rect_sdf is the
// shaders.com kit's sdf.ts roundedRectSdf (MIT, Copyright (c) 2026 Shader Effects Inc.; see
// effects/LICENSE.shaders-com.txt); the element box with per-corner radii is the engine's.
#ifndef JFX_SDF_GLSL
#define JFX_SDF_GLSL

// the element's rounded box: px from the box's top-left corner, radii tl tr br bl
float jfx_box_sdf(vec2 px, vec2 size, vec4 radii){
    vec2 halfSize = size * 0.5;
    vec2 p = px - halfSize;
    float r = p.x < 0.0 ? (p.y < 0.0 ? radii.x : radii.w) : (p.y < 0.0 ? radii.y : radii.z);
    r = min(r, min(halfSize.x, halfSize.y));
    vec2 q = abs(p) - halfSize + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// sdf.ts roundedRectSdf: d from the centre, half extents, one radius
float jfx_rounded_rect_sdf(vec2 d, vec2 halfSize, float r){
    vec2 q = abs(d) - halfSize + r;
    vec2 mq = max(q, 0.0);
    return sqrt(mq.x * mq.x + mq.y * mq.y) + min(max(q.x, q.y), 0.0) - r;
}

float jfx_circle_sdf(vec2 p, float r){
    return length(p) - r;
}

// distance to the segment a-b
float jfx_segment_sdf(vec2 p, vec2 a, vec2 b){
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-12), 0.0, 1.0);
    return length(pa - ba * h);
}

// coverage of the inside, anti-aliased over one pixel of the distance's screen footprint
float jfx_aa_fill(float sdf){
    float w = max(fwidth(sdf), 1e-4);
    return clamp(0.5 - sdf / w, 0.0, 1.0);
}

// coverage of a band of halfWidth around the edge
float jfx_aa_band(float sdf, float halfWidth){
    return jfx_aa_fill(abs(sdf) - halfWidth);
}

// the box distance's gradient (central differences of 0.5 px): points out of the box
vec2 jfx_box_normal(vec2 px, vec2 size, vec4 radii){
    const float e = 0.5;
    vec2 g = vec2(jfx_box_sdf(px + vec2(e, 0.0), size, radii) - jfx_box_sdf(px - vec2(e, 0.0), size, radii),
                  jfx_box_sdf(px + vec2(0.0, e), size, radii) - jfx_box_sdf(px - vec2(0.0, e), size, radii));
    float l = length(g);
    return l > 1e-6 ? g / l : vec2(0.0);
}

#endif // JFX_SDF_GLSL
