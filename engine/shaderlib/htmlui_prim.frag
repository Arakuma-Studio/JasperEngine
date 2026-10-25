#version 450

#include "includes/htmlui_sdf.glsl"

// HTML UI primitives (S16 R8, htmlui_prim.vert): premultiplied sRGB like htmlui.frag. Boxes and
// shadows are evaluated from rounded-rectangle distances at the fragment's document position,
// antialiased by the box filter of its screen footprint (at any sample count); boxes return their
// fill without any distance inside the inner rectangle (R24). Quads sample the run's texture by
// its kind (u_fs_htmluiClip.params.y: RGBA, R8 coverage, MTSDF glyphs) when textured.

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

// a corner's colour: the side of the line from the outer corner o through the inner corner (along
// (dx, dy), the widths of the two sides) p is on, antialiased over a pixel's footprint
vec4 cornerColor(vec2 p, vec2 o, vec2 direction, vec4 first, vec4 second, vec2 dpdx, vec2 dpdy){
    vec2 n = vec2(direction.y, -direction.x);
    float len = length(n);
    if (len <= 0.0)
        return second;
    n /= len;
    float d = dot(p - o, n);
    float w = max(length(vec2(dot(dpdx, n), dot(dpdy, n))), 1e-4);
    return mix(second, first, clamp(d / w + 0.5, 0.0, 1.0));
}

// the border colour of the side p is on: in a corner's box (its outer radii, at least the two
// sides' widths) on the side of the line from the outer to the inner corner (the meshes' split,
// antialiased as theirs), else the side past whose inner edge p is (left last)
vec4 borderColor(vec2 p, vec4 outer, vec4 rx, vec4 ry, vec4 widths, vec2 dpdx, vec2 dpdy){
    vec2 o0 = outer.xy;
    vec2 o1 = outer.xy + outer.zw;
    // corner boxes (top-left, top-right, bottom-right, bottom-left)
    vec4 cw = max(rx, vec4(widths.w, widths.y, widths.y, widths.w));
    vec4 ch = max(ry, vec4(widths.x, widths.x, widths.z, widths.z));
    bool leftHalf = p.x < o0.x + 0.5 * outer.z;
    bool topHalf = p.y < o0.y + 0.5 * outer.w;
    if (topHalf && leftHalf && p.x < o0.x + cw.x && p.y < o0.y + ch.x)
        return cornerColor(p, o0, vec2(widths.w, widths.x), v_border0, v_border3, dpdx, dpdy);
    if (topHalf && !leftHalf && p.x > o1.x - cw.y && p.y < o0.y + ch.y)
        return cornerColor(p, vec2(o1.x, o0.y), vec2(-widths.y, widths.x), v_border1, v_border0, dpdx, dpdy);
    if (!topHalf && !leftHalf && p.x > o1.x - cw.z && p.y > o1.y - ch.z)
        return cornerColor(p, o1, vec2(-widths.y, -widths.z), v_border2, v_border1, dpdx, dpdy);
    if (!topHalf && leftHalf && p.x < o0.x + cw.w && p.y > o1.y - ch.w)
        return cornerColor(p, vec2(o0.x, o1.y), vec2(widths.w, -widths.z), v_border3, v_border2, dpdx, dpdy);
    // a straight side: the one past whose inner edge p is
    if (p.y < o0.y + widths.x)
        return v_border0;
    if (p.y > o1.y - widths.z)
        return v_border2;
    if (p.x > o1.x - widths.y)
        return v_border1;
    return v_border3;
}

