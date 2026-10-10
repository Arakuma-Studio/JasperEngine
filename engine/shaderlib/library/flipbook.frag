#version 450

// @shader mesh
// @name "Flipbook"
// @category "Effects"
// @description "Sprite-sheet animation on a mesh: plays the frames of a columns x rows sheet at a frame rate, looping or holding the last frame, optionally crossfading between frames. Alpha blended by default; alpha clip for opaque cutouts."
// @require uv1 color
// @state blend=alpha
// @property tint hdrcolor (1, 1, 1, 1) "Tint"
// @property columns int 4 "Columns"
// @property rows int 4 "Rows"
// @property fps float 12 "Frames per second"
// @property startFrame int 0 "First frame"
// @property frameCount int 0 "Frame count (0: to the end of the sheet)"
// @property loop toggle 1 "Loop (off: hold the last frame)"
// @property startTime float 0 "Start time (seconds of the engine clock, Lua Engine.systemTime)"
// @property alphaCutoff range(0, 1) 0.5 "Alpha cutoff" feature=ALPHA_CLIP
// @texture u_sheetTexture white "Sprite sheet"
// @feature FRAME_BLEND "Crossfade frames"
// @feature ALPHA_CLIP "Alpha clip"
// @feature VERTEX_COLOR "Vertex colour"
// @depth

// The frame is computed per vertex (library/flipbook.glsl) from the engine clock: frame
// startFrame + (time - startTime) * fps, wrapped over frameCount frames or held on the
// last one. Frames run left to right, then top to bottom, and fill the mesh's UV square.
// To restart a one-shot animation, a script sets startTime to Engine.systemTime.

uniform u_fs_customParams {
    vec4 tint;
    float alphaCutoff;
} customParams;

uniform texture2D u_sheetTexture;
uniform sampler u_sheet_smp;

in vec4 v_frameUV; // xy this frame's UV, zw the next frame's (library/flipbook.glsl)
#ifdef FRAME_BLEND
    in float v_frameBlend;
#endif
in vec4 v_color; // the instance colour, times the vertex colour with VERTEX_COLOR

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    vec4 sheet = texture(sampler2D(u_sheetTexture, u_sheet_smp), v_frameUV.xy);
    #ifdef FRAME_BLEND
        sheet = mix(sheet, texture(sampler2D(u_sheetTexture, u_sheet_smp), v_frameUV.zw), v_frameBlend);
    #endif
    vec4 color = customParams.tint * sRGBToLinear(sheet) * v_color;

    #ifdef ALPHA_CLIP
        // the same test as flipbook_depth.frag
        if (color.a < customParams.alphaCutoff){
            discard;
        }
    #endif

    g_finalColor = finalizeColor(color);
}
