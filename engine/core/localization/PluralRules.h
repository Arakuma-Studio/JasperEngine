// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PLURAL_RULES_H
#define PLURAL_RULES_H

// Contract (docs/specs/S95-lua-runtime-gaps.md REQ-26, D-13)
// - Purpose: the CLDR cardinal plural category of a count in a language, for String Tables'
//   plural values. A compiled table of the cardinal rules (no ordinals) of the languages S87's
//   language list names, selected by the tag's language subtag (and pt-PT's region); an unknown
//   language uses `one` (exactly 1) and `other`.
// - Operands: a double is read as its shortest decimal form (up to 6 fraction digits), so 1.0 is
//   "1" (one in English) and 1.5 is "1.5" (other).
// - Thread-safety: pure functions.

#include "Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine{

    // the order is the stored order of a plural value's forms
    enum class PluralCategory : uint8_t{
        ZERO = 0,
        ONE,
        TWO,
        FEW,
        MANY,
        OTHER
    };
    constexpr int PLURAL_CATEGORY_COUNT = 6;

    class JASPERENGINE_API PluralRules{
    public:
        // "zero", "one", "two", "few", "many", "other"
        static const char* name(PluralCategory category);
        // false for any other text
        static bool fromName(const std::string& name, PluralCategory& out);

        // the category of `count` in `language` (a BCP 47 tag, any case)
        static PluralCategory select(const std::string& language, double count);
        // the categories `language` uses, in PluralCategory order (always ends with OTHER)
        static std::vector<PluralCategory> categories(const std::string& language);
    };

}

#endif //PLURAL_RULES_H
