// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MATH_CURVE_H
#define MATH_CURVE_H

// Animation curves and colour gradients with Unity semantics (docs/specs/S20-particles.md D5,
// docs/specs/S27-editor-tools.md R6-R8): the particle effects' MinMaxCurve and MinMaxGradient
// values are made of them, tweens ease along a Curve (EaseType::CURVE), Lua names them
// AnimationCurve / Keyframe / Gradient, and the editor's curve and gradient fields edit them.
// Runtime code bakes them into lookup tables (particles/ParticleLut.h, Curve::bake).

#include "Export.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace jasperengine {

    enum class EaseType;

    // How the editor sets one side's tangent of a key (Unity's AnimationUtility.TangentMode).
    // Evaluation reads only the tangents: a mode is what recomputes them when keys move.
    enum class CurveTangentMode : uint8_t {
        Free,           // as set (dragged or typed)
        Auto,           // smooth through the neighbours (may overshoot)
        Linear,         // towards the neighbour on that side
        Constant,       // a step: infinite tangent, the segment holds its left value
        ClampedAuto     // smooth, flat at extremes and never overshooting the neighbours
    };

    // What a curve does before its first key (pre) and after its last (post): Unity's WrapMode
    enum class CurveWrapMode : uint8_t {
        Clamp,          // the end key's value (Unity "Clamp" / "Clamp Forever")
        Loop,
        PingPong,
        Linear,         // the end key's tangent continued as a line
        LoopWithOffset  // Loop, each cycle offset by the last key's value minus the first's
    };

    // A key of a Curve: tangents are slopes (value units per time unit) on each side of the key.
    // An infinite tangent makes the segment it starts or ends a step (held at the left value).
    struct CurveKey {
        float time = 0.0f;
        float value = 0.0f;
        float inTangent = 0.0f;
        float outTangent = 0.0f;
        // the tangent modes: bits 0-2 the in side's CurveTangentMode, 3-5 the out side's, bit 6
        // "broken" (the sides move independently). 0: Free on both sides, as every key read
        // without a mode, so its tangents stay as stored
        uint8_t mode = 0;

        static constexpr uint8_t kBroken = 1u << 6;

        CurveTangentMode inMode() const { return (CurveTangentMode)(mode & 7u); }
        CurveTangentMode outMode() const { return (CurveTangentMode)((mode >> 3) & 7u); }
        bool broken() const { return (mode & kBroken) != 0; }
        void setInMode(CurveTangentMode m) { mode = (uint8_t)((mode & ~7u) | ((uint8_t)m & 7u)); }
        void setOutMode(CurveTangentMode m) { mode = (uint8_t)((mode & ~(7u << 3)) | (((uint8_t)m & 7u) << 3)); }
        void setBroken(bool b) { mode = (uint8_t)(b ? (mode | kBroken) : (mode & ~kBroken)); }
        // both sides, joined (Auto, Clamped Auto, Free Smooth: Unity's key menu)
        void setBothModes(CurveTangentMode m) { mode = 0; setInMode(m); setOutMode(m); }

        bool operator==(const CurveKey& other) const {
            return time == other.time && value == other.value && inTangent == other.inTangent && outTangent == other.outTangent &&
                   mode == other.mode;
        }
        bool operator!=(const CurveKey& other) const { return !(*this == other); }
    };

    // Keys sorted by time, cubic Hermite between neighbours (the left key's out tangent and the
    // right key's in tangent); outside the keys the wrap modes apply (Clamp: held flat). Evaluation
    // is pure and allocation-free. No key is 0.
    struct JASPERENGINE_API Curve {
        std::vector<CurveKey> keys;
        CurveWrapMode preWrapMode = CurveWrapMode::Clamp;
        CurveWrapMode postWrapMode = CurveWrapMode::Clamp;

        Curve() = default;
        explicit Curve(std::vector<CurveKey> keys): keys(std::move(keys)) {}

        // (a NaN time: the first key)
        float evaluate(float time) const;
        // the slope at `time` (outside the keys: the wrap's, 0 when clamped)
        float derivative(float time) const;
        // out[i] = evaluate(t0 + (t1 - t0) * i / (out.size() - 1)); one entry: evaluate(t0)
        void bake(std::span<float> out, float t0, float t1) const;

        bool empty() const { return keys.empty(); }
        // Sorts the keys by time (stable for equal times)
        void sortKeys();
        // Drops the keys with a non-finite time or value or a NaN tangent, sorts the rest by time
        // (stable) and drops each key at the time of the one before it (the first stays): what a
        // curve read from a lenient source (upstream's lines-and-trails files, Unity) becomes
        void normalize();
        // smallest and largest value the curve takes between its first and last key
        void valueRange(float& lo, float& hi) const;
        // ... over [minTime, maxTime], wraps included (the keys inside it and 64 samples)
        void valueRange(float minTime, float maxTime, float& lo, float& hi) const;
        // the first and last key's times (0 without keys)
        float firstTime() const { return keys.empty() ? 0.0f : keys.front().time; }
        float lastTime() const { return keys.empty() ? 0.0f : keys.back().time; }
        // the same value everywhere: every key's value equal and every segment flat (flat
        // tangents at both of its ends, or a step)
        bool isConstant() const;

        // ---- key editing (Unity's AnimationCurve API). Indices are 0-based.

        // A key at (time, value), its tangents smoothed (Clamped Auto) with its neighbours'
        // modes applied; -1 when a key already sits at that time (or a value is not finite)
        int addKey(float time, float value);
        // `key` inserted in time order (its tangents as given unless its modes recompute them);
        // -1 when a key already sits at that time, or its time or value is not finite or a
        // tangent is NaN
        int addKey(const CurveKey& key);
        // Replaces key `index` by `key` and returns its new index. When another key sits at
        // key.time, the old key's time is kept (Unity's MoveKey). -1 for a bad index or a key
        // addKey refuses (the curve unchanged).
        int moveKey(size_t index, const CurveKey& key);
        // false for a bad index
        bool removeKey(size_t index);
        // Unity's SmoothTangents: in = out = the slopes to the neighbours blended by `weight`
        // (-1 the left one, 0 their mean, 1 the right one); the end keys point at their neighbour
        void smoothTangents(size_t index, float weight);
        // Recomputes the tangents its modes set for key `index` and its two neighbours (whose
        // Auto and Linear sides depend on it)
        void updateTangents(size_t index);
        void updateAllTangents();

        // ---- constructors

        // one key: the value everywhere
        static Curve constant(float value);
        // Unity's AnimationCurve.Constant: keys at timeStart and timeEnd with `value`, flat
        static Curve constant(float timeStart, float timeEnd, float value);
        // a line from (0, from) to (1, to)
        static Curve linear(float from, float to);
        // Unity's AnimationCurve.Linear: a line from (timeStart, valueStart) to (timeEnd, valueEnd)
        // (the keys in time order when timeEnd comes first)
        static Curve linear(float timeStart, float valueStart, float timeEnd, float valueEnd);
        // linear segments through (time, value) points, tangents along the segments
        static Curve polyline(const std::vector<std::pair<float, float>>& points);
        // the ease-in-out of Unity's AnimationCurve.EaseInOut: flat tangents at both ends (keys in
        // time order)
        static Curve easeInOut(float timeStart, float valueStart, float timeEnd, float valueEnd);
        // An ease over [0, 1] as keys (the ends, then a key where the curve is furthest from the
        // ease, each with the ease's slopes on both sides; a jump becomes a step): `keys` keys,
        // and more (up to 64) while the curve is further than 0.005 from the ease anywhere (the
        // elastic eases need about 24). LINEAR is two keys; CUSTOM and CURVE have no function of
        // their own (a linear curve).
        static Curve fromEase(EaseType type, int keys = 16);
        // fromEase's sampler over any function of [0, 1]: exactly `keys` keys (fewer when it is
        // exact sooner), then more up to `maxKeys` while the error is above `tolerance` (0: none)
        static Curve fromFunction(const std::function<float(float)>& f, int keys = 16, float tolerance = 0.0f, int maxKeys = 64);

        bool operator==(const Curve& other) const {
            return keys == other.keys && preWrapMode == other.preWrapMode && postWrapMode == other.postWrapMode;
        }
        bool operator!=(const Curve& other) const { return !(*this == other); }
    };

    // Unity's names (C++ and Lua)
    using AnimationCurve = Curve;
    using Keyframe = CurveKey;

    enum class GradientMode : uint8_t {
        Blend,  // linear between keys
        Fixed   // each key's value holds from the key before it up to the key itself
    };

    struct GradientColorKey {
        float time = 0.0f;
        Vector3 color = Vector3(1.0f, 1.0f, 1.0f);

        bool operator==(const GradientColorKey& other) const { return time == other.time && color == other.color; }
        bool operator!=(const GradientColorKey& other) const { return !(*this == other); }
    };

    struct GradientAlphaKey {
        float time = 0.0f;
        float alpha = 1.0f;

        bool operator==(const GradientAlphaKey& other) const { return time == other.time && alpha == other.alpha; }
        bool operator!=(const GradientAlphaKey& other) const { return !(*this == other); }
    };

    // Up to kMaxKeys colour and kMaxKeys alpha keys over [0, 1]. Colours are sRGB-encoded as an
    // artist picks them; by default the gradient blends them there (Unity's look), `linear`
    // blends them in linear space instead. evaluate() returns linear RGB and alpha.
    struct JASPERENGINE_API Gradient {
        static constexpr size_t kMaxKeys = 8;

        std::vector<GradientColorKey> colorKeys;
        std::vector<GradientAlphaKey> alphaKeys;
        GradientMode mode = GradientMode::Blend;
        bool linear = false;

        // linear RGB, alpha (allocation-free)
        Vector4 evaluate(float time) const;
        // out[i] = evaluate(i / (out.size() - 1)); one entry: evaluate(0)
        void bake(std::span<Vector4> out) const;
        // the colour as it is blended (sRGB unless linear) before the conversion to linear
        Vector3 evaluateEncoded(float time) const;
        float evaluateAlpha(float time) const;

        void sortKeys();
        // Drops the keys with a non-finite time or value, clamps the times to [0, 1], sorts them
        // (stable) and keeps at most kMaxKeys of each (the first kMaxKeys - 1 by time and the
        // last, so both ends stay): what a gradient read from a lenient source becomes
        void normalize();

        // a single colour (sRGB) and alpha
        static Gradient constant(const Vector3& color, float alpha = 1.0f);
        static Gradient twoColors(const Vector3& from, const Vector3& to, float alphaFrom = 1.0f, float alphaTo = 1.0f);

        bool operator==(const Gradient& other) const {
            return colorKeys == other.colorKeys && alphaKeys == other.alphaKeys && mode == other.mode && linear == other.linear;
        }
        bool operator!=(const Gradient& other) const { return !(*this == other); }
    };

}

#endif // MATH_CURVE_H
