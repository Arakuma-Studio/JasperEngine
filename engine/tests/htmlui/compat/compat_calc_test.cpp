// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ForkFeature::Calc (phase 3 p3-base: upstream PR #983 in the fork, docs/html-ui/FORK.md P3-1).
// With the feature, compat keeps calc(), min(), max() and clamp() where RmlUi reads the value as
// written (a property without a rewrite, the sizing, gap, font-size, line-height, z-index and
// opacity rewrites, custom properties; p3-values: logical properties and font-weight), checks
// their types, and folds them as phase 2 did where
// a rewrite reads the value token by token or writes it into compat decorator arguments. Every
// test runs with the feature present and forced absent.

#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/RmlLogTranslator.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/CssRewriter.h"

#include "HtmlUI.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    struct Rewritten {
        std::string rcss;
        std::vector<Diagnostic> diagnostics;

        // the value written for `property` in the first rule that has it ("" when none)
        std::string value(const std::string& property) const {
            const std::string key = "\n" + property + ": ";
            const size_t at = rcss.find(key);
            if (at == std::string::npos)
                return std::string();
            const size_t begin = at + key.size();
            const size_t end = rcss.find(";\n", begin);
            return rcss.substr(begin, end - begin);
        }
        bool reported(DiagCode code) const {
            for (const Diagnostic& d : diagnostics){
                if (d.code == code)
                    return true;
            }
            return false;
        }
        size_t warnings() const {
            size_t count = 0;
            for (const Diagnostic& d : diagnostics){
                if (d.severity != Severity::Info)
                    count++;
            }
            return count;
        }
    };

    Rewritten rewrite(const std::string& css, const ForkFeatures& fork){
        SourceFiles files;
        const uint32_t file = files.add("ui/calc.css", css);
        DiagnosticSink sink(files);
        CompatOptions options;
        options.fork = fork;
        RewriteContext context;
        context.files.read = [](const std::string&, std::string&){ return false; };
        context.files.exists = [](const std::string&){ return true; };
        CssRewriter rewriter(options, files, sink, context);
        StylesheetInput input;
        input.file = file;
        input.text = files.text(file);
        const RewriteResult result = rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
        Rewritten out;
        for (const StylesheetOutput& sheet : result.sheets)
            out.rcss += sheet.rcss;
        out.diagnostics = sink.diagnostics();
        return out;
    }

    ForkFeatures withCalc(){
        return ForkFeatures::present().with(ForkFeature::Calc, true);
    }
    ForkFeatures withoutCalc(){
        return ForkFeatures::present().with(ForkFeature::Calc, false);
    }

    // the properties RmlUi reads as written, with the expressions the fork evaluates
    const char* kNativeSheet = ".a {\n"
                               "  width: calc(100% - 32px);\n"
                               "  min-width: min(100%, 600px);\n"
                               "  height: clamp(10px, 50vh, 300px);\n"
                               "  margin: calc(10px + 4px) calc(2 * 8px);\n"
                               "  left: calc(50% - 10px);\n"
                               "  font-size: calc(1rem + 2px);\n"
                               "  gap: calc(4px * 2);\n"
                               "  line-height: calc(1em + 50%);\n"
                               "  flex-basis: calc(100% / 3);\n"
                               "  opacity: calc(1 / 2);\n"
                               "  z-index: calc(2 + 1);\n"
                               "  transform: translateX(calc(100% - 10px));\n"
                               "  max-height: -webkit-calc(100vh - 20px);\n"
                               "  padding-inline: calc(2px + 3px) calc(10% - 1px);\n"
                               "  font-weight: calc(300 + 100);\n"
                               "}\n";

}

TEST(CompatCalc, KeptWhereRmlUiReadsTheValue){
    ASSERT_TRUE(ForkFeatures::present().has(ForkFeature::Calc));
    const Rewritten out = rewrite(kNativeSheet, withCalc());
    EXPECT_EQ(out.value("width"), "calc(100% - 32px)");
    EXPECT_EQ(out.value("min-width"), "min(100%, 600px)");
    EXPECT_EQ(out.value("height"), "clamp(10px, 50vh, 300px)");
    EXPECT_EQ(out.value("margin"), "calc(10px + 4px) calc(2 * 8px)");
    EXPECT_EQ(out.value("left"), "calc(50% - 10px)");
    EXPECT_EQ(out.value("font-size"), "calc(1rem + 2px)");
    EXPECT_EQ(out.value("gap"), "calc(4px * 2)");
    EXPECT_EQ(out.value("line-height"), "calc(1em + 50%)");
    EXPECT_EQ(out.value("flex-basis"), "calc(100% / 3)");
    EXPECT_EQ(out.value("opacity"), "calc(1 / 2)");
    EXPECT_EQ(out.value("z-index"), "calc(2 + 1)");
    EXPECT_EQ(out.value("transform"), "translateX(calc(100% - 10px))");
    EXPECT_EQ(out.value("max-height"), "calc(100vh - 20px)");   // -webkit-calc
    // (p3-values) a logical property's physical properties, font-weight
    EXPECT_EQ(out.value("padding-left"), "calc(2px + 3px)");
    EXPECT_EQ(out.value("padding-right"), "calc(10% - 1px)");
    EXPECT_EQ(out.value("font-weight"), "calc(300 + 100)");
    EXPECT_EQ(out.warnings(), 0u) << out.rcss;
}

