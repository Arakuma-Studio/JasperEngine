#version 450
precision highp float;
precision highp int;

// The screen transition (docs/specs/S48-pixel-fx.md R12-R17, D7, D8): one fullscreen triangle
// over the finished frame, premultiplied alpha (ONE, ONE_MINUS_SRC_ALPHA), no texture. Every
// output pixel is a function of its art pixel (the S41 grid, or pixel_size screen pixels per art
// pixel) and the coverage, so all the screen pixels of one art pixel get one colour and the edges
// are hard. It writes (light + cover * colour, cover): ink where covered, a rim band of ramp
// steps just inside the cover's edge, and light from the ramp just outside it.
//
// The band kinds (iris, blinds, slash, burn away) are a field over art pixel centres, covered
// where it is below the coverage's level. The rim is exactly `rim` art pixels along the field's
// dominant axis: a covered pixel is rim at depth s when the pixel s steps toward the uncovered
// side is uncovered (no derivative instruction anywhere: the gradient is the field's own finite
// difference at the neighbouring art pixels, as pixelfx's field() in
// src/fx/transitions/_lib.js, MIT, Copyright (c) 2026 Patch). Pixel Dissolve winks blocks of
// 1, 2 and 4 cells in by a clumpy rank, each drawn first as a ramp outline; CRT Off squeezes the
// picture to a line and a dot; Fade is an 8x8 Bayer dither per art pixel. engine/core/pixelart/
// ScreenTransition.cpp mirrors this on the CPU (the edge lint, the tests).

#include "includes/pixel_fx.glsl"
#include "includes/bayer.glsl"

in vec2 v_texcoord;
out vec4 frag_color;

uniform u_fs_transition {
    vec4 art;       // x art width, y art height (art pixels), z screen pixels per art pixel, w 1: the target's rows run bottom-up (the GL swapchain)
    vec4 frame;     // xy the art grid's top-left in screen pixels from the image's top-left, zw the target's size (pixels)
    vec4 state;     // x coverage 0..1, y phase (0 cover, 1 hold, 2 reveal), z kind, w the kind's switch (iris shape, blinds vertical, fade through the ramp)
    vec4 params0;   // iris: centre xy, outer radius z; blinds: count x; slash: normal xy, lowest z, range w; dissolve: cell x; burn: scale x, the field's lowest y and highest z
    vec4 params1;   // burn: the reveal hole's centre xy, its field's lowest z and highest w
    vec4 rim;       // x rim (art pixels), y glow, z ramp steps (1..16), w seed
    vec4 ink;       // rgb display colour, a 1
    vec4 ramp[16];  // display colours, darkest first
} tr;

const int KIND_IRIS = 0;
const int KIND_BLINDS = 1;
const int KIND_SLASH = 2;
const int KIND_PIXEL_DISSOLVE = 3;
const int KIND_CRT_OFF = 4;
const int KIND_BURN_AWAY = 5;
const int KIND_FADE = 6;

int trKind(){
    return int(tr.state.z + 0.5);
}

int trSteps(){
    return clamp(int(tr.rim.z + 0.5), 1, 16);
}

// the ramp's step i counted from its hot (last) end
vec3 trHot(int i){
    int n = trSteps();
    return tr.ramp[clamp(n - 1 - i, 0, n - 1)].rgb;
}

uint trSeed(){
    return uint(tr.rim.w + 0.5);
}

// burn away's field: rising from the edges' midpoints (the corners first), ragged at two scales,
// the bottom a little faster; the reveal's: a hole from near the middle out to the corners
float trBurn(vec2 p){
    vec2 size = tr.art.xy;
    float scale = tr.params0.x;
    uint seed = trSeed();
    float ragged = 0.36 * scale * (pfxValueNoise(p, max(scale * 0.5, 2.0), seed + 21u) - 0.5)
                 + 2.4 * (pfxValueNoise(p, 7.0, seed + 33u) - 0.5);
    if (int(tr.state.y + 0.5) == 2){
        vec2 d = (p - tr.params1.xy) / (0.5 * size);
        return -length(d) * scale - ragged;
    }
    vec2 u = (p - 0.5 * size) / (0.5 * size);
    vec2 u2 = u * u;
    float m = sqrt(sqrt(u2.x * u2.x + u2.y * u2.y));
    return (1.0 - m) * scale + ragged + 0.07 * scale * (0.5 - p.y / size.y);
}

