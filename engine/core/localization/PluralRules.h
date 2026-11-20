// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LOCALIZATION_PLURALRULES_H
#define LOCALIZATION_PLURALRULES_H

// Contract (docs/specs/game-ui-localization.md R4, D4)
// - CLDR cardinal plural rules and the latn number symbols of every CLDR language, compiled by
//   tools/l10n/gen_plural_rules.py into PluralRules.gen.inc (pinned release, LICENSE.cldr.txt).
// - A tag resolves by dropping subtags ("pt-PT" has its own rules, "de-AT" uses "de"); an unknown
//   language is CLDR's root: every number is `other`.
// - Operands come from the number as it is shown (its visible fraction digits), as CLDR defines
//   them: "1.0" is not "1" in languages whose rules read v.
// - Pure functions, safe from any thread; no allocation but the returned strings.

#include "Export.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace jasperengine::l10n {

    enum class PluralCategory : uint8_t { Zero, One, Two, Few, Many, Other };

    // CLDR's operands (TR35 "Plural Operand Meanings")
    struct PluralOperands {
        double n = 0;       // the absolute value
        int64_t i = 0;      // its integer digits
        int64_t v = 0;      // the count of visible fraction digits, with trailing zeros
        int64_t w = 0;      // the same without trailing zeros
        int64_t f = 0;      // the visible fraction digits, with trailing zeros
        int64_t t = 0;      // the same without trailing zeros
        int64_t e = 0;      // the exponent of compact notation ("1.2c6": 6)
    };

    struct NumberSymbols {
        const char* decimal;
        const char* group;
        const char* minus;
        const char* percent;
        const char* percentSpacer;      // between the number and the percent sign
        uint8_t percentPrefix;          // 1: "%12" (tr), 0: "12 %"
        uint8_t primaryGroup;           // digits in the first group (3; 0: no grouping)
        uint8_t secondaryGroup;         // digits in the next ones (2 in hi: 12,34,567)
        uint8_t minimumGrouping;        // the integer digits before grouping starts (es: 2, "1234")
    };

    enum class NumberStyle : uint8_t { Decimal, Integer, Percent };

    class JASPERENGINE_API PluralRules {
    public:
        // the operands of a number written as text ("1.50", "-3", "1.2c6"); false when it is not one
        static bool operands(std::string_view text, PluralOperands& out);
        static PluralCategory select(std::string_view language, const PluralOperands& operands);
        // the category of a number as `formatNumber` shows it
        static PluralCategory select(std::string_view language, double value, NumberStyle style = NumberStyle::Decimal);
        // the categories the language uses, in Zero..Other order (Insert Plural's skeleton)
        static std::string categories(std::string_view language);
        static const char* name(PluralCategory category);
        static bool fromName(std::string_view name, PluralCategory& out);

        static const NumberSymbols& symbols(std::string_view language);
        // ICU's default patterns: Decimal "#,##0.###", Integer "#,##0", Percent "#,##0%" (value x 100)
        static std::string formatNumber(double value, std::string_view language, NumberStyle style = NumberStyle::Decimal);
        static std::string formatInteger(int64_t value, std::string_view language);
    };

}

#endif //LOCALIZATION_PLURALRULES_H
