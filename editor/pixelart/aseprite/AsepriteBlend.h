// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from the Aseprite Document Library (src/doc/blend_funcs.h, blend_mode.h):
//   Aseprite Document Library
//   Copyright (c) 2019-2025 Igara Studio S.A.
//   Copyright (c) 2001-2017 David Capello
//   This file is released under the terms of the MIT license.
// NOTICE.md lists it. The blenders are Aseprite's bit for bit (MUL_UN8 / DIV_UN8 from pixman's
// pixman-combine32.h), so a composition equals Aseprite's own export.

#pragma once

#include <cstdint>

namespace jasperengine::editor::pixelart::aseprite::blend {

    // Aseprite's packed colours: rgba = r | g << 8 | b << 16 | a << 24, graya = v | a << 8
    inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
        return (r & 0xff) | ((g & 0xff) << 8) | ((b & 0xff) << 16) | ((a & 0xff) << 24);
    }
    inline uint32_t getR(uint32_t c) { return c & 0xff; }
    inline uint32_t getG(uint32_t c) { return (c >> 8) & 0xff; }
    inline uint32_t getB(uint32_t c) { return (c >> 16) & 0xff; }
    inline uint32_t getA(uint32_t c) { return (c >> 24) & 0xff; }
    inline uint32_t graya(uint32_t v, uint32_t a) { return (v & 0xff) | ((a & 0xff) << 8); }
    inline uint32_t grayV(uint32_t c) { return c & 0xff; }
    inline uint32_t grayA(uint32_t c) { return (c >> 8) & 0xff; }

    inline uint32_t pack(const uint8_t* p) { return rgba(p[0], p[1], p[2], p[3]); }
    inline void unpack(uint32_t c, uint8_t* p) {
        p[0] = static_cast<uint8_t>(getR(c));
        p[1] = static_cast<uint8_t>(getG(c));
        p[2] = static_cast<uint8_t>(getB(c));
        p[3] = static_cast<uint8_t>(getA(c));
    }

    // The file's blend modes 0..18 (layer chunk), plus the internal ones the renderer uses
    enum class Mode : int {
        Src = -2,
        Merge = -3,
        DstOver = -7,
        Normal = 0, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, ColorBurn, HardLight,
        SoftLight, Difference, Exclusion, Hue, Saturation, Color, Luminosity, Addition, Subtract, Divide
    };

    using Func = uint32_t (*)(uint32_t backdrop, uint32_t src, int opacity);

    // newBlend: Aseprite 1.3's default "new blending" (the *_n blenders); false: the classic ones.
    // Hue and Saturation follow Aseprite 1.3.15.3 and later (set_sat scales every channel by the
    // range, upstream 0995e72a6f). legacyHsl: as Aseprite 1.3.12 and earlier drew them (set_sat
    // took MIN/MID/MAX of references, which alias when two channels are equal), to match exports
    // made with those versions; 1.3.13-1.3.15.2 (ed56011a0f) differ from both only on ties.
    Func rgbaBlender(Mode mode, bool newBlend, bool legacyHsl = false);
    Func grayaBlender(Mode mode, bool newBlend);

    uint32_t rgbaNormal(uint32_t backdrop, uint32_t src, int opacity = 255);
    uint32_t rgbaMerge(uint32_t backdrop, uint32_t src, int opacity);
    uint32_t grayaNormal(uint32_t backdrop, uint32_t src, int opacity = 255);

    // pixman's 8-bit rounding multiply and divide
    inline int mulUn8(int a, int b) {
        const int t = a * b + 0x80;
        return ((t >> 8) + t) >> 8;
    }
    inline int divUn8(int a, int b) { return (a * 0xff + (b / 2)) / b; }

}
