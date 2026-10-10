// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_COLORVALUE_H
#define COMPAT_CSS_COLORVALUE_H

// CSS Color 4/5 values (phase-2 §3.9.6 step 4): every syntax a browser reads, parsed into
// its own color space and converted to the 8-bit sRGB `#rrggbbaa` RCSS takes. RmlUi itself
// misreads rgba() alpha (0-255 scale) and rejects the space syntax, so every color the
// compat layer writes goes through here.

#include "Export.h"
#include "htmlui/compat/css/CssAst.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    enum class ColorSpace : uint8_t {
        Srgb, SrgbLinear, DisplayP3, A98Rgb, ProphotoRgb, Rec2020, XyzD50, XyzD65,
        Lab, Lch, Oklab, Oklch, Hsl, Hwb,
    };

    // A parsed color in its own space. sRGB-family channels are 0..1 (hsl/hwb: hue in degrees,
    // the others 0..100); lab/lch L 0..100; oklab/oklch L 0..1. Channels are kept as written
    // (no clamping beyond what the CSS grammar does); conversion to sRGB clips.
    struct CssColor {
        ColorSpace space = ColorSpace::Srgb;
        double c[3] = {0, 0, 0};
        double alpha = 1.0;
        bool none[4] = {false, false, false, false};
        // hex, keywords, rgb(), hsl(), hwb(): CSS serializes these as rgb()/rgba()
        bool legacy = false;
    };

    // What a parse met, for the diagnostics of the caller.
    struct ColorParseInfo {
        bool currentColor = false;   // the value is currentColor itself
        bool dynamic = false;        // var() without a static value, or currentColor, inside a color function
        bool system = false;         // a system color keyword
        bool lightDark = false;      // light-dark(): the light color was taken
        bool colorMix = false;
        bool relative = false;       // relative color syntax (from ...)
        std::string error;           // why the value is not a color ("" = not a color at all)
    };

    struct ColorContext {
        // the static replacement of a var() function (false: no static value)
        std::function<bool(const ComponentValue& var, ValueList& replacement)> resolveVar;
    };

    class JASPERENGINE_API ColorValue {
    public:
        // A <color> as one component value (ident, hash or function). currentColor parses
        // with info.currentColor set and returns nullopt (it has no static value).
        static std::optional<CssColor> parse(const ComponentValue& value, ColorParseInfo& info,
                                             const ColorContext& context = ColorContext());
        // the same from CSS text (tests, the UA)
        static std::optional<CssColor> parse(std::string_view css, ColorParseInfo* info = nullptr);

        // Could this component value be meant as a color (a hash, a color keyword, a color
        // function, currentColor)? Used to classify values without parsing them fully.
        static bool isColorSyntax(const ComponentValue& value);
        static bool isColorFunction(std::string_view name);

        // sRGB 0..1 (unclamped) and alpha
        static void toSrgb(const CssColor& color, double out[4]);
        // 0xRRGGBBAA rounded to the nearest 8-bit value; clipped: a channel was outside sRGB
        static uint32_t toRgba8(const CssColor& color, bool* clipped = nullptr);
        static std::string hex(uint32_t rgba);   // "#rrggbbaa"
        // parse + hex in one step; "" when not a static color
        static std::string toHex(const ComponentValue& value, ColorParseInfo& info, const ColorContext& context = ColorContext(),
                                 bool* clipped = nullptr);

        // the color converted into another space (channels as CssColor describes them)
        static CssColor convert(const CssColor& color, ColorSpace space);
        static const char* spaceName(ColorSpace space);   // "srgb", "display-p3", "lab", ...
        static std::optional<ColorSpace> findSpace(std::string_view name);
    };

}

#endif //COMPAT_CSS_COLORVALUE_H
