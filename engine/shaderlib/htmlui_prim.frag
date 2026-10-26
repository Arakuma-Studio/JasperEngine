#version 450

#include "includes/htmlui_sdf.glsl"

// HTML UI primitives (S16 R8, htmlui_prim.vert): premultiplied sRGB like htmlui.frag. Boxes and
// shadows are evaluated from rounded-rectangle distances at the fragment's document position,
// antialiased by the box filter of its screen footprint (at any sample count); boxes return their
// fill without any distance inside the inner rectangle (R24). Quads sample the run's texture by
// its slot's kind (u_fs_htmluiSlots: RGBA, R8 coverage, MTSDF glyphs) when textured.

// up to four textures a draw (S16 B1): an instance's slot is v_params.z (0: untextured, 1-4), each
// slot's kind (0 RGBA, 1 R8 coverage, 2 MTSDF) and MTSDF parameters (effect, dilation, sigma, range)
uniform texture2D u_htmluiTexture0;
uniform sampler u_htmlui_smp0;
uniform texture2D u_htmluiTexture1;
uniform sampler u_htmlui_smp1;
uniform texture2D u_htmluiTexture2;
uniform sampler u_htmlui_smp2;
uniform texture2D u_htmluiTexture3;
uniform sampler u_htmlui_smp3;
uniform u_fs_htmluiSlots {
    vec4 kind;
    vec4 text[4];
} slots;

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

// A gradient instance's colour at g, its own px (mode 4: function in v_f.x, p and v in v_d, stops in
// v_e and v_border0-3, their count in v_f.y)
vec4 gradientStops(vec2 g){
    float t;
    if (v_f.x < 0.5){
        float distSquare = max(dot(v_d.zw, v_d.zw), 1e-12);
        t = dot(v_d.zw, g - v_d.xy) / distSquare;
    }else{
        t = length(v_d.zw * (g - v_d.xy));
    }
    int count = int(v_f.y + 0.5);
    vec4 stopColor = v_border0;
    float previous = v_e.x;
    for (int i = 1; i < 4; i++){
        if (i >= count)
            break;
        float position = (i == 1) ? v_e.y : ((i == 2) ? v_e.z : v_e.w);
        vec4 next = (i == 1) ? v_border1 : ((i == 2) ? v_border2 : v_border3);
        // a hard stop (equal positions) switches at its position
        float f = (position > previous) ? clamp((t - previous) / (position - previous), 0.0, 1.0) : step(position, t);
        stopColor = mix(stopColor, next, f);
        previous = position;
    }
    return stopColor;
}

// Along one axis of a gradient repeated with `period` (0: one tile): `at`, g in its tile; `other`, g in
// the neighbouring tile the pixel (`halfPixel` either side) reaches across a seam; the part of the
// pixel there (0: none)
float tileSeam(float g, float period, float halfPixel, out float at, out float other){
    at = g;
    other = g;
    if (period <= 0.0)
        return 0.0;
    at = g - period * floor(g / period);
    other = at;
    float h = max(halfPixel, 1e-4);
    if (at < h){
        // the pixel's start lies in the tile before: that tile's colour beyond its end
        other = at + period;
        return (h - at) / (2.0 * h);
    }
    if (at > period - h){
        other = at - period;
        return (at - (period - h)) / (2.0 * h);
    }
    return 0.0;
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
    }else if (mode == 4){
        // a gradient over its fill area (S16-F5): htmlui_gradient.frag's linear or radial function, at
        // most four stops (positions in v_e, colours in v_border0-3), in the gradient's own px: from
        // its origin (a background tile's, v_params.yw), repeated with the tiles' period (v_f.zw, 0:
        // one tile). A pixel across a tile seam mixes the two tiles by its part in each, as the
        // multisampled tile quads of the mesh path do (each tile's colour at the pixel's centre).
        vec2 g = p - v_a.xy - v_params.yw;
        vec2 footprint = 0.5 * (abs(dpdx) + abs(dpdy));
        vec2 m, other, across;
        across.x = tileSeam(g.x, v_f.z, footprint.x, m.x, other.x);
        across.y = tileSeam(g.y, v_f.w, footprint.y, m.y, other.y);
        vec4 stops = gradientStops(m);
        if (across.x > 0.0 || across.y > 0.0){
            vec4 x = gradientStops(vec2(other.x, m.y));
            vec4 y = gradientStops(vec2(m.x, other.y));
            vec4 xy = gradientStops(other);
            stops = mix(mix(stops, x, across.x), mix(y, xy, across.x), across.y);
        }
        color = v_color * stops * htmluiClipShape(p, dpdx, dpdy, v_a, v_b, v_c);
    }else{
        // a quad: the colour, times its slot's texture by kind
        color = v_color;
        int slot = int(v_params.z + 0.5) - 1;
        if (slot >= 0){
            // the UI's textures have one level (explicit level: the slot's branch is not uniform)
            vec4 texel;
            vec2 size;
            if (slot == 0){
                texel = textureLod(sampler2D(u_htmluiTexture0, u_htmlui_smp0), v_local.zw, 0.0);
                size = vec2(textureSize(sampler2D(u_htmluiTexture0, u_htmlui_smp0), 0));
            }else if (slot == 1){
                texel = textureLod(sampler2D(u_htmluiTexture1, u_htmlui_smp1), v_local.zw, 0.0);
                size = vec2(textureSize(sampler2D(u_htmluiTexture1, u_htmlui_smp1), 0));
            }else if (slot == 2){
                texel = textureLod(sampler2D(u_htmluiTexture2, u_htmlui_smp2), v_local.zw, 0.0);
                size = vec2(textureSize(sampler2D(u_htmluiTexture2, u_htmlui_smp2), 0));
            }else{
                texel = textureLod(sampler2D(u_htmluiTexture3, u_htmlui_smp3), v_local.zw, 0.0);
                size = vec2(textureSize(sampler2D(u_htmluiTexture3, u_htmlui_smp3), 0));
            }
            float kind = (slot == 0) ? slots.kind.x : ((slot == 1) ? slots.kind.y : ((slot == 2) ? slots.kind.z : slots.kind.w));
            if (kind > 1.5)
                color *= htmluiDistanceTextOf(texel, v_local.zw * size, p, slots.text[slot]);
            else if (kind > 0.5)
                color *= texel.r;
            else
                color *= texel;
        }
    }

    if (clip.params.x > 0.5)
        color *= htmluiClipCoverage(p, dpdx, dpdy);
    g_finalColor = color;
}
