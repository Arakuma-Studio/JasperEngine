// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Compat consistency with the fork (phase-3 spec 5.4). Every fork feature (compat::ForkFeature,
// phase-2 §3.6, phase-3 Appendix A) has sample declarations or selectors below:
//
//   present (ForkFeatures::present() has it): RmlUi parses the sample, compat passes it through
//            (normalized: RmlUi parses compat's output to the same properties) and reports nothing;
//   absent  (forced off with ForkFeatures::with): compat still produces phase 2's rewrite or
//            emulation (the code is kept, spec §1), seen in its output or its diagnostic.
//
// A table row whose support becomes Native with the present features (effectiveSupport) needs a
// sample of its feature and property here, so a wave-1 package that flips a feature (kPresent)
// adds the samples of the rows it makes native in the same merge ("every package adds rows").

#include "CompatTestCommon.h"

#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/css/CssRewriter.h"

#include "HtmlUI.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    enum class SampleKind {
        Declaration,   // a style="" declaration (CssRewriter::rewriteDeclarations)
        Selector,      // a selector (CssRewriter::rewriteSelector)
        Rule,          // the declaration in a stylesheet rule ".sample { ... }" (an inline <style>)
    };

    struct FeatureSample {
        ForkFeature feature;
        SampleKind kind;
        const char* css;        // "border-top: 2px dashed red" or ".a::before"
        // absent: a piece of compat's output (normalized; for a Rule, of the rule's block and of
        // compat's !important sheet), or "" when phase 2 leaves the CSS as it is and the
        // difference is its diagnostic `code` (or nothing at all: GroupOpacity, with code "" too:
        // phase 2 documents it and does not rewrite)
        const char* absentOutput;
        const char* absentCode;
    };

    // One or more per feature; append the samples of the rows a feature makes Native.
    const FeatureSample kSamples[] = {
        {ForkFeature::Calc, SampleKind::Declaration, "width: calc(10px + 20px)", "width:30px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-top: 2px dashed red", "border-top-width:2px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border: 3px double blue", "border-top-width:3px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-right: 1px dotted #00ff00", "border-right-width:1px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-bottom: 6px groove red", "border-bottom-width:6px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-left: 2px ridge red", "border-left-width:2px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-style: dashed solid", "border-top-width:3px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-top-style: inset", "border-top-width:3px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-right-style: outset", "border-right-width:3px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-bottom-style: hidden", "border-bottom-width:0px", ""},
        {ForkFeature::BorderStyle, SampleKind::Declaration, "border-left-style: none", "border-left-width:0px", ""},
        {ForkFeature::PercentRadius, SampleKind::Declaration, "border-radius: 50%", "9999px", ""},
        {ForkFeature::PseudoElements, SampleKind::Selector, ".a::before", "", "HTMLUI-SEL-0003"},
        {ForkFeature::RootIsWhere, SampleKind::Selector, ":is(.a, .b) > .c", ".a>.c,.b>.c", ""},
        {ForkFeature::Fixed, SampleKind::Declaration, "position: fixed", "", "HTMLUI-CSS-0007"},
        {ForkFeature::GroupOpacity, SampleKind::Declaration, "opacity: 0.5", "", ""},
        {ForkFeature::TimingFunctions, SampleKind::Declaration, "transition: opacity 0.2s ease-in-out", "quadratic-in-out", ""},
        {ForkFeature::Important, SampleKind::Rule, "color: red !important", ".sample{color:#ff0000ff;", ""},
        {ForkFeature::CssWideKeywords, SampleKind::Declaration, "color: inherit", "var(", ""},
        {ForkFeature::Grid, SampleKind::Declaration, "display: grid", "display:flex", ""},
    };

    // lower case, no white space, no trailing ';'
    std::string normalized(std::string_view css){
        std::string out;
        for (char c : css){
            if (!std::isspace((unsigned char)c))
                out += (char)std::tolower((unsigned char)c);
        }
        while (!out.empty() && out.back() == ';')
            out.pop_back();
        return out;
    }

    struct Rewrite {
        std::string output;      // declarations (Declaration, Rule: the rule's block), or the selector
        std::string important;   // Rule: compat's !important sheet
        std::vector<Diagnostic> diagnostics;
    };

    Rewrite rewrite(const FeatureSample& sample, const ForkFeatures& fork){
        SourceFiles files;
        DiagnosticSink sink(files);
        CompatOptions options;
        options.fork = fork;
        Rewrite result;
        if (sample.kind == SampleKind::Selector){
            result.output = CssRewriter::rewriteSelector(sample.css, &sink, options);
        }else if (sample.kind == SampleKind::Declaration){
            result.output = CssRewriter::rewriteDeclarations(sample.css, &sink, options);
        }else{
            const std::string sheet = std::string(".sample { ") + sample.css + " }";
            const uint32_t file = files.add("ui/sample.html", sheet);
            CssRewriter rewriter(options, files, sink);
            StylesheetInput input;
            input.kind = StylesheetInput::Kind::Inline;
            input.file = file;
            input.text = files.text(file);
            const RewriteResult rewritten = rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
            if (!rewritten.sheets.empty()){
                const std::string& rcss = rewritten.sheets[0].rcss;
                const size_t open = rcss.find('{'), close = rcss.rfind('}');
                if (open != std::string::npos && close != std::string::npos && close > open)
                    result.output = rcss.substr(open + 1, close - open - 1);
            }
            result.important = rewritten.importantSheet.rcss;
        }
        result.diagnostics = sink.diagnostics();
        return result;
    }

    bool hasCode(const std::vector<Diagnostic>& diagnostics, const std::string& code){
        return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& d){ return d.code.id() == code; });
    }

    std::string describe(const Rewrite& rewrite){
        std::string text = "output '" + rewrite.output + "'";
        if (!rewrite.important.empty())
            text += ", !important sheet '" + rewrite.important + "'";
        for (const Diagnostic& d : rewrite.diagnostics)
            text += "; " + toText(d);
        return text;
    }

    // a declaration list as RmlUi parses it (custom properties, compat's companions, left out);
    // false when RmlUi rejects a declaration
    bool parseDeclarations(const std::string& css, Rml::PropertyDictionary& dictionary, std::string* rejected){
        size_t at = 0;
        while (at < css.size()){
            size_t end = css.find(';', at);
            if (end == std::string::npos)
                end = css.size();
            const std::string declaration = css.substr(at, end - at);
            at = end + 1;
            const size_t colon = declaration.find(':');
            if (colon == std::string::npos)
                continue;
            std::string name = declaration.substr(0, colon), value = declaration.substr(colon + 1);
            name.erase(0, name.find_first_not_of(" \t\n"));
            name.erase(name.find_last_not_of(" \t\n") + 1);
            if (name.rfind("--", 0) == 0)
                continue;
            const size_t important = value.find("!important");
            if (important != std::string::npos)
                value.erase(important);
            if (!Rml::StyleSheetSpecification::ParsePropertyDeclaration(dictionary, name, value)){
                if (rejected)
                    *rejected = declaration;
                return false;
            }
        }
        return true;
    }

    // spec 5.4 (a)-(c) for one sample with the given features: "" when RmlUi parses it, compat
    // passes it through (normalized: the same properties and values as RmlUi parses them) and
    // reports nothing (no fallback, approximation or note); else what is wrong. Needs RmlUi
    // initialised.
    std::string passThroughProblem(const FeatureSample& sample, const ForkFeatures& fork){
        const Rewrite result = rewrite(sample, fork);
        if (!result.diagnostics.empty())
            return "a diagnostic: " + toText(result.diagnostics[0]);
        if (sample.kind == SampleKind::Selector){
            Rml::SharedPtr<Rml::StyleSheetContainer> container = Rml::Factory::InstanceStyleSheetString(std::string(sample.css) + " { color: red; }");
            if (!container)
                return "RmlUi does not parse the selector";
            return normalized(result.output) == normalized(sample.css) ? std::string() : "rewritten: " + describe(result);
        }
        Rml::PropertyDictionary expected, actual;
        std::string rejected;
        if (!parseDeclarations(sample.css, expected, &rejected))
            return "RmlUi rejects '" + rejected + "'";
        if (!parseDeclarations(result.output, actual, &rejected))
            return "RmlUi rejects compat's '" + rejected + "'";
        if (!result.important.empty())
            return "hoisted: " + describe(result);
        if (expected.GetNumProperties() != actual.GetNumProperties())
            return "other properties: " + describe(result);
        for (const auto& property : expected.GetProperties()){
            const Rml::Property* other = actual.GetProperty(property.first);
            if (!other || other->ToString() != property.second.ToString())
                return "other values: " + describe(result);
        }
        return std::string();
    }

}

