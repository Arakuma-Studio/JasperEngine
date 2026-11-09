// The pixel-snap rule (docs/specs/S41-pixel-perfect-camera.md §6.2), the GPU twin of
// engine/core/pixelart/PixelSnap.h: the same float32 operations in the same order, so the CPU
// predicts the pixel a vertex lands on. Self-contained: include it from any vertex stage of a
// pixel-snapped variant (PIXEL_SNAP, the 'Pxs' bit) after the frame block (FRAME_VS,
// includes/frame.glsl), whose members it reads:
//
//   frame.pixelSnap        xy T / 2 (half the drawing target, base pixels), z flipY (-1 when the
//                          pass's matrices flip Y, GL render targets), w k (screen pixels per
//                          base pixel)
//   frame.pixelSnapOffset  xy T / 2 minus the sub-pixel smoothing offset of a direct pass (where
//                          NDC 0 lands, base pixels), z 1 when the pass's view is on the grid (no
//                          camera roll), w the rounding bias 2^-7
// (pixelart::snapTerms computes both on the CPU, which uses the same values.)
//
// Space: base pixels of the target, x right, y up from its bottom-left corner, whatever the
// backend's flip. Order in a vertex stage: every displacement hook, the model, the MVP, then
// the snap on gl_Position, then the output (the depth program of the variant snaps identically).
// A draw whose 2D linear part is a signed permutation in pixels (0, 90, 180, 270 degrees, flips)
// snaps every vertex (pixelSnapClip); any other snaps its pivot (pixelSnapCenterDelta, added to
// every vertex): the engine decides per draw (pixelart::isGridAligned) and passes it as the mode
// of pixelSnapApply. No caller rounds with floor(p + 0.5) without the bias.

#ifndef PIXEL_SNAP_GLSL
#define PIXEL_SNAP_GLSL

vec2 pixelSnapToPixels(vec4 clip){
    vec2 ndc = clip.xy / clip.w * vec2(1.0, frame.pixelSnap.z);
    return ndc * frame.pixelSnap.xy + frame.pixelSnapOffset.xy;
}

vec4 pixelSnapFromPixels(vec2 p, vec4 clip){
    vec2 ndc = (p - frame.pixelSnapOffset.xy) / frame.pixelSnap.xy * vec2(1.0, frame.pixelSnap.z);
    return vec4(ndc * clip.w, clip.zw);
}

// floor(p + 0.5 + 2^-7) on both axes: the bias keeps the half-pixel lattice away from rounding
// differences (frame.pixelSnapOffset.w; the engine uploads 2^-7)
vec2 pixelSnapRound2(vec2 p){
    return floor(p + 0.5 + frame.pixelSnapOffset.w);
}

// one coordinate (a caller rounding a single value, as S49's pixel box)
float pixelSnapRound(float p){
    return floor(p + 0.5 + frame.pixelSnapOffset.w);
}

// per vertex: the position on the nearest base pixel corner
vec4 pixelSnapClip(vec4 clip){
    return pixelSnapFromPixels(pixelSnapRound2(pixelSnapToPixels(clip)), clip);
}

// the clip-space delta that puts a pivot on the grid (added to every vertex of the draw)
vec4 pixelSnapCenterDelta(vec4 clipCenter){
    return pixelSnapFromPixels(pixelSnapRound2(pixelSnapToPixels(clipCenter)), clipCenter) - clipCenter;
}

// the unit axis of the 90-degree step nearest the rotated x axis (pixelart::snapRotationStep):
// ties at 45 degrees go to the step nearer 0 degrees
vec2 pixelSnapRotationStep(vec2 axisX){
    vec2 a = abs(axisX);
    bool useX = a.x > a.y || (a.x == a.y && axisX.x > 0.0);
    return useX ? vec2(sign(axisX.x), 0.0) : vec2(0.0, sign(axisX.y));
}

// an instance's mode (S52 D8): every vertex only when its draw's mode is 1 and the instance's 2D
// linear part (the first two columns of its matrix) is a signed permutation written with exact
// zeros, as the engine's packers write a grid-aligned instance; any other instance of a snapped
// draw snaps its pivot. Exact zeros compare exactly on every backend
float pixelSnapInstanceMode(float drawMode, vec4 col1, vec4 col2){
    bool aligned = (col1.y == 0.0 && col2.x == 0.0) || (col1.x == 0.0 && col2.y == 0.0);
    if (drawMode > 0.5 && drawMode < 1.5)
        return aligned ? 1.0 : 2.0;
    return drawMode;
}

// a draw's snap by its mode (the engine's per-draw pixelSnapDraw.x): 0 none (a pass without a
// Pixel Perfect Camera, a shadow map), 1 every vertex, 2 the pivot (clipPivot: the model origin)
vec4 pixelSnapApply(vec4 clip, float mode, vec4 clipPivot){
    if (mode > 1.5)
        return clip + pixelSnapCenterDelta(clipPivot);
    if (mode > 0.5)
        return pixelSnapClip(clip);
    return clip;
}

#endif
