// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The curve editor's model (docs/specs/S27-editor-tools.md R8, R10, AC15), without ImGui: the
// view transform, hit testing of keys, tangent handles and curves, and every edit the editor
// makes (add on a curve or at a point, move, delete, tangent modes, tangent drags, clamp to the
// range, copy and paste, framing). CurveEditor.h draws it and turns input into these calls.

#include "math/Curve.h"

#include <cstddef>
#include <vector>

namespace jasperengine::editor::curveops {

    // Unity's `ranges` argument of a curve field: the rect keys are kept in when clamp is set
    // (and the rect drawn), and what a normalised [0,1]x[0,1] preset is mapped onto
    struct CurveRange {
        float t0 = 0.0f, v0 = 0.0f, t1 = 1.0f, v1 = 1.0f;
        bool clamp = false;
    };

    // The visible window of the graph: curve space [t0, t1] x [v0, v1] onto pixels [x0, x1] x
    // [y0, y1] (y down: v1 at y0)
    struct View {
        float t0 = 0.0f, t1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
        float x0 = 0.0f, y0 = 0.0f, x1 = 1.0f, y1 = 1.0f;

        float toX(float t) const { return x0 + (t - t0) / (t1 - t0) * (x1 - x0); }
        float toY(float v) const { return y1 - (v - v0) / (v1 - v0) * (y1 - y0); }
        float toTime(float x) const { return t0 + (x - x0) / (x1 - x0) * (t1 - t0); }
        float toValue(float y) const { return v0 + (y1 - y) / (y1 - y0) * (v1 - v0); }
        // pixels per unit, x and y
        float scaleX() const { return (x1 - x0) / (t1 - t0); }
        float scaleY() const { return (y1 - y0) / (v1 - v0); }
    };

    struct KeyRef {
        int curve = -1;
        int key = -1;
        bool operator==(const KeyRef& o) const { return curve == o.curve && key == o.key; }
        bool operator<(const KeyRef& o) const { return curve != o.curve ? curve < o.curve : key < o.key; }
    };

    enum class Side : uint8_t { In, Out, Both };

    struct Hit {
        enum class Kind : uint8_t { None, Key, InHandle, OutHandle, Curve } kind = Kind::None;
        int curve = -1;
        int key = -1;
        float time = 0.0f;   // Curve: the time under the point
    };

    // the end of a tangent handle of `key` (on `side`, In or Out): `length` pixels along the
    // tangent from the key (a step tangent's handle lies flat)
    void handlePosition(const CurveKey& key, Side side, const View& view, float length, float& x, float& y);

    // What is under (x, y): a tangent handle of a selected key first, then a key, then a curve
    // (within `radius` pixels). Only `editable` curves are hit.
    Hit hitTest(const std::vector<const Curve*>& curves, const std::vector<bool>& editable, const View& view, float x, float y,
                float radius, float handleLength, const std::vector<KeyRef>& selection);

    // a key on the curve at `time` (its value there, Clamped Auto, the neighbours following);
    // -1 when a key sits at that time
    int addKeyOnCurve(Curve& curve, float time, const CurveRange* range);
    // a key at (time, value), Clamped Auto; -1 when a key sits at that time
    int addKeyAt(Curve& curve, float time, float value, const CurveRange* range);

    // the selected keys removed (a curve keeps at least one key); the selection is emptied
    void deleteKeys(std::vector<Curve*>& curves, std::vector<KeyRef>& selection);

    // The selected keys moved by (dt, dv) from where they were (`original`: the curves when the
    // drag began), clamped to the range; the selection follows the keys' new indices. A key that
    // would land on another key's time keeps its old time.
    void moveKeys(std::vector<Curve*>& curves, const std::vector<Curve>& original, std::vector<KeyRef>& selection, float dt, float dv,
                  const CurveRange* range);

    // A key's tangent mode on one side or both (Unity's Left / Right / Both Tangents), the
    // tangents recomputed. On one side the key becomes broken.
    void setTangentMode(Curve& curve, size_t key, Side side, CurveTangentMode mode);
    // Unity's key menu: Clamped Auto, Auto, Free Smooth (Free, joined), Flat (Free, slope 0),
    // Broken (the sides apart)
    void setClampedAuto(Curve& curve, size_t key);
    void setAuto(Curve& curve, size_t key);
    void setFreeSmooth(Curve& curve, size_t key);
    void setFlat(Curve& curve, size_t key);
    void setBroken(Curve& curve, size_t key, bool broken);

    // A handle dragged to slope `slope`: that side becomes Free (and, unbroken, the other side
    // takes the same slope)
    void dragTangent(Curve& curve, size_t key, Side side, float slope);
    // the slope a handle dragged to (x, y) gives (`side` In: the handle lies left of the key)
    float slopeTowards(const CurveKey& key, Side side, const View& view, float x, float y);

    // the keys' times and values kept in the range (when it clamps)
    void clampToRange(Curve& curve, const CurveRange& range);

    // a curve over [0,1]x[0,1] mapped onto the range, and back (tangents scaled with it)
    Curve remap(const Curve& normalised, const CurveRange& range);
    Curve normalise(const Curve& curve, const CurveRange& range);

    // Align (S91 R23): the selected keys take the mean of their values, a level line through them
    void alignKeys(std::vector<Curve*>& curves, const std::vector<KeyRef>& selection);
    // Smooth: each selected key Free and joined, its slope the mean of the slopes to its
    // neighbours (Unity's SmoothTangents, weight 0); Sharpen: Linear on both sides, a corner
    void smoothKeys(std::vector<Curve*>& curves, const std::vector<KeyRef>& selection);
    void sharpenKeys(std::vector<Curve*>& curves, const std::vector<KeyRef>& selection);
    // the selected keys of one curve, their times relative to the first; pasted at `time`
    std::vector<CurveKey> copyKeys(const Curve& curve, const std::vector<KeyRef>& selection, int curveIndex);
    // the pasted keys' indices (a key landing on an existing key's time replaces it)
    std::vector<int> pasteKeys(Curve& curve, const std::vector<CurveKey>& keys, float time, const CurveRange* range);

    // the view zoomed around the pixel (x, y), which stays over the same point: `timeFactor` and
    // `valueFactor` scale the spans (below 1 closer, 1 that axis unchanged)
    View zoom(const View& view, float x, float y, float timeFactor, float valueFactor);
    // the view dragged by (dx, dy) pixels: the graph follows the mouse
    View pan(const View& view, float dx, float dy);

    // A view showing the keys (the selected ones, or all when none is) with a margin of
    // `margin` of the size on each side, and at least `minSpan` in each axis
    View frame(const std::vector<const Curve*>& curves, const std::vector<KeyRef>& selection, const View& pixels, float margin,
               float minSpan);

}