// the band kinds' field at an art pixel centre: covered where it is below trLevel()
float trField(vec2 p){
    int kind = trKind();
    if (kind == KIND_IRIS){
        vec2 d = abs(p - tr.params0.xy);
        int shape = int(tr.state.w + 0.5);
        float f = shape == 1 ? d.x + d.y : (shape == 2 ? max(d.x, d.y) : length(d));
        return tr.params0.z - f;
    }
    if (kind == KIND_BLINDS){
        bool vertical = tr.state.w > 0.5;
        float a = vertical ? p.x : p.y;
        float band = (vertical ? tr.art.x : tr.art.y) / max(tr.params0.x, 1.0);
        return a - floor(a / band) * band;
    }
    if (kind == KIND_SLASH)
        return dot(p, tr.params0.xy) - tr.params0.z;
    return trBurn(p);
}

float trLevel(){
    float c = tr.state.x;
    int kind = trKind();
    if (kind == KIND_IRIS)
        return c * (tr.params0.z + 1.0);
    if (kind == KIND_BLINDS)
        return c * ((tr.state.w > 0.5 ? tr.art.x : tr.art.y) / max(tr.params0.x, 1.0) + 1.0);
    if (kind == KIND_SLASH)
        return c * (tr.params0.w + 1.0);
    vec2 range = int(tr.state.y + 0.5) == 2 ? tr.params1.zw : tr.params0.yz;
    return range.x - 1.0 + c * (range.y - range.x + 2.0);
}

// the field's gradient: exact for blinds and slash (a sawtooth's finite difference would jump at
// a band's end), else forward differences toward the screen's inside
vec2 trGradient(vec2 p, float v){
    int kind = trKind();
    if (kind == KIND_BLINDS)
        return tr.state.w > 0.5 ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    if (kind == KIND_SLASH)
        return tr.params0.xy;
    vec2 sx = vec2(p.x + 1.0 < tr.art.x ? 1.0 : -1.0, 0.0);
    vec2 sy = vec2(0.0, p.y + 1.0 < tr.art.y ? 1.0 : -1.0);
    return vec2((trField(p + sx) - v) * sx.x, (trField(p + sy) - v) * sy.y);
}

bool trInside(vec2 p){
    return p.x >= 0.0 && p.y >= 0.0 && p.x < tr.art.x && p.y < tr.art.y;
}

// light (rgb) and cover (a, with colour) of an art pixel of a band kind
vec4 trBand(vec2 p, out vec3 light){
    light = vec3(0.0);
    float level = trLevel();
    float v = trField(p) - level;
    vec2 g = trGradient(p, v + level);
    float dominant = max(abs(g.x), abs(g.y));
    float scale = 1.0 / clamp(dominant, 0.45, 3.0);
    int rimWidth = int(tr.rim.x + 0.5);
    if (v >= 0.0){
        // uncovered: the light just outside the edge, in four ramp steps
        float d = v * scale;
        float reach = 2.0 * float(max(rimWidth, 1)) + 2.0;
        if (tr.rim.y > 0.0 && d < reach && tr.state.x > 0.0){
            float q = floor(d / reach * 4.0);
            light = trHot(int(q)) * tr.rim.y * (1.0 - q * 0.25);
        }
        return vec4(0.0);
    }
    // covered: rim when an uncovered pixel lies within `rim` steps toward the edge
    if (rimWidth > 0 && -v * scale <= float(rimWidth) + 1.5){
        vec2 toward = abs(g.x) >= abs(g.y) ? vec2(sign(g.x), 0.0) : vec2(0.0, sign(g.y));
        for (int s = 1; s <= 8; s++){
            if (s > rimWidth)
                break;
            vec2 q = p + toward * float(s);
            if (!trInside(q))
                break;
            if (trField(q) >= level){
                int n = trSteps();
                return vec4(trHot(int(floor(float(s - 1) * float(n) / float(rimWidth)))), 1.0);
            }
        }
    }
    return vec4(tr.ink.rgb, 1.0);
}

vec4 trPixelDissolve(vec2 p, out vec3 light){
    light = vec3(0.0);
    float cell = max(tr.params0.x, 1.0);
    uint seed = trSeed();
    vec2 big = floor(p / cell);
    float h = pfxHash01(uvec2(big), seed);
    float n = h < 0.32 ? 1.0 : (h < 0.74 ? 2.0 : 4.0);
    float size = cell / n;
    vec2 origin = big * cell + floor((p - big * cell) / size) * size;
    vec2 centre = origin + 0.5 * size;
    float key = 0.65 * pfxValueNoise(centre, 48.0, seed + 11u) + 0.35 * pfxValueNoise(centre, 16.0, seed + 13u)
              + 0.22 * pfxHash01(uvec2(origin), seed + 17u) + (n == 1.0 ? 0.12 : (n == 4.0 ? -0.08 : 0.0));
    float rank = clamp((key + 0.08) / 1.42, 0.0, 1.0);
    // each block winks in over 17 % of the coverage, at its rank
    float age = (tr.state.x - rank * 0.83) / 0.17;
    if (age < 0.0)
        return vec4(0.0);
    if (age >= 1.0)
        return vec4(tr.ink.rgb, 1.0);
    vec2 local = floor(p - origin);
    float side = floor(size + 0.5);
    float edge = min(min(local.x, local.y), min(side - 1.0 - local.x, side - 1.0 - local.y));
    if (edge < tr.rim.x)
        return vec4(trHot(int(floor(age * float(trSteps())))), 1.0);
    if (age > 0.2)
        return vec4(tr.ink.rgb, 1.0);
    light = trHot(0) * tr.rim.y * (1.0 - age / 0.2);
    return vec4(0.0);
}

