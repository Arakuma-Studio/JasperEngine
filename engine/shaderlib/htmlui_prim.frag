#version 450

#include "includes/htmlui_sdf.glsl"

// HTML UI primitives (S16 R8, htmlui_prim.vert): premultiplied sRGB like htmlui.frag. Boxes and
// shadows are evaluated from rounded-rectangle distances at the fragment's document position,
// antialiased by the box filter of its screen footprint (at any sample count); boxes return their
// fill without any distance inside the inner rectangle (R24).

// the analytic clip (R6) and the texture's kind, as htmlui.frag's
uniform u_fs_htmluiClip {
    vec4 rect[2];
    vec4 radiiX[2];
    vec4 radiiY[2];
    vec4 params;        // x: clip shapes (0, 1 or 2), y: 1 = a coverage texture (R8)
} clip;

uniform texture2D u_htmluiTexture;
uniform sampler u_htmlui_smp;

in vec4 v_local;
flat in vec4 v_a;
flat in vec4 v_b;
flat in vec4 v_c;
flat in vec4 v_d;
flat in vec4 v_e;
flat in vec4 v_f;
flat in vec4 v_params;
flat in vec4 v_color;
flat in vec4 v_border0;
flat in vec4 v_border1;
flat in vec4 v_border2;
flat in vec4 v_border3;

out vec4 g_finalColor;

// the border colour of the side p is on: past the inner rectangle's edges, and in a corner on the
// side of the line from the outer to the inner corner (the meshes' split)
vec4 borderColor(vec2 p, vec4 outer, vec4 widths){
    vec2 o0 = outer.xy;
    vec2 o1 = outer.xy + outer.zw;
    vec2 i0 = o0 + vec2(widths.w, widths.x);
    vec2 i1 = o1 - vec2(widths.y, widths.z);
    bool left = p.x < i0.x, right = p.x > i1.x, top = p.y < i0.y, bottom = p.y > i1.y;
    if (top && left)
        return ((p.x - o0.x) * widths.x > (p.y - o0.y) * widths.w) ? v_border0 : v_border3;
    if (top && right)
        return ((o1.x - p.x) * widths.x > (p.y - o0.y) * widths.y) ? v_border0 : v_border1;
    if (bottom && right)
        return ((o1.x - p.x) * widths.z > (o1.y - p.y) * widths.y) ? v_border2 : v_border1;
    if (bottom && left)
        return ((p.x - o0.x) * widths.z > (o1.y - p.y) * widths.w) ? v_border2 : v_border3;
    if (top)
        return v_border0;
    if (bottom)
        return v_border2;
    if (right)
        return v_border1;
    return v_border3;
}

// the inner shape of a border: the border box without its widths, radii reduced by them (square
// where one of them vanishes)
void innerShape(vec4 outer, vec4 rx, vec4 ry, vec4 widths, out vec4 rect, out vec4 irx, out vec4 iry){
    rect = vec4(outer.xy + vec2(widths.w, widths.x), outer.zw - vec2(widths.y + widths.w, widths.x + widths.z));
    irx = rx - vec4(widths.w, widths.y, widths.y, widths.w);
    iry = ry - vec4(widths.x, widths.x, widths.z, widths.z);
    for (int i = 0; i < 4; i++){
        if (irx[i] <= 0.0 || iry[i] <= 0.0){
            irx[i] = 0.0;
            iry[i] = 0.0;
        }
    }
}

void main() {
    vec2 p = v_local.xy;
    // derivatives first, in uniform control flow
    vec2 dpdx = dFdx(p);
    vec2 dpdy = dFdy(p);
    int mode = int(v_params.x + 0.5);
    vec4 color;

    if (mode == 1){
        // a box: the fill over the border box, the border over it
        vec4 rect, irx, iry;
        innerShape(v_a, v_b, v_c, v_d, rect, irx, iry);
        vec2 q = abs(p - (rect.xy + 0.5 * rect.zw)) - 0.5 * rect.zw;
        vec4 corner = max(irx, iry);
        float curve = max(max(corner.x, corner.y), max(corner.z, corner.w));
        float pixel = max(length(dpdx), length(dpdy));
        if (q.x < -curve - pixel && q.y < -curve - pixel && rect.z > 0.0 && rect.w > 0.0){
            // inside the inner rectangle, clear of its curves and its edge pixels
            color = v_color;
        }else{
            float outer = htmluiClipShape(p, dpdx, dpdy, v_a, v_b, v_c);
            float inner = (rect.z > 0.0 && rect.w > 0.0) ? htmluiClipShape(p, dpdx, dpdy, rect, irx, iry) : 0.0;
            vec4 border = borderColor(p, v_a, v_d) * max(outer - inner, 0.0);
            color = border + v_color * outer * (1.0 - border.a);
        }
    }else if (mode == 2 || mode == 3){
        // a shadow: the Gaussian edge of its shape (sigma 0: the shape's own edge), outside the
        // border box (outer) or the padding box's inside, the shape's outside (inset)
        float sd = roundedRectSDF(p, v_a, v_b, v_c);
        float w = rectEdgeFootprint(p, v_a, dpdx, dpdy);
        float shape = (v_params.y > 0.0) ? gaussianEdge(sd, v_params.y, 0.5 * w) : sdfCoverage(sd, w);
        float box = htmluiClipShape(p, dpdx, dpdy, v_d, v_e, v_f);
        float alpha = (mode == 2) ? shape * (1.0 - box) : (1.0 - shape) * box;
        color = v_color * alpha;
    }else{
        // a quad: the colour, times its texture
        color = v_color;
        if (v_params.z > 0.5){
            vec4 texel = texture(sampler2D(u_htmluiTexture, u_htmlui_smp), v_local.zw);
            color *= (clip.params.y > 0.5) ? texel.rrrr : texel;
        }
    }

    if (clip.params.x > 0.5){
        float coverage = htmluiClipShape(p, dpdx, dpdy, clip.rect[0], clip.radiiX[0], clip.radiiY[0]);
        if (clip.params.x > 1.5)
            coverage *= htmluiClipShape(p, dpdx, dpdy, clip.rect[1], clip.radiiX[1], clip.radiiY[1]);
        color *= coverage;
    }
    g_finalColor = color;
}
