// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Fork features (phase-2 §3.6): the compiled-in table matches the runtime probes of the
// vendored RmlUi, and the set operations tests use to force both code paths.

#include "htmlui/compat/ForkFeatures.h"

#include "harness/TestSystem.h"

#include "HtmlUI.h"
#include "RmlSystem.h"

#include <RmlUi/Core/Core.h>

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

TEST(ForkFeatures, TableMatchesProbes){
    ASSERT_TRUE(HtmlUI::acquire());
    HtmlUI::getSystemInterface().resetCounters();

    const ForkFeatures present = ForkFeatures::present();
    size_t probed = 0;
    for (size_t i = 1; i < (size_t)ForkFeature::Count; i++){
        const ForkFeature feature = (ForkFeature)i;
        const std::optional<bool> result = ForkFeatures::probe(feature);
        if (feature == ForkFeature::GroupOpacity){
            EXPECT_FALSE(result.has_value()) << "GroupOpacity is not observable without rendering";
            continue;
        }
        ASSERT_TRUE(result.has_value()) << forkFeatureName(feature) << " could not be probed";
        EXPECT_EQ(*result, present.has(feature)) << forkFeatureName(feature) << ": the probe disagrees with kPresent";
        probed++;
    }
    EXPECT_EQ(probed, (size_t)ForkFeature::Count - 2);
    EXPECT_EQ(ForkFeatures::probe(), present);

    // the probes' warnings never reach the log, and they leave no context behind
    EXPECT_EQ(HtmlUI::getSystemInterface().getWarningCount(), 0u);
    EXPECT_EQ(HtmlUI::getSystemInterface().getErrorCount(), 0u);
    EXPECT_EQ(Rml::GetNumContexts(), 0);
    HtmlUI::release();
}

TEST(ForkFeatures, ProbesNeedRmlUi){
    ASSERT_FALSE(HtmlUI::isInitialized());
    EXPECT_FALSE(ForkFeatures::probe(ForkFeature::Calc).has_value());
    EXPECT_EQ(ForkFeatures::probe(), ForkFeatures::present());
}
