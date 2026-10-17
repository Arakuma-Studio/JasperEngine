// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_EFFECTS_EFFECTVALUE_H
#define HTMLUI_EFFECTS_EFFECTVALUE_H

#include "EffectTypes.h"
#include "math/Vector2.h"
#include "math/Vector4.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::htmlui {

    // a length as written; CSS px once resolved against the element (EffectValueParser::resolveLength)
    struct EffectLength {
        enum class Unit : uint8_t { Px, Em, Rem, Vw, Vh, Vmin, Vmax, Percent };
        float number = 0.f;
        Unit unit = Unit::Px;   // Percent: of the effect area's shorter side
    };

    // one axis of a position: a fraction of the area (0 left/top) plus a CSS px offset
    struct EffectPositionAxis {
        float fraction = 0.f;
        float px = 0.f;
    };

    // A parameter value, normalised (phase-5 spec 3.3): numbers, integers, percentages (0-1),
    // angles (radians), times (seconds), enum indices and booleans in `f`; lengths as written in
    // `length` (resolved to px when the element is known); colours as straight linear sRGB;
    // positions as two axes; palettes as colours and positions 0-1.
    struct EffectValue {
        EffectParamType type = EffectParamType::Number;
        float f = 0.f;
        EffectLength length;
        Vector4 color = Vector4(0.f, 0.f, 0.f, 0.f);
        bool currentColor = false;            // currentColor: the element's computed color at instancing
        EffectPositionAxis x, y;              // position
        std::vector<std::pair<Vector4, float>> palette;
        bool isVar = false;                   // a value with var(): validated at runtime
        std::string text;                     // the value as given (var() values, messages)
    };

    // what a length resolves against
    struct EffectLengthContext {
        float fontSize = 16.f;
        float rootFontSize = 16.f;
        Vector2 viewport = Vector2(0.f, 0.f);
        float areaShorterSide = 0.f;
    };

    class JASPERENGINE_API EffectValueParser {
    public:
        // a CSS value for the parameter's type; false with error set (the effect-bad-value
        // message's reason) when it does not parse. Ranges are not applied (clampToRange).
        static bool parse(const EffectParamDef& def, std::string_view css, EffectValue& out, std::string& error);
        // the same for a type, enum options given (the RmlUi property parsers)
        static bool parseType(EffectParamType type, const std::vector<std::string>& options, std::string_view css, EffectValue& out,
                              std::string& error);
        // canonical CSS text: numbers shortest round-trip, lengths with their unit, angles deg,
        // times s, colours #rrggbbaa, positions "<x> <y>", palettes "#rrggbbaa <p>% ..."
        static std::string canonical(const EffectParamDef& def, const EffectValue& v);
        // v clamped into the parameter's range (normalised units); clamped tells whether it was
        static float clampToRange(const EffectParamDef& def, float v, bool& clamped);
        // clamps a value in place (numbers, integers, percentages, angles, times, px lengths)
        static bool clamp(const EffectParamDef& def, EffectValue& value);
        // a length in CSS px
        static float resolveLength(const EffectLength& length, const EffectLengthContext& context);
        // a position axis in CSS px of an area extent
        static float resolveAxis(const EffectPositionAxis& axis, float extent) { return axis.fraction * extent + axis.px; }
        // CSS sRGB 0..1 <-> linear sRGB
        static float srgbToLinear(float c);
        static float linearToSrgb(float c);
        // shortest text that reads back as the same float
        static std::string formatNumber(float value);
        // "#rrggbbaa" of a straight linear colour
        static std::string colorHex(const Vector4& linear);
        // the type's expectation for messages ("a number in [0, 1]", "a length (px, em, ...)")
        static std::string expected(const EffectParamDef& def);
        // does the text use var()?
        static bool hasVar(std::string_view css);
    };

}

#endif //HTMLUI_EFFECTS_EFFECTVALUE_H
