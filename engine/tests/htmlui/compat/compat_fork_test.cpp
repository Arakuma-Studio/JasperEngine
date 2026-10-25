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

TEST(ForkFeatures, PresentFeatures){
    // polarity: true = the fork has it; phase 2 shipped every entry false, phase 3 flips them
    // with the fork patches (docs/html-ui/FORK.md): Calc with upstream PR #983 (p3-base), Grid (p3-grid),
    // BorderStyle, PercentRadius, Outline, GroupOpacity, ObjectFit and BackgroundLayers (p3-paint), TextShadow and TextProperties (p3-text), TimingFunctions, AnimationLonghands, IndividualTransforms (p3-animation);
    // CurrentColor, ExtraUnits, LineHeightNormal (p3-values); Fixed, Sticky and BoxParity (p3-layout);
    // Important, CssWideKeywords, RootIsWhere, PseudoElements, Has, FocusPseudoClasses, SelectorsLevel4, ListMarkers (p3-cascade);
    // RelativeFontWeight and ColorFunctions (p3-values' bolder/lighter and runtime color functions, appended by p3-integration)
    const ForkFeatures present = ForkFeatures::present();
    const ForkFeatures expected = ForkFeatures::none()
                                      .with(ForkFeature::Calc, true)
                                      .with(ForkFeature::Grid, true)
                                      .with(ForkFeature::TextShadow, true)
                                      .with(ForkFeature::TextProperties, true)
                                      .with(ForkFeature::TimingFunctions, true)
                                      .with(ForkFeature::AnimationLonghands, true)
                                      .with(ForkFeature::IndividualTransforms, true)
                                      .with(ForkFeature::CurrentColor, true)
                                      .with(ForkFeature::ExtraUnits, true)
                                      .with(ForkFeature::LineHeightNormal, true)
                                      .with(ForkFeature::Fixed, true)
                                      .with(ForkFeature::Sticky, true)
                                      .with(ForkFeature::BoxParity, true)
                                      .with(ForkFeature::Important, true)
                                      .with(ForkFeature::CssWideKeywords, true)
                                      .with(ForkFeature::RootIsWhere, true)
                                      .with(ForkFeature::Has, true)
                                      .with(ForkFeature::FocusPseudoClasses, true)
                                      .with(ForkFeature::SelectorsLevel4, true)
                                      .with(ForkFeature::PseudoElements, true)
                                      .with(ForkFeature::ListMarkers, true)
                                      .with(ForkFeature::BorderStyle, true)
                                      .with(ForkFeature::PercentRadius, true)
                                      .with(ForkFeature::Outline, true)
                                      .with(ForkFeature::GroupOpacity, true)
                                      .with(ForkFeature::ObjectFit, true)
                                      .with(ForkFeature::BackgroundLayers, true)
                                      .with(ForkFeature::RelativeFontWeight, true)
                                      .with(ForkFeature::ColorFunctions, true)
                                      .with(ForkFeature::TextFillFilter, true)   // phase 5, p5-infra: -jfx-text-fill (P5-1)
                                      .with(ForkFeature::RegisteredProperties, true)    // phase 5, p5-property
                                      .with(ForkFeature::Justify, true)                 // p6-justify (P6-J1)
                                      .with(ForkFeature::TextIndent, true)              // p6-justify (P6-J2)
                                      .with(ForkFeature::LineClamp, true)               // phase 6, p6-line-clamp (P6-L1)
                                      .with(ForkFeature::RowAliases, true)              // S17 (P7-D)
                                      .with(ForkFeature::LayoutBoundary, true);         // S17 (P7-L)
    for (size_t i = 0; i < (size_t)ForkFeature::Count; i++)
        EXPECT_EQ(present.has((ForkFeature)i), expected.has((ForkFeature)i)) << forkFeatureName((ForkFeature)i);
    EXPECT_EQ(present, expected);
    // phase 3 is complete: the fork has every feature phase 2 and 3 named
    EXPECT_EQ(present, ForkFeatures::all());
}

TEST(ForkFeatures, WithBitsAndNames){
    const ForkFeatures none = ForkFeatures::none();
    const ForkFeatures calc = none.with(ForkFeature::Calc, true);
    EXPECT_TRUE(calc.has(ForkFeature::Calc));
    EXPECT_FALSE(calc.has(ForkFeature::Grid));
    EXPECT_FALSE(none.has(ForkFeature::Calc));   // with() returns a copy
    EXPECT_NE(calc, none);
    EXPECT_EQ(calc.with(ForkFeature::Calc, false), none);
    EXPECT_FALSE(calc.with(ForkFeature::None, true).has(ForkFeature::None));

    const ForkFeatures all = ForkFeatures::all();
    for (size_t i = 1; i < (size_t)ForkFeature::Count; i++)
        EXPECT_TRUE(all.has((ForkFeature)i));
    EXPECT_FALSE(all.has(ForkFeature::None));
    EXPECT_EQ(ForkFeatures::fromBits(all.bits()), all);
    EXPECT_EQ(ForkFeatures::fromBits(~uint64_t(0)), all);
    EXPECT_EQ(ForkFeatures::fromBits(calc.bits()), calc);

    EXPECT_STREQ(forkFeatureName(ForkFeature::TimingFunctions), "TimingFunctions");
    EXPECT_STREQ(forkFeatureName(ForkFeature::None), "None");
    for (size_t i = 1; i < (size_t)ForkFeature::Count; i++)
        EXPECT_EQ(findForkFeature(forkFeatureName((ForkFeature)i)), (ForkFeature)i);
    EXPECT_EQ(findForkFeature("NoSuchFeature"), ForkFeature::None);
}