vec4 trCrtOff(vec2 p, out vec3 light){
    light = vec3(0.0);
    float c = tr.state.x;
    bool reveal = int(tr.state.y + 0.5) == 2;
    vec2 size = tr.art.xy;
    vec2 centre = floor(0.5 * size);
    vec2 d = abs(floor(p) - centre);
    // the strip's half height and the line's half length in whole art pixels; the dot's brightness
    float halfRows = 0.0;
    float halfCols = floor(0.5 * size.x);
    float bright = 1.0;
    if (c < 0.55){
        float u = c / 0.55;
        halfRows = floor(0.5 * size.y * (1.0 - u * u * u));
    }else if (c < 0.85){
        float u = (c - 0.55) / 0.3;
        halfCols = floor(0.5 * size.x * (1.0 - u * u));
    }else{
        halfCols = 0.0;
        bright = 1.0 - (c - 0.85) / 0.15;
    }
    int rimWidth = int(tr.rim.x + 0.5);
    if (halfRows >= 1.0){
        if (d.y < halfRows){
            // the squeezed picture: brighter as it narrows (and warming up on the way back),
            // with scanlines on its odd rows, quantised to four steps
            float squeeze = 1.0 - halfRows / (0.5 * size.y);
            float k = 0.55 * squeeze * squeeze + (reveal ? 0.6 * squeeze : 0.0) + (mod(floor(p.y), 2.0) > 0.5 ? 0.08 * squeeze : 0.0);
            k = floor(clamp(k, 0.0, 1.0) * 4.0) * 0.25;
            light = trHot(1) * tr.rim.y * k;
            return vec4(0.0);
        }
        float depth = d.y - halfRows + 1.0;
        if (depth <= float(rimWidth))
            return vec4(trHot(int(floor((depth - 1.0) * float(trSteps()) / float(max(rimWidth, 1))))), 1.0);
        return vec4(tr.ink.rgb, 1.0);
    }
    // the line, then the dot: a white-hot core and its fringe, cooling to ink as the dot fades
    float depth = max(d.y, max(d.x - halfCols, 0.0));
    if (depth <= float(rimWidth)){
        int n = trSteps();
        int rampStep = int(floor(depth * float(n) / float(rimWidth + 1))) + int(floor((1.0 - bright) * float(n)));
        if (rampStep < n)
            return vec4(trHot(rampStep), 1.0);
    }
    return vec4(tr.ink.rgb, 1.0);
}

vec4 trFade(vec2 p){
    float c = tr.state.x;
    if (bayer8(ivec2(floor(p))) >= c)
        return vec4(0.0);
    if (tr.state.w > 0.5){
        int n = trSteps();
        int k = int(floor(c * float(n + 1)));
        if (k < n)
            return vec4(trHot(k), 1.0);
    }
    return vec4(tr.ink.rgb, 1.0);
}

void main(){
    // the screen pixel from the image's top-left, then its art pixel's centre (clamped onto the
    // grid: letterbox bars follow the nearest edge)
    vec2 pixel = v_texcoord * tr.frame.zw;
    if (tr.art.w > 0.5)
        pixel.y = tr.frame.w - pixel.y;
    vec2 p = floor((pixel - tr.frame.xy) / tr.art.z);
    p = clamp(p, vec2(0.0), tr.art.xy - 1.0) + 0.5;

    vec3 light;
    vec4 cover;
    int kind = trKind();
    if (kind == KIND_PIXEL_DISSOLVE)
        cover = trPixelDissolve(p, light);
    else if (kind == KIND_CRT_OFF)
        cover = trCrtOff(p, light);
    else if (kind == KIND_FADE){
        cover = trFade(p);
        light = vec3(0.0);
    }else
        cover = trBand(p, light);
    frag_color = vec4(light * (1.0 - cover.a) + cover.rgb * cover.a, cover.a);
}
