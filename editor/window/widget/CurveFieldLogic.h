// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The curve field's pure part (lines and trails spec 6.1, "Editor widgets"): the editor canvas'
// mapping between (time, value) and screen pixels, zoom to fit, hit testing of keys and tangent
// handles, key insertion / deletion / moves that keep the list sorted, Unity's per-key tangent
// modes (Auto, Linear, Constant, Free) and the preset curves. No ImGui; the widget is
// CurveField.h. gtested (editor/tests/CurveFieldLogicTest.cpp). Main thread.
//
// Auto is a property of the tangents, not a stored flag (CurveKey has none): a key whose
// tangents equal autoTangent() is Auto, and insertKey / deleteKey / moveKey recompute the Auto
// keys around the edit so they stay Auto. Free (broken) has no stored flag either: a key is
// broken when its tangents differ; the widget keeps a per-key flag for Free keys whose two
// tangents happen to be equal (setFree).

#pragma once

#include "math/AnimationCurve.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jasperengine::editor::curvefield{

    struct Point{
        float x = 0.0f, y = 0.0f;   // screen pixels
    };

    struct Rect{
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;   // top-left and size, pixels
    };

    // the canvas rect showing [timeMin, timeMax] left to right and [valueMin, valueMax]
    // bottom to top (screen y grows downwards)
    struct View{
        Rect canvas;
        float timeMin = 0.0f, timeMax = 1.0f;
        float valueMin = 0.0f, valueMax = 1.0f;

        float timeToX(float time) const;
        float valueToY(float value) const;
        float xToTime(float x) const;
        float yToValue(float y) const;
        Point toScreen(float time, float value) const;
        // pixels per time unit and per value unit (never 0)
        float pixelsPerTime() const;
        float pixelsPerValue() const;
    };

    // The view zoomed to the keys' times and the values the curve takes between them (sampled,
    // overshoot included), `timeMargin` / `valueMargin` of each range added at both ends. A
    // single key or no keys show the times [0, 1] (or [t, t + 1]); a flat curve gets a value
    // range of max(1, |v|) around its value. `second` (optional) is fitted together with `curve`.
    View fitView(const AnimationCurve& curve, const Rect& canvas, const AnimationCurve* second = nullptr,
                 float timeMargin = 0.05f, float valueMargin = 0.1f);

    // zooms by `factor` (> 1 zooms in) on either axis, keeping the point under `anchor` still
    View zoomView(const View& view, Point anchor, float timeFactor, float valueFactor);
    // moves the view by a mouse delta in pixels (content follows the mouse)
    View panView(const View& view, float dxPx, float dyPx);

    Point keyPosition(const View& view, const CurveKey& key);

    // the key nearest to `mouse` within `radiusPx`, -1 when none
    int hitKey(const AnimationCurve& curve, const View& view, Point mouse, float radiusPx);

    enum class TangentSide : uint8_t{ In = 0, Out = 1 };

    struct TangentHit{
        int key = -1;
        TangentSide side = TangentSide::In;
    };

    // a key shows the handle of a side when a segment is on that side (In: not the first key,
    // Out: not the last) and the tangent is finite (step tangents have no handle)
    bool hasTangentHandle(const AnimationCurve& curve, size_t key, TangentSide side);
    // the handle's end: `lengthPx` pixels from the key along the tangent's screen direction
    // (In to the left, Out to the right)
    Point tangentHandlePosition(const AnimationCurve& curve, size_t key, TangentSide side, const View& view, float lengthPx);
    // the handle of `key` (the selected one; -1: none) under `mouse` within `radiusPx`
    TangentHit hitTangentHandle(const AnimationCurve& curve, const View& view, int key, Point mouse, float radiusPx, float lengthPx);

    // a key edits its two tangents separately when they differ
    bool isBroken(const CurveKey& key);

    // Unity's ClampedAuto slope at a key: (v[i+1] - v[i-1]) / (t[i+1] - t[i-1]), 0 at a local
    // extremum and at the first and last keys, limited to 3x the smaller neighbouring secant so
    // the segments never overshoot
    float autoTangent(const AnimationCurve& curve, size_t index);

    enum class TangentMode : uint8_t{ Auto = 0, Linear = 1, Constant = 2, Free = 3 };

    // both tangents autoTangent()
    void setAuto(AnimationCurve& curve, size_t index);
    // in = the slope from the previous key, out = the slope to the next key (an end key takes
    // its one slope on both sides; a single key 0)
    void setLinear(AnimationCurve& curve, size_t index);
    // both tangents AnimationCurve::STEP: the segments on both sides hold the value before them
    void setConstant(AnimationCurve& curve, size_t index);
    // tangents unchanged; the key edits broken from now on: broken[index] = 1 (`broken` grown to
    // the key count). The caller keeps the flags (CurveKey has no field for it)
    void setFree(const AnimationCurve& curve, size_t index, std::vector<char>& broken);
    // setAuto / setLinear / setConstant by mode (Free: nothing)
    void setMode(AnimationCurve& curve, size_t index, TangentMode mode);

    // the mode the key's tangents match: Constant (both step), Auto, Linear, else Free
    TangentMode detectMode(const AnimationCurve& curve, size_t index);

    // A key at `time` on the curve (value = curve.evaluate(time)) with Auto tangents: its index,
    // -1 when a key is already at that time. The Auto keys around it are recomputed
    int insertKey(AnimationCurve& curve, float time);
    // removes key `index` (false when out of range or it is the only key: the editor keeps one);
    // the Auto keys around it are recomputed
    bool deleteKey(AnimationCurve& curve, size_t index);
    // moves key `index` to (time, value), tangents kept: the index it ends at after re-sorting.
    // When another key is at `time` only the value changes. The Auto keys (the moved one and its
    // old and new neighbours) are recomputed. -1 when out of range or not finite
    int moveKey(AnimationCurve& curve, size_t index, float time, float value);

    // Sets the tangent of `side` from the handle dragged to `handle` (screen pixels): the slope
    // of the line from the key to the handle (a handle dragged past the key's vertical keeps a
    // steep finite slope). Unless `broken`, the other side takes the same slope. False when out
    // of range
    bool dragTangent(AnimationCurve& curve, size_t key, TangentSide side, Point handle, const View& view, bool broken);

    enum class CurvePreset : uint8_t{ Constant = 0, LinearUp, LinearDown, EaseIn, EaseOut, EaseInOut };
    constexpr int CURVE_PRESET_COUNT = 6;

    const char* presetName(CurvePreset preset);
    // a preset over the times [0, 1] between `low` and `high` (Constant: flat at `high`;
    // EaseIn: flat start, ends at slope 2 (high - low) (t^2); EaseOut the mirror; EaseInOut flat at
    // both ends (smoothstep))
    AnimationCurve presetCurve(CurvePreset preset, float low = 0.0f, float high = 1.0f);
    // a preset keeping the value range of `current` (its keys' smallest and largest values; a
    // flat curve at v: [min(0, v), max(0, v)], [0, 1] when v is 0) and its wrap modes
    AnimationCurve presetCurve(CurvePreset preset, const AnimationCurve& current);

}
