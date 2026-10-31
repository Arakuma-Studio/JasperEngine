// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef GRADIENT_H
#define GRADIENT_H

// Unity's Gradient: colour keys and alpha keys over [0, 1], blended or stepped, shared by
// the line and trail renderers (colour over the length) and the particle modules. Colours
// are linear RGB in memory (the engine's convention; the Inspector, Lua and MCP show and
// take sRGB). No dependency on components; pure, gtested (editor/tests/GradientTest.cpp).

#include "Export.h"
#include "Vector3.h"
#include "Vector4.h"
#include <cstdint>
#include <vector>

namespace jasperengine {

    struct JASPERENGINE_API GradientColorKey {
        float time = 0;
        Vector3 color = Vector3(1, 1, 1);   // linear RGB

        GradientColorKey() = default;
        GradientColorKey(float time, const Vector3& color);

        bool operator==(const GradientColorKey& other) const;
        bool operator!=(const GradientColorKey& other) const { return !(*this == other); }
    };

    struct JASPERENGINE_API GradientAlphaKey {
        float time = 0;
        float alpha = 1;

        GradientAlphaKey() = default;
        GradientAlphaKey(float time, float alpha);

        bool operator==(const GradientAlphaKey& other) const;
        bool operator!=(const GradientAlphaKey& other) const { return !(*this == other); }
    };

    // Blend: linear between keys; Fixed: the first key at or after t (Unity's step mode)
    enum class GradientMode : uint8_t { Blend = 0, Fixed = 1 };

    struct JASPERENGINE_API Gradient {
        // Unity's limit per list
        static constexpr size_t MAX_KEYS = 8;

        // 0..8 keys each, times in [0, 1]. A new gradient is Unity's: white, opaque (a key at
        // each end of both lists)
        std::vector<GradientColorKey> colorKeys;
        std::vector<GradientAlphaKey> alphaKeys;
        GradientMode mode = GradientMode::Blend;

        Gradient();
        Gradient(std::vector<GradientColorKey> colorKeys, std::vector<GradientAlphaKey> alphaKeys, GradientMode mode = GradientMode::Blend);

        // linear RGBA at t (clamped to [0, 1]); no colour keys: white, no alpha keys: 1
        Vector4 evaluate(float t) const;
        Vector3 evaluateColor(float t) const;
        float evaluateAlpha(float t) const;

        // one colour (linear RGBA) everywhere
        static Gradient constant(const Vector4& color);
        // from one colour to another (linear RGBA), both ends keyed
        static Gradient twoColors(const Vector4& start, const Vector4& end);

        // non-finite times and values dropped, times clamped to [0, 1], stable sort by time,
        // at most MAX_KEYS keys per list (the first ones by time)
        void normalize();

        bool operator==(const Gradient& other) const;
        bool operator!=(const Gradient& other) const { return !(*this == other); }
    };

}

#endif //GRADIENT_H
