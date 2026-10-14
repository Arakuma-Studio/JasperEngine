// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// effect(<name>, <param> <value>, ...) and its typed RCSS form (phase-5 spec 3.2, 3.5, 5.1).

#include "htmlui/compat/DiagnosticCodes.h"
#include "htmlui/effects/EffectRegistry.h"
#include "htmlui/effects/EffectSyntax.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::htmlui;
namespace hc = jasperengine::htmlui::compat;

namespace {

    bool hasCode(const std::vector<EffectDiagnostic>& diags, hc::DiagCode code){
        for (const EffectDiagnostic& d : diags){
            if (d.code == code)
                return true;
        }
        return false;
    }

    const EffectDiagnostic* find(const std::vector<EffectDiagnostic>& diags, hc::DiagCode code){
        for (const EffectDiagnostic& d : diags){
            if (d.code == code)
                return &d;
        }
        return nullptr;
    }

}

TEST(EffectSyntaxTest, NamesAndArguments){
    EffectCall call;
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(EffectSyntax::parse("effect(Stripes, Speed 2, color-a rgb(0, 0, 0), angle var(--a, 10deg))", call, diags));
    EXPECT_TRUE(diags.empty());
    EXPECT_EQ(call.name, "stripes");
    ASSERT_EQ(call.args.size(), 3u);
    EXPECT_EQ(call.args[0].name, "speed");
    EXPECT_EQ(call.args[0].value, "2");
    // commas inside parentheses do not split arguments
    EXPECT_EQ(call.args[1].name, "color-a");
    EXPECT_EQ(call.args[1].value, "rgb(0, 0, 0)");
    EXPECT_EQ(call.args[2].value, "var(--a, 10deg)");
}

TEST(EffectSyntaxTest, CaseAndWhitespace){
    EffectCall call;
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(EffectSyntax::parse("  EFFECT( pixelate ,\n\tsize   12px  ,gap 0.1 )  ", call, diags));
    EXPECT_TRUE(diags.empty());
    EXPECT_EQ(call.name, "pixelate");
    ASSERT_EQ(call.args.size(), 2u);
    EXPECT_EQ(call.args[0].name, "size");
    EXPECT_EQ(call.args[0].value, "12px");
    EXPECT_EQ(call.args[1].name, "gap");
    EXPECT_EQ(call.args[1].value, "0.1");
}

TEST(EffectSyntaxTest, EmptyAndTrailingComma){
    EffectCall call;
    std::vector<EffectDiagnostic> diags;
    EXPECT_FALSE(EffectSyntax::parse("effect()", call, diags));
    EXPECT_TRUE(hasCode(diags, hc::codes::EffectUnknown));

    diags.clear();
    ASSERT_TRUE(EffectSyntax::parse("effect(stripes)", call, diags));
    EXPECT_TRUE(diags.empty());
    EXPECT_TRUE(call.args.empty());

    diags.clear();
    ASSERT_TRUE(EffectSyntax::parse("effect(stripes, speed 1, )", call, diags));
    EXPECT_EQ(call.args.size(), 1u);
    EXPECT_TRUE(hasCode(diags, hc::codes::EffectPositionalArg));
}

TEST(EffectSyntaxTest, PositionalArgumentAndOffsets){
    EffectCall call;
    std::vector<EffectDiagnostic> diags;
    const std::string text = "effect(stripes, 2, speed 1)";
    ASSERT_TRUE(EffectSyntax::parse(text, call, diags));
    const EffectDiagnostic* positional = find(diags, hc::codes::EffectPositionalArg);
    ASSERT_NE(positional, nullptr);
    EXPECT_EQ(positional->severity, hc::Severity::Error);
    // the column is the 1-based byte offset of the argument in the text
    EXPECT_EQ(positional->column, (uint32_t)text.find('2') + 1);
    ASSERT_EQ(call.args.size(), 1u);
    EXPECT_EQ(call.args[0].nameOffset, text.find("speed"));
    EXPECT_EQ(call.args[0].valueOffset, text.find("1)"));
    EXPECT_EQ(call.nameOffset, text.find("stripes"));
}

TEST(EffectSyntaxTest, DuplicateParamLastWins){
    const EffectManifest* m = EffectRegistry::get().find("stripes");
    ASSERT_NE(m, nullptr);
    EffectCall call;
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(EffectSyntax::parse("effect(stripes, density 3, density 7)", call, diags));
    const std::string rcss = EffectSyntax::toRcss(call, *m, false, "border-box", diags);
    EXPECT_TRUE(hasCode(diags, hc::codes::EffectDuplicateParam));
    EXPECT_NE(rcss.find("v(7)"), std::string::npos) << rcss;
    EXPECT_EQ(rcss.find("v(3)"), std::string::npos) << rcss;
}

TEST(EffectSyntaxTest, TypedFormHasEverySlotInOrder){
    const EffectManifest* m = EffectRegistry::get().find("stripes");
    ASSERT_NE(m, nullptr);
    EffectCall call;
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(EffectSyntax::parse("effect(stripes, speed 0.5, color-b red)", call, diags));
    const std::string rcss = EffectSyntax::toRcss(call, *m, false, "border-box", diags);
    EXPECT_TRUE(diags.empty());
    // color-a color-b angle density balance softness speed offset color-space, then the universal slots
    EXPECT_EQ(rcss, "jfx-stripes(v(#000000ff) v(#ff0000ff) v(45deg) v(5) v(0.5) v(0) v(0.5) v(0) v(linear) v(100%) v(1) v(0s) v(normal)) border-box");
    // a filter form of a paint effect keeps blend; a filter effect has none
    const std::string filterForm = EffectSyntax::toRcss(call, *m, true, "", diags);
    EXPECT_EQ(filterForm.find("border-box"), std::string::npos);
    EXPECT_NE(filterForm.find("v(normal))"), std::string::npos);
    const EffectManifest* pixelate = EffectRegistry::get().find("pixelate");
    ASSERT_NE(pixelate, nullptr);
    EffectCall pixel;
    ASSERT_TRUE(EffectSyntax::parse("effect(pixelate)", pixel, diags));
    EXPECT_EQ(EffectSyntax::toRcss(pixel, *pixelate, true, "", diags), "jfx-pixelate(v(8px) v(0) v(0) v(100%) v(1) v(0s))");
}

