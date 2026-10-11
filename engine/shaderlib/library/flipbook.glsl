// The frame of library:flipbook, computed per vertex and shared by its colour
// (flipbook.vert) and depth (flipbook_depth.vert) programs: out vec4 v_frameUV (xy the
// frame's UV, zw the next frame's) and, with FRAME_BLEND, out float v_frameBlend (how far
// towards the next frame). Include after the surface include.

#define FRAME_VS
#include "includes/frame.glsl"

uniform u_vs_customParams {
    float columns;
    float rows;
    float fps;
    float startFrame;
    float frameCount;
    float loop;
    float startTime;
} customParams;

out vec4 v_frameUV;
#ifdef FRAME_BLEND
    out float v_frameBlend;
#endif

// the UV of a frame of the sheet: frames run left to right, then top to bottom
vec2 flipbookFrameUV(vec2 uv, float index, vec2 grid){
    float row = floor(index / grid.x);
    float column = index - row * grid.x;
    return (clamp(uv, 0.0, 1.0) + vec2(column, row)) / grid;
}

// called from the stage's surfaceDisplace hook, which moves nothing
void flipbookVaryings(vec2 uv){
    vec2 grid = max(floor(vec2(customParams.columns, customParams.rows) + 0.5), vec2(1.0));
    float total = grid.x * grid.y;
    float first = clamp(floor(customParams.startFrame + 0.5), 0.0, total - 1.0);
    // frameCount 0 (or less): every frame from the first to the end of the sheet
    float count = total - first;
    if (customParams.frameCount >= 1.0){
        count = min(floor(customParams.frameCount + 0.5), count);
    }

    // a negative fps plays backwards
    float elapsed = max(frame.time.x - customParams.startTime, 0.0) * customParams.fps;
    float position;
    if (customParams.loop > 0.5){
        // wrapped in cycles first, so a long run keeps the crossfade's precision
        position = fract(elapsed / count) * count;
    }else{
        // holds the last frame (backwards: from the last frame, holding the first)
        position = clamp((customParams.fps < 0.0) ? count - 1.0 + elapsed : elapsed, 0.0, count - 1.0);
    }
    float index = min(floor(position), count - 1.0);
    float next = (customParams.loop > 0.5) ? mod(index + 1.0, count) : min(index + 1.0, count - 1.0);

    v_frameUV = vec4(flipbookFrameUV(uv, first + index, grid), flipbookFrameUV(uv, first + next, grid));
    #ifdef FRAME_BLEND
        v_frameBlend = position - index;
    #endif
}