TEST(NativeFeatureConsistency, EveryFeatureHasASample){
    for (size_t i = 1; i < (size_t)ForkFeature::Count; i++){
        const ForkFeature feature = (ForkFeature)i;
        EXPECT_TRUE(std::any_of(std::begin(kSamples), std::end(kSamples), [&](const FeatureSample& s){ return s.feature == feature; }))
            << forkFeatureName(feature) << " has no sample in NativeFeatureConsistencyTest.cpp";
    }
}

// The rows a present feature makes Native have a sample of that feature and property.
TEST(NativeFeatureConsistency, NativeRowsHaveSamples){
    const ForkFeatures present = ForkFeatures::present();
    size_t rows = 0;
    for (const PropertySupport& row : propertyTable()){
        if (row.fork == ForkFeature::None || !present.has(row.fork) || effectiveSupport(row, present) != Support::Native)
            continue;
        rows++;
        const std::string property = row.name;
        EXPECT_TRUE(std::any_of(std::begin(kSamples), std::end(kSamples), [&](const FeatureSample& s){
            const std::string css = s.css;
            return s.feature == row.fork && s.kind != SampleKind::Selector && css.compare(0, property.size() + 1, property + ":") == 0;
        })) << "property " << row.name << " is Native with " << forkFeatureName(row.fork) << ": add a sample";
    }
    for (const SelectorSupport& row : selectorTable()){
        if (row.fork == ForkFeature::None || !present.has(row.fork) || effectiveSupport(row, present) != Support::Native)
            continue;
        rows++;
        EXPECT_TRUE(std::any_of(std::begin(kSamples), std::end(kSamples), [&](const FeatureSample& s){ return s.feature == row.fork && s.kind == SampleKind::Selector; }))
            << "selector " << row.name << " is Native with " << forkFeatureName(row.fork) << ": add a selector sample";
    }
    std::cout << "[ NATIVE   ] " << rows << " table rows native through present fork features\n";
}

