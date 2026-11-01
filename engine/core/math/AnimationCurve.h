// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMATIONCURVE_H
#define ANIMATIONCURVE_H

// Unity's AnimationCurve (non-weighted keys): a float curve of cubic Hermite segments,
// shared by the line and trail renderers (width over the length) and the particle modules.
// No dependency on components; pure, gtested (editor/tests/AnimationCurveTest.cpp).

#include "Export.h"
#include <cstdint>
#include <limits>
#include <vector>

// SYNC4-ABSORB(curve): upstream's AnimationCurve / Gradient, in a namespace of their own so they
// do not collide with S27's (math/Curve.h). Their users (Line / Trail Renderer, LineRibbonBuilder,
// their codecs) move onto S27's types in the Curve absorption, which then deletes this file.
namespace jasperengine::upcurve {

    struct JASPERENGINE_API CurveKey {
        float time = 0, value = 0;
        // slopes, value units per time unit; +-inf = step (the segment holds the key before it)
        float inTangent = 0, outTangent = 0;

        CurveKey() = default;
        CurveKey(float time, float value, float inTangent = 0, float outTangent = 0);

        bool operator==(const CurveKey& other) const;
        bool operator!=(const CurveKey& other) const { return !(*this == other); }
    };

    // what a curve does outside its keys (Unity WrapMode for curves: Once/ClampForever = Clamp)
    enum class CurveWrap : uint8_t { Clamp = 0, Loop = 1, PingPong = 2 };

    struct JASPERENGINE_API AnimationCurve {
        // a step tangent (Unity's "Constant" key): the segment holds the value before it
        static constexpr float STEP = std::numeric_limits<float>::infinity();

        // sorted by time (normalize() sorts and drops equal times)
        std::vector<CurveKey> keys;
        CurveWrap preWrap = CurveWrap::Clamp, postWrap = CurveWrap::Clamp;

        AnimationCurve() = default;
        AnimationCurve(std::vector<CurveKey> keys, CurveWrap preWrap = CurveWrap::Clamp, CurveWrap postWrap = CurveWrap::Clamp);

        // empty: 0; one key: its value; else the Hermite segment around t (dt = k1.time -
        // k0.time, m0 = k0.outTangent * dt, m1 = k1.inTangent * dt; an infinite tangent on
        // either side holds k0.value), outside the keys by the wrap modes
        float evaluate(float t) const;

        // one key (0, v)
        static AnimationCurve constant(float v);
        // two keys with the slope of the line between them as both tangents
        static AnimationCurve linear(float t0, float v0, float t1, float v1);
        // two keys with flat tangents (Unity AnimationCurve.EaseInOut)
        static AnimationCurve easeInOut(float t0, float v0, float t1, float v1);

        // stable sort by time; keys with non-finite time or value are dropped, then every key
        // at the time of the one before it (the first one stays)
        void normalize();

        // inserts a key at its time (sorted); -1 when a key is already at that time (Unity's
        // AddKey) or the values are not finite, else its index
        int addKey(const CurveKey& key);
        // moves key `index` to `key`'s time and value: the index it ends at (the list stays
        // sorted), -1 when out of range or another key is at that time
        int moveKey(size_t index, const CurveKey& key);
        bool removeKey(size_t index);

        // the smallest and largest value of the curve over [minTime, maxTime] (sampled with
        // the keys themselves included): the editor's preview range
        void valueRange(float minTime, float maxTime, float& minValue, float& maxValue) const;
        float firstTime() const;
        float lastTime() const;

        bool isConstant() const;

        bool operator==(const AnimationCurve& other) const;
        bool operator!=(const AnimationCurve& other) const { return !(*this == other); }
    };

}

#endif //ANIMATIONCURVE_H
