#version 450

// HTML UI primitives (S16 R8): one instance per glyph or image quad, box (background and border),
// outline ring or box shadow, drawn over a unit quad. Positions are document px (the translation
// baked in) mapped by `transform` (projection * element transform), so every transform, 2D, 3D
// or perspective, draws them; the fragment stage antialiases from screen derivatives.
//
// Instance fields (HtmlUIPrimitive, RmlRendererInternal.h), by mode (i_params.x):
//   0 QUAD:      i_a = corners 0 and 1, i_b = corners 2 and 3 (top-left, top-right, bottom-right,
//                bottom-left), i_d = uv rect; i_params.z = 1 when textured
//   1 BOX:       i_a = border box (x, y, w, h), i_b/i_c = its radii x/y, i_d = border widths
//                (top, right, bottom, left), i_e = the fill's insets, i_color = fill,
//                i_border0..3 = border colours
//   2 SHADOW_OUTER, 3 SHADOW_INSET: i_a = the shadow's shape, i_b/i_c = its radii, i_d = the
//                clip box, i_e/i_f = its radii, i_params.y = sigma, i_color = the shadow colour

uniform u_vs_htmluiPrim {
    mat4 transform;
    vec4 viewport;      // xy: target px; z: 1 to cut the shapes' quads to `scissor`
    vec4 scissor;       // document px (x0, y0, x1, y1): the draw's scissor, when transform is the projection
} prim;

in vec2 a_position;     // the unit quad's corner
in vec4 i_a;
in vec4 i_b;
in vec4 i_c;
in vec4 i_d;
in vec4 i_e;
in vec4 i_f;
in vec4 i_params;
in vec4 i_color;
in vec4 i_border0;
in vec4 i_border1;
in vec4 i_border2;
in vec4 i_border3;

// in the fragment stage's order: their locations follow declaration order
out vec4 v_local;       // xy: document px, zw: uv
flat out vec4 v_a;
flat out vec4 v_b;
flat out vec4 v_c;
flat out vec4 v_d;
flat out vec4 v_e;
flat out vec4 v_f;
flat out vec4 v_params;
flat out vec4 v_color;
flat out vec4 v_border0;
flat out vec4 v_border1;
flat out vec4 v_border2;
flat out vec4 v_border3;

vec2 toScreen(vec2 p){
    vec4 clipPos = prim.transform * vec4(p, 0.0, 1.0);
    return clipPos.xy / max(abs(clipPos.w), 1e-6) * sign(clipPos.w) * 0.5 * prim.viewport.xy;
}

void main() {
    int mode = int(i_params.x + 0.5);
    vec2 p;
    vec2 uv = vec2(0.0);
    if (mode == 0){
        // the quad's own corners: the mesh it replaces, exactly
        p = (a_position.y < 0.5) ? ((a_position.x < 0.5) ? i_a.xy : i_a.zw) : ((a_position.x < 0.5) ? i_b.zw : i_b.xy);
        uv = vec2((a_position.x < 0.5) ? i_d.x : i_d.z, (a_position.y < 0.5) ? i_d.y : i_d.w);
    }else{
        // the area the shape reaches (an inset shadow stays in its clip box), widened by its blur and
        // by two device pixels of edge
        vec4 area = (mode == 3) ? i_d : i_a;
        float reach = (mode == 2) ? 3.0 * i_params.y : 0.0;
        vec2 center = area.xy + 0.5 * area.zw;
        vec2 s0 = toScreen(center);
        float pixelsPerUnit = max(min(length(toScreen(center + vec2(1.0, 0.0)) - s0), length(toScreen(center + vec2(0.0, 1.0)) - s0)), 1e-3);
        float margin = reach + 2.0 / pixelsPerUnit;
        p = area.xy - vec2(margin) + a_position * (area.zw + vec2(2.0 * margin));
        if (prim.viewport.z > 0.5)
            p = clamp(p, prim.scissor.xy, prim.scissor.zw);
    }
    v_local = vec4(p, uv);
    v_a = i_a;
    v_b = i_b;
    v_c = i_c;
    v_d = i_d;
    v_e = i_e;
    v_f = i_f;
    v_params = i_params;
    v_color = i_color;
    v_border0 = i_border0;
    v_border1 = i_border1;
    v_border2 = i_border2;
    v_border3 = i_border3;
    gl_Position = prim.transform * vec4(p, 0.0, 1.0);
    // no depth test: keep z inside every backend's clip range (GL [-w,w], D3D/Metal/Vulkan [0,w])
    gl_Position.z = 0.5 * gl_Position.w;
}