// The checker itself: phase 2 leaves `opacity` alone and rewrites `display: grid`.
TEST(NativeFeatureConsistency, PassThroughCheckSeesRewrites){
    ASSERT_TRUE(HtmlUI::acquire());
    const ForkFeatures none = ForkFeatures::none();
    EXPECT_EQ(passThroughProblem({ForkFeature::GroupOpacity, SampleKind::Declaration, "opacity: 0.5", "", ""}, none), "");
    EXPECT_EQ(passThroughProblem({ForkFeature::None, SampleKind::Declaration, "width: 10px; height: 20%", "", ""}, none), "");
    EXPECT_EQ(passThroughProblem({ForkFeature::None, SampleKind::Selector, ".a > .b:hover", "", ""}, none), "");
    EXPECT_NE(passThroughProblem({ForkFeature::Grid, SampleKind::Declaration, "display: grid", "", ""}, none), "");
    EXPECT_NE(passThroughProblem({ForkFeature::RootIsWhere, SampleKind::Selector, ":is(.a, .b) > .c", "", ""}, none), "");
    EXPECT_NE(passThroughProblem({ForkFeature::Important, SampleKind::Rule, "color: red !important", "", ""}, none), "");
    EXPECT_NE(passThroughProblem({ForkFeature::Fixed, SampleKind::Declaration, "position: fixed", "", ""}, none), "");
    HtmlUI::release();
}

// present: RmlUi parses it, compat passes it through and reports nothing
TEST(NativeFeatureConsistency, PresentFeaturesPassThrough){
    const ForkFeatures present = ForkFeatures::present();
    ASSERT_TRUE(HtmlUI::acquire());
    size_t checked = 0;
    for (const FeatureSample& sample : kSamples){
        if (!present.has(sample.feature))
            continue;
        checked++;
        EXPECT_EQ(passThroughProblem(sample, present), "") << forkFeatureName(sample.feature) << ": " << sample.css;
    }
    HtmlUI::release();
    std::cout << "[ NATIVE   ] " << checked << " samples of present fork features checked\n";
}

// absent: phase 2's rewrite or emulation is still there
TEST(NativeFeatureConsistency, AbsentFeaturesKeepPhaseTwoRewrites){
    for (const FeatureSample& sample : kSamples){
        SCOPED_TRACE(std::string(forkFeatureName(sample.feature)) + ": " + sample.css);
        const Rewrite result = rewrite(sample, ForkFeatures::present().with(sample.feature, false));
        const std::string output = normalized(result.output);
        if (*sample.absentOutput){
            EXPECT_NE((output + " " + normalized(result.important)).find(normalized(sample.absentOutput)), std::string::npos) << describe(result);
        }
        if (*sample.absentCode){
            EXPECT_TRUE(hasCode(result.diagnostics, sample.absentCode)) << describe(result);
        }
        if (!*sample.absentOutput && !*sample.absentCode){
            EXPECT_EQ(output, normalized(sample.css)) << "phase 2 leaves it as it is: " << describe(result);
        }
    }
}
