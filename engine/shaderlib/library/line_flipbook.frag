#version 450

// @shader lines
// @name "Line Flipbook"
// @category "Effects"
// @description "Animated sprite sheet along a Line or Trail Renderer: lightning, lasers, energy beams. Plays the frames of a columns x rows sheet at a frame rate (left to right, then top to bottom), each frame stretched over the line as its texture mode lays out u, optionally scrolling, times a tint and the line's colour gradient. Additive by default (set the component's blend mode to alpha for an opaque-looking beam); writes no depth. Needs a width: a hairline has no texture coordinates."
// @require uv1 color
// @state blend=additive zwrite=off
// @property tint hdrcolor (1, 1, 1, 1) "Tint"
// @property columns float 1 "Columns"
// @property rows float 8 "Rows"
// @property fps float 30 "Frames per second (negative: backwards)"
// @property scroll vector4 (0, 0, 0, 0) "UV scroll per second (xy)"
// @texture u_baseTexture white "Sprite sheet"
// @feature VERTEX_COLOR "Vertex colour (the line's colour gradient)" default=on

// The frame is floor(time x fps) wrapped over columns x rows (time: the engine's seconds since
// startup, the "time" member the engine writes), so every line with this shader animates in
// step. u runs along the line by the component's texture mode (Stretch: once over the line;
// Tile and the per-segment modes repeat the frame), v across it; scroll moves both before the
// frame is picked, in frames' UV per second. The B lightning of the Unity game: columns 1,
// rows 8, fps 30, Stretch, additive.

uniform u_fs_customParams {
    float time; // engine-written seconds
    vec4 tint;
    float columns;
    float rows;
    float fps;
    vec4 scroll;
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

out vec4 g_finalColor;

#include "includes/lines_fs.glsl"
#include "includes/output.glsl"

// the coordinate inside one frame: a repeat of u (Tile, the per-segment modes) starts the
// frame again; the end of one repeat (u exactly 1, 2...) is its last texel, not the next start
float flipbookRepeat(float value){
    float wrapped = fract(value);
    return (wrapped == 0.0 && value > 0.0) ? 1.0 : wrapped;
}

void main() {
    vec2 grid = max(floor(vec2(customParams.columns, customParams.rows) + 0.5), vec2(1.0));
    float total = grid.x * grid.y;
    float position = mod(floor(customParams.time * customParams.fps), total);
    // a negative fps counts down: mod keeps it in 0..total-1
    float row = floor(position / grid.x);
    float column = position - row * grid.x;

    vec2 uv = lineUV() + customParams.scroll.xy * customParams.time;
    vec2 local = vec2(flipbookRepeat(uv.x), clamp(flipbookRepeat(uv.y), 0.0, 1.0));
    vec2 frameUV = (local + vec2(column, row)) / grid;

    vec4 color = customParams.tint * sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), frameUV));
    #ifdef VERTEX_COLOR
        color *= lineVertexColor();
    #endif

    g_finalColor = finalizeStraightColor(color);
}
