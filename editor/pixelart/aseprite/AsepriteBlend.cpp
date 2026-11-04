// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from the Aseprite Document Library (src/doc/blend_funcs.cpp):
//   Aseprite Document Library
//   Copyright (c) 2019-2025 Igara Studio S.A.
//   Copyright (c) 2001-2017 David Capello
//   This file is released under the terms of the MIT license.
//   Some references about alpha compositing and blend modes:
//     http://dev.w3.org/fxtf/compositing-1/
//     http://www.adobe.com/devnet/pdf/pdf_reference.html
// The arithmetic is unchanged (integer rounding, the order of operations, the HSL doubles and
// their truncation), including upstream's quirk that the new grayscale Addition blender is
// the Exclusion one. NOTICE.md lists it.

#include "AsepriteBlend.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace jasperengine::editor::pixelart::aseprite::blend {

    namespace {

        constexpr uint32_t kRgbaAMask = 0xff000000u;
        constexpr uint32_t kRgbaRgbMask = 0x00ffffffu;
        constexpr uint32_t kGrayaAMask = 0xff00u;
        constexpr uint32_t kGrayaVMask = 0x00ffu;

        inline int blendMultiply(int b, int s) { return mulUn8(b, s); }
        inline int blendScreen(int b, int s) { return b + s - mulUn8(b, s); }
        inline int blendHardLight(int b, int s) {
            return s < 128 ? blendMultiply(b, s << 1) : blendScreen(b, (s << 1) - 255);
        }
        inline int blendOverlay(int b, int s) { return blendHardLight(s, b); }
        inline int blendDarken(int b, int s) { return std::min(b, s); }
        inline int blendLighten(int b, int s) { return std::max(b, s); }
        inline int blendDifference(int b, int s) { return std::abs(b - s); }
        inline int blendExclusion(int b, int s) {
            const int t = mulUn8(b, s);
            return b + s - 2 * t;
        }

        inline uint32_t blendDivide(uint32_t b, uint32_t s) {
            if (b == 0)
                return 0;
            else if (b >= s)
                return 255;
            else
                return static_cast<uint32_t>(divUn8(static_cast<int>(b), static_cast<int>(s)));   // b / s
        }

        inline uint32_t blendColorDodge(uint32_t b, uint32_t s) {
            if (b == 0) return 0;
            s = (255 - s);
            if (b >= s)
                return 255;
            else
                return static_cast<uint32_t>(divUn8(static_cast<int>(b), static_cast<int>(s)));   // b / (1-s)
        }

        inline uint32_t blendColorBurn(uint32_t b, uint32_t s) {
            if (b == 255) return 255;
            b = (255 - b);
            if (b >= s)
                return 0;
            else
                return 255 - static_cast<uint32_t>(divUn8(static_cast<int>(b), static_cast<int>(s)));   // 1 - ((1-b)/s)
        }

        inline uint32_t blendSoftLight(uint32_t ib, uint32_t is) {
            const double b = ib / 255.0;
            const double s = is / 255.0;
            double r, d;
            if (b <= 0.25)
                d = ((16 * b - 12) * b + 4) * b;
            else
                d = std::sqrt(b);
            if (s <= 0.5)
                r = b - (1.0 - 2.0 * s) * b * (1.0 - b);
            else
                r = b + (2.0 * s - 1.0) * (d - b);
            return static_cast<uint32_t>(r * 255 + 0.5);
        }

        uint32_t rgbaSrc(uint32_t, uint32_t src, int) { return src; }

        uint32_t rgbaNormalDstOver(uint32_t backdrop, uint32_t src, int opacity) {
            const int sa = mulUn8(static_cast<int>(getA(src)), opacity);
            src = (src & kRgbaRgbMask) | (static_cast<uint32_t>(sa) << 24);
            return rgbaNormal(src, backdrop);
        }

        // the channel-wise blenders: the blended RGB with the source alpha, composited normally
        template<int (*F)(int, int)>
        uint32_t rgbaChannels(uint32_t backdrop, uint32_t src, int opacity) {
            const int r = F(static_cast<int>(getR(backdrop)), static_cast<int>(getR(src)));
            const int g = F(static_cast<int>(getG(backdrop)), static_cast<int>(getG(src)));
            const int b = F(static_cast<int>(getB(backdrop)), static_cast<int>(getB(src)));
            src = rgba(static_cast<uint32_t>(r), static_cast<uint32_t>(g), static_cast<uint32_t>(b), 0) | (src & kRgbaAMask);
            return rgbaNormal(backdrop, src, opacity);
        }

        template<uint32_t (*F)(uint32_t, uint32_t)>
        uint32_t rgbaChannelsU(uint32_t backdrop, uint32_t src, int opacity) {
            const uint32_t r = F(getR(backdrop), getR(src));
            const uint32_t g = F(getG(backdrop), getG(src));
            const uint32_t b = F(getB(backdrop), getB(src));
            src = rgba(r, g, b, 0) | (src & kRgbaAMask);
            return rgbaNormal(backdrop, src, opacity);
        }

        uint32_t rgbaMultiply(uint32_t b, uint32_t s, int o) { return rgbaChannels<blendMultiply>(b, s, o); }
        uint32_t rgbaScreen(uint32_t b, uint32_t s, int o) { return rgbaChannels<blendScreen>(b, s, o); }
        uint32_t rgbaOverlay(uint32_t b, uint32_t s, int o) { return rgbaChannels<blendOverlay>(b, s, o); }
        uint32_t rgbaDarken(uint32_t b, uint32_t s, int o) { return rgbaChannels<blendDarken>(b, s, o); }
        uint32_t rgbaLighten(uint32_t b, uint32_t s, int o) { return rgbaChannels<blendLighten>(b, s, o); }
        uint32_t rgbaColorDodge(uint32_t b, uint32_t s, int o) { return rgbaChannelsU<blendColorDodge>(b, s, o); }
        uint32_t rgbaColorBurn(uint32_t b, uint32_t s, int o) { return rgbaChannelsU<blendColorBurn>(b, s, o); }
        uint32_t rgbaHardLight(uint32_t b, uint32_t s, int o) { return rgbaChannels<blendHardLight>(b, s, o); }
        uint32_t rgbaSoftLight(uint32_t b, uint32_t s, int o) { return rgbaChannelsU<blendSoftLight>(b, s, o); }
        uint32_t rgbaDifference(uint32_t b, uint32_t s, int o) { return rgbaChannels<blendDifference>(b, s, o); }
        uint32_t rgbaExclusion(uint32_t b, uint32_t s, int o) { return rgbaChannels<blendExclusion>(b, s, o); }

        // ---- HSL blenders

        double lum(double r, double g, double b) { return 0.3 * r + 0.59 * g + 0.11 * b; }

        double sat(double r, double g, double b) {
            return std::max(r, std::max(g, b)) - std::min(r, std::min(g, b));
        }

        void clipColor(double& r, double& g, double& b) {
            const double l = lum(r, g, b);
            const double n = std::min(r, std::min(g, b));
            const double x = std::max(r, std::max(g, b));
            if (n < 0) {
                r = l + (((r - l) * l) / (l - n));
                g = l + (((g - l) * l) / (l - n));
                b = l + (((b - l) * l) / (l - n));
            }
            if (x > 1) {
                r = l + (((r - l) * (1 - l)) / (x - l));
                g = l + (((g - l) * (1 - l)) / (x - l));
                b = l + (((b - l) * (1 - l)) / (x - l));
            }
        }

        void setLum(double& r, double& g, double& b, double l) {
            const double d = l - lum(r, g, b);
            r += d;
            g += d;
            b += d;
            clipColor(r, g, b);
        }

        void setSat(double& r, double& g, double& b, double s) {
            const double minv = std::min(std::min(r, g), b);
            const double maxv = std::max(std::max(r, g), b);
            const double range = maxv - minv;
            if (range > 0.0) {
                r = ((r - minv) * s) / range;
                g = ((g - minv) * s) / range;
                b = ((b - minv) * s) / range;
            } else {
                r = g = b = 0.0;
            }
        }

        // set_sat before Aseprite 1.3.13: MIN/MID/MAX of references, so two equal channels can
        // alias the same variable and the assignments below overwrite each other (kept as it was)
        void setSatLegacy(double& r, double& g, double& b, double s) {
            double* minInner = (g < b) ? &g : &b;
            double* min = (r < *minInner) ? &r : minInner;
            double* maxInner = (g > b) ? &g : &b;
            double* max = (r > *maxInner) ? &r : maxInner;
            double* mid = r > g ? (g > b ? &g : (r > b ? &b : &r)) : (g > b ? (b > r ? &b : &r) : &g);
            if (*max > *min) {
                *mid = ((*mid - *min) * s) / (*max - *min);
                *max = s;
            } else {
                *mid = *max = 0;
            }
            *min = 0;
        }

        uint32_t hslResult(double r, double g, double b, uint32_t src) {
            return rgba(static_cast<uint32_t>(static_cast<int>(255.0 * r)), static_cast<uint32_t>(static_cast<int>(255.0 * g)),
                        static_cast<uint32_t>(static_cast<int>(255.0 * b)), 0) |
                   (src & kRgbaAMask);
        }

        uint32_t rgbaHslHue(uint32_t backdrop, uint32_t src, int opacity) {
            double r = getR(backdrop) / 255.0;
            double g = getG(backdrop) / 255.0;
            double b = getB(backdrop) / 255.0;
            const double s = sat(r, g, b);
            const double l = lum(r, g, b);
            r = getR(src) / 255.0;
            g = getG(src) / 255.0;
            b = getB(src) / 255.0;
            setSat(r, g, b, s);
            setLum(r, g, b, l);
            return rgbaNormal(backdrop, hslResult(r, g, b, src), opacity);
        }

        uint32_t rgbaHslHueLegacy(uint32_t backdrop, uint32_t src, int opacity) {
            double r = getR(backdrop) / 255.0;
            double g = getG(backdrop) / 255.0;
            double b = getB(backdrop) / 255.0;
            const double s = sat(r, g, b);
            const double l = lum(r, g, b);
            r = getR(src) / 255.0;
            g = getG(src) / 255.0;
            b = getB(src) / 255.0;
            setSatLegacy(r, g, b, s);
            setLum(r, g, b, l);
            return rgbaNormal(backdrop, hslResult(r, g, b, src), opacity);
        }

        uint32_t rgbaHslSaturationLegacy(uint32_t backdrop, uint32_t src, int opacity) {
            double r = getR(src) / 255.0;
            double g = getG(src) / 255.0;
            double b = getB(src) / 255.0;
            const double s = sat(r, g, b);
            r = getR(backdrop) / 255.0;
            g = getG(backdrop) / 255.0;
            b = getB(backdrop) / 255.0;
            const double l = lum(r, g, b);
            setSatLegacy(r, g, b, s);
            setLum(r, g, b, l);
            return rgbaNormal(backdrop, hslResult(r, g, b, src), opacity);
        }

        uint32_t rgbaHslSaturation(uint32_t backdrop, uint32_t src, int opacity) {
            double r = getR(src) / 255.0;
            double g = getG(src) / 255.0;
            double b = getB(src) / 255.0;
            const double s = sat(r, g, b);
            r = getR(backdrop) / 255.0;
            g = getG(backdrop) / 255.0;
            b = getB(backdrop) / 255.0;
            const double l = lum(r, g, b);
            setSat(r, g, b, s);
            setLum(r, g, b, l);
            return rgbaNormal(backdrop, hslResult(r, g, b, src), opacity);
        }

        uint32_t rgbaHslColor(uint32_t backdrop, uint32_t src, int opacity) {
            double r = getR(backdrop) / 255.0;
            double g = getG(backdrop) / 255.0;
            double b = getB(backdrop) / 255.0;
            const double l = lum(r, g, b);
            r = getR(src) / 255.0;
            g = getG(src) / 255.0;
            b = getB(src) / 255.0;
            setLum(r, g, b, l);
            return rgbaNormal(backdrop, hslResult(r, g, b, src), opacity);
        }

        uint32_t rgbaHslLuminosity(uint32_t backdrop, uint32_t src, int opacity) {
            double r = getR(src) / 255.0;
            double g = getG(src) / 255.0;
            double b = getB(src) / 255.0;
            const double l = lum(r, g, b);
            r = getR(backdrop) / 255.0;
            g = getG(backdrop) / 255.0;
            b = getB(backdrop) / 255.0;
            setLum(r, g, b, l);
            return rgbaNormal(backdrop, hslResult(r, g, b, src), opacity);
        }

        uint32_t rgbaAddition(uint32_t backdrop, uint32_t src, int opacity) {
            const int r = static_cast<int>(getR(backdrop) + getR(src));
            const int g = static_cast<int>(getG(backdrop) + getG(src));
            const int b = static_cast<int>(getB(backdrop) + getB(src));
            src = rgba(static_cast<uint32_t>(std::min(r, 255)), static_cast<uint32_t>(std::min(g, 255)),
                       static_cast<uint32_t>(std::min(b, 255)), 0) |
                  (src & kRgbaAMask);
            return rgbaNormal(backdrop, src, opacity);
        }

        uint32_t rgbaSubtract(uint32_t backdrop, uint32_t src, int opacity) {
            const int r = static_cast<int>(getR(backdrop)) - static_cast<int>(getR(src));
            const int g = static_cast<int>(getG(backdrop)) - static_cast<int>(getG(src));
            const int b = static_cast<int>(getB(backdrop)) - static_cast<int>(getB(src));
            src = rgba(static_cast<uint32_t>(std::max(r, 0)), static_cast<uint32_t>(std::max(g, 0)),
                       static_cast<uint32_t>(std::max(b, 0)), 0) |
                  (src & kRgbaAMask);
            return rgbaNormal(backdrop, src, opacity);
        }

        uint32_t rgbaDivide(uint32_t b, uint32_t s, int o) { return rgbaChannelsU<blendDivide>(b, s, o); }

        // Aseprite's RGBA_BLENDER_N: the normal result merged towards the blended one by the
        // backdrop's alpha, then by the composite alpha
        template<Func F>
        uint32_t rgbaNew(uint32_t backdrop, uint32_t src, int opacity) {
            if (backdrop & kRgbaAMask) {
                const uint32_t normal = rgbaNormal(backdrop, src, opacity);
                const uint32_t blend = F(backdrop, src, opacity);
                const int ba = static_cast<int>(getA(backdrop));
                const uint32_t normalToBlendMerge = rgbaMerge(normal, blend, ba);
                const int srcTotalAlpha = mulUn8(static_cast<int>(getA(src)), opacity);
                const int compositeAlpha = mulUn8(ba, srcTotalAlpha);
                return rgbaMerge(normalToBlendMerge, blend, compositeAlpha);
            }
            return rgbaNormal(backdrop, src, opacity);
        }

        // ---- grayscale

        uint32_t grayaSrc(uint32_t, uint32_t src, int) { return src; }

        uint32_t grayaMerge(uint32_t backdrop, uint32_t src, int opacity) {
            const int bk = static_cast<int>(grayV(backdrop));
            const int ba = static_cast<int>(grayA(backdrop));
            const int sk = static_cast<int>(grayV(src));
            const int sa = static_cast<int>(grayA(src));
            int rk;
            if (ba == 0)
                rk = sk;
            else if (sa == 0)
                rk = bk;
            else
                rk = bk + mulUn8((sk - bk), opacity);
            const int ra = ba + mulUn8((sa - ba), opacity);
            if (ra == 0) rk = 0;
            return graya(static_cast<uint32_t>(rk), static_cast<uint32_t>(ra));
        }

        uint32_t grayaNormalDstOver(uint32_t backdrop, uint32_t src, int opacity) {
            const int sa = mulUn8(static_cast<int>(grayA(src)), opacity);
            src = (src & kGrayaVMask) | (static_cast<uint32_t>(sa) << 8);
            return grayaNormal(src, backdrop);
        }

        template<int (*F)(int, int)>
        uint32_t grayaChannel(uint32_t backdrop, uint32_t src, int opacity) {
            const int v = F(static_cast<int>(grayV(backdrop)), static_cast<int>(grayV(src)));
            src = graya(static_cast<uint32_t>(v), 0) | (src & kGrayaAMask);
            return grayaNormal(backdrop, src, opacity);
        }

        template<uint32_t (*F)(uint32_t, uint32_t)>
        uint32_t grayaChannelU(uint32_t backdrop, uint32_t src, int opacity) {
            const uint32_t v = F(grayV(backdrop), grayV(src));
            src = graya(v, 0) | (src & kGrayaAMask);
            return grayaNormal(backdrop, src, opacity);
        }

        uint32_t grayaMultiply(uint32_t b, uint32_t s, int o) { return grayaChannel<blendMultiply>(b, s, o); }
        uint32_t grayaScreen(uint32_t b, uint32_t s, int o) { return grayaChannel<blendScreen>(b, s, o); }
        uint32_t grayaOverlay(uint32_t b, uint32_t s, int o) { return grayaChannel<blendOverlay>(b, s, o); }
        uint32_t grayaDarken(uint32_t b, uint32_t s, int o) { return grayaChannel<blendDarken>(b, s, o); }
        uint32_t grayaLighten(uint32_t b, uint32_t s, int o) { return grayaChannel<blendLighten>(b, s, o); }
        uint32_t grayaColorDodge(uint32_t b, uint32_t s, int o) { return grayaChannelU<blendColorDodge>(b, s, o); }
        uint32_t grayaColorBurn(uint32_t b, uint32_t s, int o) { return grayaChannelU<blendColorBurn>(b, s, o); }
        uint32_t grayaHardLight(uint32_t b, uint32_t s, int o) { return grayaChannel<blendHardLight>(b, s, o); }
        uint32_t grayaSoftLight(uint32_t b, uint32_t s, int o) { return grayaChannelU<blendSoftLight>(b, s, o); }
        uint32_t grayaDifference(uint32_t b, uint32_t s, int o) { return grayaChannel<blendDifference>(b, s, o); }
        uint32_t grayaExclusion(uint32_t b, uint32_t s, int o) { return grayaChannel<blendExclusion>(b, s, o); }

        uint32_t grayaAddition(uint32_t backdrop, uint32_t src, int opacity) {
            const int v = static_cast<int>(grayV(backdrop) + grayV(src));
            src = graya(static_cast<uint32_t>(std::min(v, 255)), 0) | (src & kGrayaAMask);
            return grayaNormal(backdrop, src, opacity);
        }

        uint32_t grayaSubtract(uint32_t backdrop, uint32_t src, int opacity) {
            const int v = static_cast<int>(grayV(backdrop)) - static_cast<int>(grayV(src));
            src = graya(static_cast<uint32_t>(std::max(v, 0)), 0) | (src & kGrayaAMask);
            return grayaNormal(backdrop, src, opacity);
        }

        uint32_t grayaDivide(uint32_t b, uint32_t s, int o) { return grayaChannelU<blendDivide>(b, s, o); }

        template<Func F>
        uint32_t grayaNew(uint32_t backdrop, uint32_t src, int opacity) {
            if (backdrop & kGrayaAMask) {
                const uint32_t normal = grayaNormal(backdrop, src, opacity);
                const uint32_t blend = F(backdrop, src, opacity);
                const int ba = static_cast<int>(grayA(backdrop));
                const uint32_t normalToBlendMerge = grayaMerge(normal, blend, ba);
                const int srcTotalAlpha = mulUn8(static_cast<int>(grayA(src)), opacity);
                const int compositeAlpha = mulUn8(ba, srcTotalAlpha);
                return grayaMerge(normalToBlendMerge, blend, compositeAlpha);
            }
            return grayaNormal(backdrop, src, opacity);
        }

    }

    uint32_t rgbaMerge(uint32_t backdrop, uint32_t src, int opacity) {
        const int br = static_cast<int>(getR(backdrop));
        const int bg = static_cast<int>(getG(backdrop));
        const int bb = static_cast<int>(getB(backdrop));
        const int ba = static_cast<int>(getA(backdrop));
        const int sr = static_cast<int>(getR(src));
        const int sg = static_cast<int>(getG(src));
        const int sb = static_cast<int>(getB(src));
        const int sa = static_cast<int>(getA(src));
        int rr, rg, rb;
        if (ba == 0) {
            rr = sr;
            rg = sg;
            rb = sb;
        } else if (sa == 0) {
            rr = br;
            rg = bg;
            rb = bb;
        } else {
            rr = br + mulUn8((sr - br), opacity);
            rg = bg + mulUn8((sg - bg), opacity);
            rb = bb + mulUn8((sb - bb), opacity);
        }
        const int ra = ba + mulUn8((sa - ba), opacity);
        if (ra == 0) rr = rg = rb = 0;
        return rgba(static_cast<uint32_t>(rr), static_cast<uint32_t>(rg), static_cast<uint32_t>(rb), static_cast<uint32_t>(ra));
    }

    uint32_t rgbaNormal(uint32_t backdrop, uint32_t src, int opacity) {
        if (!(backdrop & kRgbaAMask)) {
            int a = static_cast<int>(getA(src));
            a = mulUn8(a, opacity);
            return (src & kRgbaRgbMask) | (static_cast<uint32_t>(a) << 24);
        } else if (!(src & kRgbaAMask)) {
            return backdrop;
        }
        const int br = static_cast<int>(getR(backdrop));
        const int bg = static_cast<int>(getG(backdrop));
        const int bb = static_cast<int>(getB(backdrop));
        const int ba = static_cast<int>(getA(backdrop));
        const int sr = static_cast<int>(getR(src));
        const int sg = static_cast<int>(getG(src));
        const int sb = static_cast<int>(getB(src));
        int sa = static_cast<int>(getA(src));
        sa = mulUn8(sa, opacity);
        // Ra = Sa + Ba*(1-Sa); Rc = Bc + (Sc-Bc)*Sa/Ra
        const int ra = sa + ba - mulUn8(ba, sa);
        const int rr = br + (sr - br) * sa / ra;
        const int rg = bg + (sg - bg) * sa / ra;
        const int rb = bb + (sb - bb) * sa / ra;
        return rgba(static_cast<uint32_t>(rr), static_cast<uint32_t>(rg), static_cast<uint32_t>(rb), static_cast<uint32_t>(ra));
    }

    uint32_t grayaNormal(uint32_t backdrop, uint32_t src, int opacity) {
        if (!(backdrop & kGrayaAMask)) {
            int a = static_cast<int>(grayA(src));
            a = mulUn8(a, opacity);
            return (src & 0xff) | (static_cast<uint32_t>(a) << 8);
        } else if (!(src & kGrayaAMask)) {
            return backdrop;
        }
        const int bg = static_cast<int>(grayV(backdrop));
        const int ba = static_cast<int>(grayA(backdrop));
        const int sg = static_cast<int>(grayV(src));
        int sa = static_cast<int>(grayA(src));
        sa = mulUn8(sa, opacity);
        const int ra = ba + sa - mulUn8(ba, sa);
        const int rg = bg + (sg - bg) * sa / ra;
        return graya(static_cast<uint32_t>(rg), static_cast<uint32_t>(ra));
    }

    Func rgbaBlender(Mode mode, bool newBlend, bool legacyHsl) {
        if (legacyHsl && mode == Mode::Hue) return newBlend ? rgbaNew<rgbaHslHueLegacy> : rgbaHslHueLegacy;
        if (legacyHsl && mode == Mode::Saturation) return newBlend ? rgbaNew<rgbaHslSaturationLegacy> : rgbaHslSaturationLegacy;
        switch (mode) {
            case Mode::Src: return rgbaSrc;
            case Mode::Merge: return rgbaMerge;
            case Mode::DstOver: return rgbaNormalDstOver;
            case Mode::Normal: return [](uint32_t b, uint32_t s, int o) { return rgbaNormal(b, s, o); };
            case Mode::Multiply: return newBlend ? rgbaNew<rgbaMultiply> : rgbaMultiply;
            case Mode::Screen: return newBlend ? rgbaNew<rgbaScreen> : rgbaScreen;
            case Mode::Overlay: return newBlend ? rgbaNew<rgbaOverlay> : rgbaOverlay;
            case Mode::Darken: return newBlend ? rgbaNew<rgbaDarken> : rgbaDarken;
            case Mode::Lighten: return newBlend ? rgbaNew<rgbaLighten> : rgbaLighten;
            case Mode::ColorDodge: return newBlend ? rgbaNew<rgbaColorDodge> : rgbaColorDodge;
            case Mode::ColorBurn: return newBlend ? rgbaNew<rgbaColorBurn> : rgbaColorBurn;
            case Mode::HardLight: return newBlend ? rgbaNew<rgbaHardLight> : rgbaHardLight;
            case Mode::SoftLight: return newBlend ? rgbaNew<rgbaSoftLight> : rgbaSoftLight;
            case Mode::Difference: return newBlend ? rgbaNew<rgbaDifference> : rgbaDifference;
            case Mode::Exclusion: return newBlend ? rgbaNew<rgbaExclusion> : rgbaExclusion;
            case Mode::Hue: return newBlend ? rgbaNew<rgbaHslHue> : rgbaHslHue;
            case Mode::Saturation: return newBlend ? rgbaNew<rgbaHslSaturation> : rgbaHslSaturation;
            case Mode::Color: return newBlend ? rgbaNew<rgbaHslColor> : rgbaHslColor;
            case Mode::Luminosity: return newBlend ? rgbaNew<rgbaHslLuminosity> : rgbaHslLuminosity;
            case Mode::Addition: return newBlend ? rgbaNew<rgbaAddition> : rgbaAddition;
            case Mode::Subtract: return newBlend ? rgbaNew<rgbaSubtract> : rgbaSubtract;
            case Mode::Divide: return newBlend ? rgbaNew<rgbaDivide> : rgbaDivide;
        }
        return rgbaSrc;
    }

    Func grayaBlender(Mode mode, bool newBlend) {
        constexpr Func normal = [](uint32_t b, uint32_t s, int o) { return grayaNormal(b, s, o); };
        switch (mode) {
            case Mode::Src: return grayaSrc;
            case Mode::Merge: return grayaMerge;
            case Mode::DstOver: return grayaNormalDstOver;
            case Mode::Normal: return normal;
            case Mode::Multiply: return newBlend ? grayaNew<grayaMultiply> : grayaMultiply;
            case Mode::Screen: return newBlend ? grayaNew<grayaScreen> : grayaScreen;
            case Mode::Overlay: return newBlend ? grayaNew<grayaOverlay> : grayaOverlay;
            case Mode::Darken: return newBlend ? grayaNew<grayaDarken> : grayaDarken;
            case Mode::Lighten: return newBlend ? grayaNew<grayaLighten> : grayaLighten;
            case Mode::ColorDodge: return newBlend ? grayaNew<grayaColorDodge> : grayaColorDodge;
            case Mode::ColorBurn: return newBlend ? grayaNew<grayaColorBurn> : grayaColorBurn;
            case Mode::HardLight: return newBlend ? grayaNew<grayaHardLight> : grayaHardLight;
            case Mode::SoftLight: return newBlend ? grayaNew<grayaSoftLight> : grayaSoftLight;
            case Mode::Difference: return newBlend ? grayaNew<grayaDifference> : grayaDifference;
            case Mode::Exclusion: return newBlend ? grayaNew<grayaExclusion> : grayaExclusion;
            // the HSL modes have no grayscale meaning: normal, as upstream
            case Mode::Hue:
            case Mode::Saturation:
            case Mode::Color:
            case Mode::Luminosity: return normal;
            // upstream returns the new Exclusion blender for the new Addition one: kept, so the
            // composition stays Aseprite's
            case Mode::Addition: return newBlend ? grayaNew<grayaExclusion> : grayaAddition;
            case Mode::Subtract: return newBlend ? grayaNew<grayaSubtract> : grayaSubtract;
            case Mode::Divide: return newBlend ? grayaNew<grayaDivide> : grayaDivide;
        }
        return grayaSrc;
    }

}