TEST(CompatCalc, FoldedWithoutTheFeature){
    const Rewritten out = rewrite(kNativeSheet, withoutCalc());
    EXPECT_EQ(out.value("width"), "100%");
    EXPECT_TRUE(out.reported(codes::ForkFallback));
    EXPECT_EQ(out.value("margin"), "14px 16px");
    EXPECT_EQ(out.value("gap"), "8px");
    EXPECT_EQ(out.value("opacity"), "0.5");
    EXPECT_EQ(out.value("z-index"), "3");
    EXPECT_EQ(out.value("padding-left"), "5px");
    EXPECT_EQ(out.value("font-weight"), "400");
    EXPECT_EQ(out.rcss.find("calc("), std::string::npos) << out.rcss;
}

TEST(CompatCalc, InvalidExpressionsAreDroppedInBothStates){
    for (const ForkFeatures& fork : {withCalc(), withoutCalc()}){
        const Rewritten out = rewrite(".a { width: calc(10px + 2deg); height: calc(100%-2px); left: min(10px, 2); top: 1px; }\n", fork);
        EXPECT_EQ(out.value("width"), "");
        EXPECT_EQ(out.value("height"), "");
        EXPECT_EQ(out.value("left"), "");
        EXPECT_EQ(out.value("top"), "1px");
        EXPECT_TRUE(out.reported(codes::UnsupportedValue));
    }
}

TEST(CompatCalc, OtherMathFunctionsAreFoldedInBothStates){
    for (const ForkFeatures& fork : {withCalc(), withoutCalc()}){
        const Rewritten out = rewrite(".a { width: calc(abs(-20px) + 10px); opacity: sin(30deg); }\n", fork);
        EXPECT_EQ(out.value("width"), "30px");
        EXPECT_EQ(out.value("opacity"), "0.5");
    }
}

TEST(CompatCalc, VariablesInsideAreTheForksToSubstitute){
    const char* css = ".a { width: calc(var(--runtime) + 10px); }\n";
    const Rewritten with = rewrite(css, withCalc());
    EXPECT_EQ(with.value("width"), "calc(var(--runtime) + 10px)");
    EXPECT_EQ(with.warnings(), 0u);
    const Rewritten without = rewrite(css, withoutCalc());
    EXPECT_EQ(without.value("width"), "");
    EXPECT_TRUE(without.reported(codes::VarUntyped));
}

TEST(CompatCalc, CustomPropertiesKeepItForRmlUi){
    const char* css = ":root { --w: calc(100% - 10px); }\n.a { width: var(--w); }\n";
    const Rewritten with = rewrite(css, withCalc());
    EXPECT_EQ(with.value("--w"), "calc(100% - 10px)");
    EXPECT_EQ(with.value("width"), "var(--w)");
    const Rewritten without = rewrite(css, withoutCalc());
    EXPECT_EQ(without.value("--w"), "100%");
}

TEST(CompatCalc, RewrittenFamiliesFoldItInBothStates){
    // border and box-shadow are rewritten token by token: no calc() reaches RmlUi through them
    const char* css = ".a { border-width: calc(1px + 1px); box-shadow: 0 calc(2px * 2) 4px #000; }\n";
    for (const ForkFeatures& fork : {withCalc(), withoutCalc()}){
        const Rewritten out = rewrite(css, fork);
        EXPECT_EQ(out.value("border-width"), "2px") << out.rcss;
        EXPECT_EQ(out.value("--html-box-shadow"), "0px 4px 4px #000000ff") << out.rcss;
        EXPECT_EQ(out.rcss.find("calc("), std::string::npos) << out.rcss;
        EXPECT_FALSE(out.reported(codes::ForkFallback));
    }
    // a percentage reduced in a rewritten value is an approximation of that rewrite, not a
    // fallback the feature removes
    const Rewritten mixed = rewrite(".a { background-image: linear-gradient(red, blue); background-position: calc(10% + 2px) 0; }\n", withCalc());
    EXPECT_TRUE(mixed.reported(codes::ApproximatedVisible)) << mixed.rcss;
    EXPECT_FALSE(mixed.reported(codes::ForkFallback));
}

// what compat keeps is what the fork parses: the sheet loads without a warning
TEST(CompatCalc, TheForkParsesWhatIsKept){
    ASSERT_TRUE(HtmlUI::acquire());
    const Rewritten out = rewrite(kNativeSheet, withCalc());
    {
        RmlLogTranslator::Capture capture;
        Rml::SharedPtr<Rml::StyleSheetContainer> sheet = Rml::Factory::InstanceStyleSheetString(out.rcss);
        EXPECT_NE(sheet, nullptr);
        for (const RmlLogTranslator::Capture::Message& message : capture.messages())
            ADD_FAILURE() << "RmlUi: " << message.text << "\n" << out.rcss;
    }
    // each kept value parses for its property; a length with a percentage stays a calculation
    for (const char* property : {"width", "min-width", "height", "margin", "left", "font-size", "gap", "line-height", "flex-basis", "opacity",
                                 "z-index", "transform", "max-height", "padding-left", "padding-right", "font-weight"}){
        Rml::PropertyDictionary dictionary;
        EXPECT_TRUE(Rml::StyleSheetSpecification::ParsePropertyDeclaration(dictionary, property, out.value(property))) << property;
    }
    Rml::PropertyDictionary width;
    ASSERT_TRUE(Rml::StyleSheetSpecification::ParsePropertyDeclaration(width, "width", out.value("width")));
    ASSERT_NE(width.GetProperty(Rml::PropertyId::Width), nullptr);
    EXPECT_EQ(width.GetProperty(Rml::PropertyId::Width)->unit, Rml::Unit::CALCULATION);
    Rml::Factory::ClearStyleSheetCache();
    HtmlUI::release();
}
