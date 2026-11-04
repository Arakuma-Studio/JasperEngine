// The pixel-snap rule (docs/specs/S41-pixel-perfect-camera.md §6.2), the GPU twin of
// engine/core/pixelart/PixelSnap.h: the same float32 operations in the same order, so the CPU
// predicts the pixel a vertex lands on. Self-contained: include it from any vertex stage of a
// pixel-snapped variant (PIXEL_SNAP, the 'Pxs' bit) after the frame block (FRAME_VS,
// includes/frame.glsl), whose members it reads:
//
//   frame.pixelSnap        xy T (the drawing target in base pixels), z flipY (-1 when the pass's
//                          matrices flip Y, GL render targets), w k (screen pixels per base pixel)
//   frame.pixelSnapOffset  xy the sub-pixel smoothing offset of a direct pass (base pixels),
//                          z 1 when the pass's view is on the grid (no camera roll), w 0
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
    vec2 ndc = clip.xy / clip.w;
    ndc.y *= frame.pixelSnap.z;
    return (ndc * 0.5 + 0.5) * frame.pixelSnap.xy - frame.pixelSnapOffset.xy;
}

vec4 pixelSnapFromPixels(vec2 p, vec4 clip){
    vec2 ndc = (p + frame.pixelSnapOffset.xy) / frame.pixelSnap.xy * 2.0 - 1.0;
    ndc.y *= frame.pixelSnap.z;
    return vec4(ndc * clip.w, clip.zw);
}

// floor(p + 0.5 + 2^-7): the bias keeps the half-pixel lattice away from rounding differences
float pixelSnapRound(float p){
    return floor(p + 0.5 + 0.0078125);
}

// per vertex: the position on the nearest base pixel corner
vec4 pixelSnapClip(vec4 clip){
    vec2 p = pixelSnapToPixels(clip);
    return pixelSnapFromPixels(vec2(pixelSnapRound(p.x), pixelSnapRound(p.y)), clip);
}

// the clip-space delta that puts a pivot on the grid (added to every vertex of the draw)
vec4 pixelSnapCenterDelta(vec4 clipCenter){
    vec2 p = pixelSnapToPixels(clipCenter);
    return pixelSnapFromPixels(vec2(pixelSnapRound(p.x), pixelSnapRound(p.y)), clipCenter) - clipCenter;
}

// the unit axis of the 90-degree step nearest the rotated x axis (pixelart::snapRotationStep):
// ties at 45 degrees go to the step nearer 0 degrees
vec2 pixelSnapRotationStep(vec2 axisX){
    vec2 a = abs(axisX);
    bool useX = a.x > a.y || (a.x == a.y && axisX.x > 0.0);
    return useX ? vec2(sign(axisX.x), 0.0) : vec2(0.0, sign(axisX.y));
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
