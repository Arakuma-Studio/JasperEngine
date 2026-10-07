// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMVALUE_H
#define ANIMVALUE_H

// Value decomposition of the anime API, ported from Anime.js 4.5.0 (core/values.js,
// core/colors.js): numbers, numbers with a unit, relative values (`+=`, `-=`, `*=`), colours
// (`#rgb[a]`, `#rrggbb[aa]`, `rgb()/rgba()/hsl()/hsla()`, interpolated per channel in
// non-premultiplied 0-255 RGBA) and complex strings (the numbers inside a template string).

#include "Export.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine {

    // a property value as read from or given to a target: nothing, a number or a string
    struct JASPERENGINE_API AnimRawValue {
        enum class Kind { NIL, NUMBER, STRING };
        Kind kind = Kind::NIL;
        double number = 0.0;
        std::string text;

        AnimRawValue() = default;
        AnimRawValue(double v): kind(Kind::NUMBER), number(v) {}
        AnimRawValue(const std::string& v): kind(Kind::STRING), text(v) {}
        AnimRawValue(const char* v): kind(Kind::STRING), text(v ? v : "") {}

        bool isNil() const { return kind == Kind::NIL; }
        bool isNumber() const { return kind == Kind::NUMBER; }
        bool isString() const { return kind == Kind::STRING; }
        // JavaScript truthiness (nil, 0, NaN and "" are false)
        bool isTruthy() const;
        // JavaScript unary plus
        double toNumber() const;
        // JavaScript String(): numbers as Number#toString
        std::string toString() const;
        bool operator==(const AnimRawValue& other) const;
    };

    enum class AnimValueType { NUMBER = 0, UNIT = 1, COLOR = 2, COMPLEX = 3 };

    // Anime's TweenDecomposedValue {t, n, u, o, d, s}
    struct JASPERENGINE_API AnimDecomposed {
        AnimValueType t = AnimValueType::NUMBER;
        double n = 0.0;
        std::string u;                                // unit, empty for none
        char o = 0;                                   // relative operator '+' '-' '*' (or the char before '='), 0 for none
        std::optional<std::vector<double>> d;         // colour channels or complex numbers
        std::optional<std::vector<std::string>> s;    // complex string pieces around the numbers
    };

    using AnimColor = std::array<double, 4>;          // r, g, b 0-255, a 0-1

    class JASPERENGINE_API AnimValue {
    public:
        // decomposeRawValue
        static AnimDecomposed decompose(const AnimRawValue& raw);
        // getRelativeValue
        static double relative(double x, double y, char op);

        static bool isHex(const std::string& text);
        static bool isColor(const std::string& text);
        // convertColorStringValuesToRgbaArray (unparsable: 0, 0, 0, 1)
        static AnimColor parseColor(const std::string& text);
        // "rgba(r,g,b,a)" as the render writes it (channels rounded, alpha as is)
        static std::string colorToRgbaString(const AnimColor& color);
        // "#rrggbbaa" (plain table targets)
        static std::string colorToHexString(const AnimColor& color);

        // the number/unit split of "12.5px" (^number unit$), false when the text is not one
        static bool splitUnit(const std::string& text, double& number, std::string& unit);
        // the numbers of a complex string and the pieces around them
        static void splitComplex(const std::string& text, std::vector<double>& numbers, std::vector<std::string>& strings);

        // camelCase -> kebab-case (Anime's toLowerCase)
        static std::string toKebabCase(const std::string& name);
    };

}

#endif //ANIMVALUE_H
