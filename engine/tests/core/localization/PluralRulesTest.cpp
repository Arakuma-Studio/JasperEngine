// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The generated CLDR plural table (docs/specs/game-ui-localization.md R4, AC2): every @integer and
// @decimal sample of the pinned CLDR release maps to its category, for every language.

#include <gtest/gtest.h>

#include "localization/PluralRules.h"

#include <string>

namespace PluralRulesTest_cpp {

using namespace jasperengine::l10n;

namespace {

    struct PluralSample {
        const char* language;
        const char* sample;
        PluralCategory category;
    };

#include "PluralSamples.gen.inc"

}

TEST(PluralRulesTest, GeneratedTableMatchesCldrSamples) {
    size_t checked = 0;
    for (const PluralSample& row : kPluralSamples) {
        PluralOperands operands;
        ASSERT_TRUE(PluralRules::operands(row.sample, operands)) << row.language << " " << row.sample;
        EXPECT_EQ(PluralRules::name(PluralRules::select(row.language, operands)), std::string(PluralRules::name(row.category)))
            << row.language << " " << row.sample;
        checked++;
    }
    EXPECT_GT(checked, 5000u);
}

}  // namespace PluralRulesTest_cpp