// the inner shape of a border: the border box without its widths, radii reduced by them (square
// where one of them vanishes)
void innerShape(vec4 outer, vec4 rx, vec4 ry, vec4 widths, out vec4 rect, out vec4 irx, out vec4 iry){
    rect = vec4(outer.xy + vec2(widths.w, widths.x), max(outer.zw - vec2(widths.y + widths.w, widths.x + widths.z), vec2(0.0)));
    irx = max(rx - vec4(widths.w, widths.y, widths.y, widths.w), vec4(0.0));
    iry = max(ry - vec4(widths.x, widths.x, widths.z, widths.z), vec4(0.0));
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
    // (the mode is flat: one per primitive, so every branch below is taken by whole pixel quads)
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
        if (q.x < -curve - pixel && q.y < -curve - pixel){
            // inside the inner rectangle, clear of its curves and its edge pixels
            color = v_color;
        }else{
            float outer = htmluiClipShape(p, dpdx, dpdy, v_a, v_b, v_c);
            float inner = (rect.z > 0.0 && rect.w > 0.0) ? htmluiClipShape(p, dpdx, dpdy, rect, irx, iry) : 0.0;
            // the fill's own shape: the border box, inside opaque borders by its insets
            vec4 fillRect, frx, fry;
            innerShape(v_a, v_b, v_c, v_e, fillRect, frx, fry);
            float fill = (fillRect.z > 0.0 && fillRect.w > 0.0) ? htmluiClipShape(p, dpdx, dpdy, fillRect, frx, fry) : 0.0;
            vec4 border = borderColor(p, v_a, v_b, v_c, v_d, dpdx, dpdy) * max(outer - inner, 0.0);
            color = border + v_color * fill * (1.0 - border.a);
        }
    }else if (mode == 2 || mode == 3){
        // a shadow: its shape blurred by sigma (none: the shape's own antialiased edge), outside the
        // border box (outer), or the shape's outside blurred inside the padding box (inset). The
        // box first: an outer shadow under the border box, or an inset one outside the padding
        // box, is not drawn (most of an outer shadow's quad)
        float box = htmluiClipShape(p, dpdx, dpdy, v_d, v_e, v_f);
        float shape = 0.0;
        if ((mode == 2 && box < 1.0) || (mode == 3 && box > 0.0)){
            if (v_params.y > 0.0){
                // (the blur widened by a pixel's box filter, sigma 1/sqrt(12) px); beyond 3 sigma of
                // the shape's edge the Gaussian is 0 or 1
                float sigma = sqrt(v_params.y * v_params.y + 0.0833333 * max(dot(dpdx, dpdx), dot(dpdy, dpdy)));
                float sd = roundedRectSDF(p, v_a, v_b, v_c);
                if (sd > 3.0 * sigma)
                    shape = 1.0;
                else if (sd > -3.0 * sigma)
                    shape = roundedRectShadow(p, v_a, v_b, v_c, sigma);
            }else{
                shape = htmluiClipShape(p, dpdx, dpdy, v_a, v_b, v_c);
            }
        }
        // an outer shadow is cut out where the box covers its pixel's centre and beyond (coverage of
        // the box shifted in by half a pixel): the box drawn over it then antialiases its own edge
        // against the shadow, not against the shadow's hole (no lighter seam, as the browsers)
        float cut = (mode == 2) ? clamp(roundedRectSDF(p, v_d, v_e, v_f) / rectEdgeFootprint(p, v_d, dpdx, dpdy), 0.0, 1.0) : box;
        float alpha = (mode == 2) ? shape * (1.0 - cut) : (1.0 - shape) * box;
        color = v_color * alpha;
    }else{
        // a quad: the colour, times its texture by kind
        color = v_color;
        if (v_params.z > 0.5){
            // the UI's textures have one level
            vec4 texel = textureLod(sampler2D(u_htmluiTexture, u_htmlui_smp), v_local.zw, 0.0);
            vec2 uvTexels = v_local.zw * vec2(textureSize(sampler2D(u_htmluiTexture, u_htmlui_smp), 0));
            if (clip.params.y > 1.5)
                color *= htmluiDistanceText(texel, uvTexels, p);
            else if (clip.params.y > 0.5)
                color *= texel.r;
            else
                color *= texel;
        }
    }

    if (clip.params.x > 0.5)
        color *= htmluiClipCoverage(p, dpdx, dpdy);
    g_finalColor = color;
}
