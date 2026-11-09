// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Math in every numeric field of the editor (Unity's numeric field expressions): typed text is
// evaluated by tinyexpr (libs/tinyexpr) instead of being parsed as a plain number. ImGui calls
// in through the one hook of libs/imgui/JASPER_PATCHES.md, so no call site changes.
//
//   2+2, 2^3^2 (= 512, right-associative), log(e) (= 1, natural), sqrt, sin, ..., min max clamp
//   round lerp, pi tau e
//   units after a value on fields that have one: 25cm (Meters), 90deg / pi rad / 0.5turn
//   (Degrees), 250ms / 2min (Seconds), 12px (Pixels)
//   +=10 -=1 *=2 /=4 apply to the current value (+5 and -5 are absolute)
//   multi-edit: relative ops apply per object; L(a,b) ramps over the selection order,
//   R(a,b) gives each object a random value (seeded)

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    // The unit a field stores its value in (S08's field Unit maps onto this)
    enum class NumericUnit { None, Meters, Degrees, Seconds, Pixels };

    struct NumericContext {
        NumericUnit unit = NumericUnit::None;
        bool integer = false;          // round half away from zero
        int objectIndex = 0;           // multi-edit: this object's place in the selection
        int objectCount = 1;
        uint32_t seed = 0;             // R(a,b)
    };

    struct NumericResult {
        bool ok = false;
        double value = 0.0;
        std::string error;             // "Unknown name 'pii' at 3"
        int position = 0;              // 1-based character of the error, 0 when none
        bool perObject = false;        // a relative op or L()/R(): differs per object in a multi-edit
    };

    class NumericExpression {
    public:
        static NumericResult evaluate(std::string_view text, double current, const NumericContext& context = NumericContext());
        // An expression over named values (S85's Animation Presets: D, n, fps and the preset's
        // parameters), with the same functions; no units, no relative ops. An unknown name fails.
        static NumericResult evaluateWith(std::string_view text, const std::vector<std::pair<std::string, double>>& variables);

        // A plain number (what ImGui parses itself): the hook leaves those alone
        static bool isPlainNumber(std::string_view text);
        // "mm cm m km in ft": the suffixes a unit accepts, empty for None
        static const char* unitSuffixes(NumericUnit unit);
        static const char* unitName(NumericUnit unit);
        static bool parseUnit(const std::string& name, NumericUnit& out);

        // The editor installs the hook once ImGui exists
        static void installImGuiHook();
        // Once per frame, after the windows: the tooltip of an expression that did not evaluate
        // (3 s at the field it was typed in)
        static void drawErrorTooltip();

        // A multi-edit field's expression that differs per object (+=, L(), R()): the widget
        // applied the first object's value; the caller applies `text` to each object with its
        // own current value and index. False when the last edit was not one.
        static bool takeMultiEdit(std::string& text);
        // Per-object values of a multi-edit expression (index order), current[i] for each
        static bool evaluateForObjects(const std::string& text, const std::vector<double>& current, NumericContext context,
                                       std::vector<double>& out);
    };

    // The unit and multi-edit context of the numeric fields drawn while it lives (the
    // Inspector's property rows). Nested scopes: the innermost wins.
    class NumericFieldScope {
    public:
        explicit NumericFieldScope(NumericUnit unit, int objectCount = 1, bool integer = false);
        ~NumericFieldScope();
        NumericFieldScope(const NumericFieldScope&) = delete;
        NumericFieldScope& operator=(const NumericFieldScope&) = delete;

        static const NumericContext* current();
    };

}
