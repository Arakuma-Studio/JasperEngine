// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MATH_CURVE_H
#define MATH_CURVE_H

// Animation curves and colour gradients with Unity semantics (docs/specs/S20-particles.md D5):
// the particle effects' MinMaxCurve and MinMaxGradient values are made of them, and a curve
// editor edits them. Runtime code bakes them into lookup tables (particles/ParticleLut.h).

#include "Export.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include <cstdint>
#include <vector>

namespace jasperengine {

    // A key of a Curve: tangents are slopes (value units per time unit) on each side of the key.
    // An infinite tangent makes the segment it starts or ends a step (held at the left value).
    struct CurveKey {
        float time = 0.0f;
        float value = 0.0f;
        float inTangent = 0.0f;
        float outTangent = 0.0f;

        bool operator==(const CurveKey& other) const {
            return time == other.time && value == other.value && inTangent == other.inTangent && outTangent == other.outTangent;
        }
        bool operator!=(const CurveKey& other) const { return !(*this == other); }
    };

    // Keys sorted by time, cubic Hermite between neighbours (the left key's out tangent and the
    // right key's in tangent), held flat before the first key and after the last. No key is 0.
    struct JASPERENGINE_API Curve {
        std::vector<CurveKey> keys;

        Curve() = default;
        explicit Curve(std::vector<CurveKey> keys): keys(std::move(keys)) {}

        float evaluate(float time) const;
        // the slope at `time` (0 outside the keys)
        float derivative(float time) const;

        bool empty() const { return keys.empty(); }
        // Sorts the keys by time (stable for equal times)
        void sortKeys();
        // smallest and largest value the curve takes between its first and last key
        void valueRange(float& lo, float& hi) const;

        // one key: the value everywhere
        static Curve constant(float value);
        // a line from (0, from) to (1, to)
        static Curve linear(float from, float to);
        // linear segments through (time, value) points, tangents along the segments
        static Curve polyline(const std::vector<std::pair<float, float>>& points);
        // the ease-in-out of Unity's AnimationCurve.EaseInOut: flat tangents at both ends
        static Curve easeInOut(float timeStart, float valueStart, float timeEnd, float valueEnd);

        bool operator==(const Curve& other) const { return keys == other.keys; }
        bool operator!=(const Curve& other) const { return keys != other.keys; }
    };

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

        // linear RGB, alpha
        Vector4 evaluate(float time) const;
        // the colour as it is blended (sRGB unless linear) before the conversion to linear
        Vector3 evaluateEncoded(float time) const;
        float evaluateAlpha(float time) const;

        void sortKeys();

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
