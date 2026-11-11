// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/color.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// The small numeric rules every cleanup module shares, written once so the port keeps
// JavaScript's semantics: Math.round rounds half up (not away from zero), colours are compared
// in S43's OKLab (docs/specs/S46-pixel-art-cleanup.md R8, R35) with distances summed in double,
// as ritmo's Math.hypot does.

#pragma once

#include "pixelart/PixelColor.h"
#include "pixelart/Rgba8.h"

#include <cmath>
#include <cstdint>

namespace jasperengine::editor::pixelart::cleanup {

    // JavaScript's Math.round: halves go up, so -2.5 is -2 (std::round would give -3)
    inline double jsRound(double value) { return std::floor(value + 0.5); }

    // color.ts's channel: a value rounded and clamped to a byte
    inline uint8_t channel(double value) {
        const double rounded = jsRound(value);
        return static_cast<uint8_t>(rounded < 0 ? 0 : rounded > 255 ? 255 : rounded);
    }

    inline jasperengine::pixelart::Rgba8 rgbOf(uint32_t packed) { return jasperengine::pixelart::unpackRgb(packed); }

    inline jasperengine::pixelart::Lab oklabOf(jasperengine::pixelart::Rgba8 color) {
        color.a = 255;
        return jasperengine::pixelart::toOklab(color);
    }

    // ritmo's `distance` (Math.hypot in OKLab)
    inline double labDistance(const jasperengine::pixelart::Lab& x, const jasperengine::pixelart::Lab& y) {
        return std::hypot(double(x.l) - double(y.l), double(x.a) - double(y.a), double(x.b) - double(y.b));
    }

    // ritmo's `distance2` (squared, palette.ts)
    inline double labDistance2(const jasperengine::pixelart::Lab& x, const jasperengine::pixelart::Lab& y) {
        const double dl = double(x.l) - double(y.l), da = double(x.a) - double(y.a), db = double(x.b) - double(y.b);
        return dl * dl + da * da + db * db;
    }

}