TEST(EffectSyntaxTest, PositionTakesTwoSlots){
    const EffectManifest* m = EffectRegistry::get().find("spotlight");
    ASSERT_NE(m, nullptr);
    EffectCall call;
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(EffectSyntax::parse("effect(spotlight, idle-position right 10px top)", call, diags));
    const std::string rcss = EffectSyntax::toRcss(call, *m, false, "padding-box", diags);
    EXPECT_TRUE(diags.empty()) << (diags.empty() ? "" : diags[0].message);
    EXPECT_NE(rcss.find("vx(calc(100% - 10px) 0%) vy(calc(100% - 10px) 0%)"), std::string::npos) << rcss;
    EXPECT_NE(rcss.find(") padding-box"), std::string::npos);
    const std::vector<std::string> slots = EffectSyntax::slotNames(*m);
    EXPECT_NE(std::find(slots.begin(), slots.end(), "idle-position-x"), slots.end());
    EXPECT_NE(std::find(slots.begin(), slots.end(), "idle-position-y"), slots.end());
}

TEST(EffectSyntaxTest, VarValuesKeptVerbatim){
    const EffectManifest* m = EffectRegistry::get().find("stripes");
    ASSERT_NE(m, nullptr);
    EffectCall call;
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(EffectSyntax::parse("effect(stripes, speed var(--s), angle calc(var(--a) * 2))", call, diags));
    const std::string rcss = EffectSyntax::toRcss(call, *m, false, "", diags);
    EXPECT_TRUE(diags.empty());
    EXPECT_NE(rcss.find("v(var(--s))"), std::string::npos) << rcss;
    EXPECT_NE(rcss.find("v(calc(var(--a) * 2))"), std::string::npos) << rcss;
}

TEST(EffectSyntaxTest, DiagnosticsOfArguments){
    const EffectManifest* m = EffectRegistry::get().find("stripes");
    ASSERT_NE(m, nullptr);
    EffectCall call;
    std::vector<EffectDiagnostic> diags;
    const std::string text = "effect(stripes, speeed 2, density lots, balance 4, blend lighten)";
    ASSERT_TRUE(EffectSyntax::parse(text, call, diags));
    const std::string rcss = EffectSyntax::toRcss(call, *m, false, "", diags);
    const EffectDiagnostic* unknown = find(diags, hc::codes::EffectUnknownParam);
    ASSERT_NE(unknown, nullptr);
    EXPECT_EQ(unknown->severity, hc::Severity::Warning);
    EXPECT_EQ(unknown->suggestion, "speed");
    EXPECT_EQ(unknown->column, (uint32_t)text.find("speeed") + 1);
    const EffectDiagnostic* bad = find(diags, hc::codes::EffectBadValue);
    ASSERT_NE(bad, nullptr);
    EXPECT_NE(bad->suggestion.find("a number in [1, 30]"), std::string::npos) << bad->suggestion;
    EXPECT_NE(rcss.find("v(5)"), std::string::npos);   // the default density
    const EffectDiagnostic* range = find(diags, hc::codes::EffectOutOfRange);
    ASSERT_NE(range, nullptr);
    EXPECT_NE(rcss.find("v(1) v(0) v(0.2)"), std::string::npos) << rcss;   // balance clamped to 1
    EXPECT_TRUE(hasCode(diags, hc::codes::EffectBadValue));                 // blend lighten is not an option
}

TEST(EffectSyntaxTest, BlendOnFilterEffect){
    const EffectManifest* m = EffectRegistry::get().find("pixelate");
    ASSERT_NE(m, nullptr);
    EffectCall call;
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(EffectSyntax::parse("effect(pixelate, blend add)", call, diags));
    const std::string rcss = EffectSyntax::toRcss(call, *m, true, "", diags);
    const EffectDiagnostic* blend = find(diags, hc::codes::EffectBlendOnFilter);
    ASSERT_NE(blend, nullptr);
    EXPECT_EQ(blend->severity, hc::Severity::Warning);
    EXPECT_EQ(rcss.find("add"), std::string::npos);
}

TEST(EffectSyntaxTest, Slots){
    std::string inner;
    char axis = 0;
    ASSERT_TRUE(EffectSyntax::unwrapSlot(" v(rgb(0 0 0 / 50%)) ", inner, axis));
    EXPECT_EQ(inner, "rgb(0 0 0 / 50%)");
    EXPECT_EQ(axis, 0);
    ASSERT_TRUE(EffectSyntax::unwrapSlot("vy(50% 0%)", inner, axis));
    EXPECT_EQ(inner, "50% 0%");
    EXPECT_EQ(axis, 'y');
    EXPECT_FALSE(EffectSyntax::unwrapSlot("v(1) v(2)", inner, axis));
    EXPECT_FALSE(EffectSyntax::unwrapSlot("12px", inner, axis));
    EXPECT_EQ(EffectSyntax::editDistance("speeed", "speed"), 1u);
    EXPECT_EQ(EffectSyntax::editDistance("", "abc"), 3u);
}
