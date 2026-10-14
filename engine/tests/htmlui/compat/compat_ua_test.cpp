// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The user-agent stylesheet (phase-2 spec 3.10, p2-ua): it rewrites with zero diagnostics in
// every fork-feature state, RmlUi accepts the result without a warning, no UA selector reaches
// the author band's 1 000 000 (an id), every element the element table gives a UA display has
// a rule with that display, and the internal images it references are embedded and served.
// Its look is calibrated by the golden pages (compat_golden_pages_test.cpp).

#include "CompatTestCommon.h"

#include "harness/TestSystem.h"

#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/compat/css/CssRewriter.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/CssTokenizer.h"
#include "htmlui/compat/ua/UserAgentStyle.h"

#include "HtmlUI.h"
#include "util/LunaSVGRuntime.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test::compat;

namespace {

    struct RmlUiSession {
        bool ok;
        RmlUiSession(): ok(HtmlUI::acquire()) {}
        ~RmlUiSession(){
            Rml::Factory::ClearStyleSheetCache();
            if (ok)
                HtmlUI::release();
        }
    };

    // the fork-feature sets the sheet must rewrite cleanly in: none, all, present() and each
    // feature alone
    std::vector<ForkFeatures> forkStates(){
        std::vector<ForkFeatures> states = {ForkFeatures::none(), ForkFeatures::all(), ForkFeatures::present()};
        for (size_t i = 1; i < (size_t)ForkFeature::Count; i++)
            states.push_back(ForkFeatures::none().with((ForkFeature)i, true));
        return states;
    }

    std::string describe(const ForkFeatures& fork){
        std::string text;
        for (size_t i = 1; i < (size_t)ForkFeature::Count; i++){
            if (fork.has((ForkFeature)i))
                text += std::string(text.empty() ? "" : "+") + forkFeatureName((ForkFeature)i);
        }
        return text.empty() ? "no fork features" : text;
    }

    // every style rule of a sheet, @media bodies included
    void collectStyleRules(const RuleList& rules, std::vector<const Rule*>& out){
        for (const Rule& rule : rules){
            if (rule.kind == Rule::Kind::Style)
                out.push_back(&rule);
            collectStyleRules(rule.rules, out);
        }
    }

    std::vector<std::string> selectorsOf(const Rule& rule){
        std::vector<std::string> selectors;
        for (const ValueList& part : splitAtCommas(rule.prelude))
            selectors.push_back(serialize(trimWhitespace(part)));
        return selectors;
    }

    int specificityOf(std::string_view selector);

    // RmlUi's specificity (StyleSheetNode::CalculateAndSetSpecificity): tag 10 000; class,
    // attribute and pseudo-class 100 000; id 1 000 000; :not(x) the largest of its arguments
    int specificityOfTokens(const std::vector<Token>& tokens, size_t begin, size_t end){
        int specificity = 0;
        bool compoundStart = true;
        for (size_t i = begin; i < end; i++){
            const Token& t = tokens[i];
            switch (t.type){
                case TokenType::Whitespace:
                    compoundStart = true;
                    continue;
                case TokenType::Delim:
                    if (t.value == ".")
                        specificity += 100000, i++;   // the class name
                    else if (t.value == ">" || t.value == "+" || t.value == "~")
                        compoundStart = true;
                    else if (t.value == "*")
                        compoundStart = false;
                    continue;
                case TokenType::Hash:
                    specificity += 1000000;
                    compoundStart = false;
                    continue;
                case TokenType::Ident:
                    if (compoundStart)
                        specificity += 10000;
                    compoundStart = false;
                    continue;
                case TokenType::LeftBracket:{
                    specificity += 100000;
                    int depth = 1;
                    while (++i < end && depth > 0){
                        if (tokens[i].type == TokenType::LeftBracket) depth++;
                        if (tokens[i].type == TokenType::RightBracket) depth--;
                    }
                    i--;
                    compoundStart = false;
                    continue;
                }
                case TokenType::Colon:{
                    compoundStart = false;
                    if (i + 1 < end && tokens[i + 1].type == TokenType::Colon)
                        i++;   // a pseudo-element: RmlUi has none, counted as a pseudo-class
                    if (i + 1 >= end)
                        continue;
                    const Token& name = tokens[++i];
                    if (name.type == TokenType::Function){
                        size_t close = i + 1;
                        int depth = 1;
                        for (; close < end; close++){
                            if (tokens[close].type == TokenType::Function || tokens[close].type == TokenType::LeftParen) depth++;
                            if (tokens[close].type == TokenType::RightParen && --depth == 0) break;
                        }
                        if (name.value == "not"){
                            int best = 0;
                            size_t from = i + 1;
                            for (size_t k = i + 1; k <= close; k++){
                                if (k == close || tokens[k].type == TokenType::Comma){
                                    best = std::max(best, specificityOfTokens(tokens, from, k));
                                    from = k + 1;
                                }
                            }
                            specificity += best;
                        }else{
                            specificity += 100000;
                        }
                        i = close;
                    }else{
                        specificity += 100000;
                    }
                    continue;
                }
                default:
                    continue;
            }
        }
        return specificity;
    }

    int specificityOf(std::string_view selector){
        std::vector<Token> tokens = tokenize(selector);
        tokens.erase(std::remove_if(tokens.begin(), tokens.end(), [](const Token& t){
            return t.type == TokenType::Comment || t.type == TokenType::EndOfFile; }), tokens.end());
        return specificityOfTokens(tokens, 0, tokens.size());
    }

}

TEST(CompatUaSheet, SpecificityHelperFollowsRmlUi){
    EXPECT_EQ(specificityOf("input"), 10000);
    EXPECT_EQ(specificityOf("input[type=\"checkbox\"]:checked"), 210000);
    EXPECT_EQ(specificityOf("details:not([open]) > :not(summary)"), 120000);
    EXPECT_EQ(specificityOf("select selectbox option:hover"), 130000);
    EXPECT_EQ(specificityOf("#htmlui-root .btn"), 1100000);
    EXPECT_EQ(specificityOf("body#htmlui-root > htmlbody"), 1020000);
    EXPECT_EQ(specificityOf(".htmlui-unsupported[data-html-tag=\"audio\"]"), 200000);
    EXPECT_EQ(specificityOf("*"), 0);
}

TEST(CompatUaSheet, RewritesWithoutDiagnosticsInEveryForkState){
    for (const ForkFeatures& fork : forkStates()){
        const std::shared_ptr<const UserAgentStyle::Sheet> sheet = UserAgentStyle::get(fork);
        ASSERT_NE(sheet, nullptr);
        EXPECT_FALSE(sheet->rcss.empty()) << describe(fork);
        EXPECT_TRUE(sheet->diagnostics.empty()) << describe(fork) << ": " << sheet->diagnostics.size() << " diagnostics, first: "
                                                << (sheet->diagnostics.empty() ? "" : toText(sheet->diagnostics[0]));
    }
    UserAgentStyle::clearCache();
}

TEST(CompatUaSheet, RmlUiAcceptsItWithoutWarnings){
    RmlUiSession rml;
    ASSERT_TRUE(rml.ok);
    // the states this RmlUi build can parse: with a feature the fork lacks, compat passes the
    // CSS through (border-style, calc(), ...) and RmlUi 6.3 rejects it, as it must
    for (const ForkFeatures& fork : {ForkFeatures::present(), ForkFeatures::none()}){
        const std::shared_ptr<const UserAgentStyle::Sheet> sheet = UserAgentStyle::get(fork);
        ASSERT_NE(sheet, nullptr);
        const std::vector<Diagnostic> verified = verifyRcssWithRmlUi(sheet->rcss, sheet->virtualPath, sheet->lines, *sheet->files);
        EXPECT_TRUE(verified.empty()) << describe(fork) << ": " << verified.size() << " RmlUi messages, first: "
                                      << (verified.empty() ? "" : toText(verified[0]));
    }
    UserAgentStyle::clearCache();
}

// the root band (3.8): one id makes every author rule outrank every UA rule
TEST(CompatUaSheet, NoSelectorReachesTheAuthorBand){
    for (const ForkFeatures& fork : {ForkFeatures::present(), ForkFeatures::all()}){
        const std::shared_ptr<const UserAgentStyle::Sheet> sheet = UserAgentStyle::get(fork);
        ASSERT_NE(sheet, nullptr);
        SourceFiles files;
        const uint32_t file = files.add("ua.rcss", sheet->rcss);
        DiagnosticSink sink(files);
        const Stylesheet parsed = parseStylesheet(file, files.text(file), sink);
        std::vector<const Rule*> rules;
        collectStyleRules(parsed.rules, rules);
        ASSERT_GT(rules.size(), 50u) << describe(fork);
        int highest = 0;
        std::string highestSelector;
        for (const Rule* rule : rules){
            for (const std::string& selector : selectorsOf(*rule)){
                EXPECT_EQ(selector.find('#'), std::string::npos) << describe(fork) << ": an id in UA selector '" << selector << "'";
                const int specificity = specificityOf(selector);
                if (specificity > highest){
                    highest = specificity;
                    highestSelector = selector;
                }
            }
        }
        // rule order adds at most the number of rules on top
        EXPECT_LT(highest + int(rules.size()), 1000000) << describe(fork) << ": '" << highestSelector << "' " << highest;
    }
    UserAgentStyle::clearCache();
}

// Appendix D: every element with a UA display has a rule giving it that display. The rule is
// looked up in the standard-CSS source under the selector the element is written with:
// its own name, the placeholder class for the elements compat replaces with
// div.htmlui-unsupported, svg for data-lucide.
TEST(CompatUaSheet, EveryElementWithADisplayHasARule){
    const std::string_view source = UserAgentStyle::source();
    ASSERT_FALSE(source.empty());
    SourceFiles files;
    const uint32_t file = files.add(UserAgentStyle::kSourcePath, std::string(source));
    DiagnosticSink sink(files);
    const Stylesheet parsed = parseStylesheet(file, files.text(file), sink);
    EXPECT_TRUE(sink.diagnostics().empty());

    // selector (a single compound) -> the display it declares
    std::map<std::string, std::string> displays;
    std::vector<const Rule*> rules;
    collectStyleRules(parsed.rules, rules);
    for (const Rule* rule : rules){
        for (const Declaration& declaration : rule->declarations){
            if (declaration.name != "display")
                continue;
            const std::string value = serialize(trimWhitespace(declaration.value));
            for (const std::string& selector : selectorsOf(*rule))
                displays[selector] = value;
        }
    }

    size_t checked = 0;
    for (const ElementSupport& element : elementTable()){
        if (!element.display || !*element.display)
            continue;
        std::string selector = element.name;
        const std::string rmlTag = element.rmlTag ? element.rmlTag : "";
        if (rmlTag == "div")
            selector = ".htmlui-unsupported";            // the placeholder of video, canvas, ...
        else if (selector == "[data-lucide]")
            selector = "svg";                            // the expanded icon
        else if (selector == "dotlottie-player" || selector == "dotlottie-wc" || selector == "lottie-player")
            selector = "lottie";
        else if (selector == "meter")
            selector = "progress";
        const auto found = displays.find(selector);
        if (found == displays.end()){
            ADD_FAILURE() << "<" << element.name << ">: no UA rule '" << selector << "' with display: " << element.display;
            continue;
        }
        EXPECT_EQ(found->second, element.display) << "<" << element.name << "> (rule '" << selector << "')";
        checked++;
    }
    EXPECT_GT(checked, 80u);

    // and the rewritten sheet keeps display for li (list-item, or block + markers before the
    // fork's ListMarkers), the root and the body
    const std::shared_ptr<const UserAgentStyle::Sheet> sheet = UserAgentStyle::get();
    EXPECT_NE(sheet->rcss.find("htmlbody"), std::string::npos);
    UserAgentStyle::clearCache();
}

// Chromium 153's light theme values the golden pages calibrate (spot checks of the source)
TEST(CompatUaSheet, SourceCarriesChromiumDefaults){
    const std::string source(UserAgentStyle::source());
    for (const char* expected : {"margin: 8px;", "font-size: 2em;", "margin: 0.67em 0;", "padding-left: 40px;", "border-spacing: 2px;",
                                 "font-size: 13.333px;", "width: var(--html-check-size, 13px);", "width: 129px;", "width: 10em;", "#767676", "#efefef",
                                 "var(--html-accent, #0075ff)", "--html-theme-fill: var(--html-control-bg, #ffffff);", "var(--html-control-bg, #ffffff)",
                                 "var(--html-control-border, #767676)", "var(--html-select-arrow,", "var(--html-thumb,",
                                 "--html-box-shadow: 0 0 0 0 #0000;", "--html-outline-shadow: 0 0 0 0 #0000;", ":focus-visible",
                                 "scrollbarvertical", "width: 15px;", "selection", ".htmlui-unsupported"})
        EXPECT_NE(source.find(expected), std::string::npos) << expected;
    // never an id, never a selector that needs the fork (comments aside)
    std::string code;
    for (const Token& token : tokenize(source)){
        if (token.type != TokenType::Comment && token.type != TokenType::EndOfFile)
            code += source.substr(token.offset, token.length);
    }
    for (const char* forbidden : {"#htmlui-root", ":is(", ":where(", ":has(", "::before", "::after", "!important"})
        EXPECT_EQ(code.find(forbidden), std::string::npos) << forbidden;
}

// The images the sheet references are embedded with it, served as @htmlui/ua/<name>, and are
// valid SVG
TEST(CompatUaSheet, InternalImagesAreEmbeddedAndServed){
    const std::string source(UserAgentStyle::source());
    std::set<std::string> referenced;
    for (size_t at = source.find("/@htmlui/ua/"); at != std::string::npos; at = source.find("/@htmlui/ua/", at + 1)){
        const size_t end = source.find('"', at);
        ASSERT_NE(end, std::string::npos);
        referenced.insert(source.substr(at + 12, end - at - 12));
    }
    EXPECT_GE(referenced.size(), 5u);   // select arrow, four scrollbar arrows

    UserAgentStyle::get();
    for (const std::string& name : referenced){
        SCOPED_TRACE(name);
        const std::shared_ptr<const std::string> file = VirtualFiles::find("@htmlui/ua/" + name);
        ASSERT_NE(file, nullptr);
        const auto embedded = std::find_if(UserAgentStyle::embeddedFiles().begin(), UserAgentStyle::embeddedFiles().end(),
                                           [&](const UserAgentStyle::EmbeddedFile& f){ return name == f.name; });
        ASSERT_NE(embedded, UserAgentStyle::embeddedFiles().end());
        std::vector<uint8_t> rgba;
        int width = 0, height = 0;
        EXPECT_TRUE(LunaSVGRuntime::rasterize(file->data(), file->size(), 2.0f, false, rgba, width, height)) << "not a valid SVG";
        EXPECT_GT(width, 0);
        size_t ink = 0;
        for (size_t i = 3; i < rgba.size(); i += 4)
            ink += rgba[i] > 0;
        EXPECT_GE(ink, std::min<size_t>(10u, (size_t)width * (size_t)height)) << "draws nothing";
        EXPECT_GT(ink, 0u) << "draws nothing";
    }
    UserAgentStyle::clearCache();
}

// `appearance: auto` (p2-css) gives a control the user-agent look back: for every companion
// `appearance: none` cleared it writes the fallback the sheet (p2-ua) reads the companion
// with. It writes one value per companion, whatever element the rule selects, so every
// var() read of such a companion in the sheet must have the same fallback, and that is the
// value written. (The range track read --html-control-bg/-border with its own gray fallbacks:
// `input[type=range] { appearance: auto }` drew the checkbox's white box and dark border.)
TEST(CompatUaSheet, AppearanceAutoWritesTheSheetsFallbacks){
    const auto declarations = [](const std::string& css){
        std::map<std::string, std::string> out;
        const std::string rcss = CssRewriter::rewriteDeclarations(css, nullptr);
        size_t at = 0;
        while (at < rcss.size()){
            const size_t colon = rcss.find(": ", at);
            const size_t end = rcss.find(';', at);
            if (colon == std::string::npos || end == std::string::npos || colon > end)
                break;
            out[rcss.substr(at, colon - at)] = rcss.substr(colon + 2, end - colon - 2);
            at = end + 1;
            while (at < rcss.size() && rcss[at] == ' ')
                at++;
        }
        return out;
    };
    const std::map<std::string, std::string> written = declarations("appearance: auto");
    const std::map<std::string, std::string> cleared = declarations("appearance: none");
    ASSERT_GE(written.size(), 11u);
    // what none clears, auto restores
    for (const auto& pair : cleared)
        EXPECT_TRUE(written.count(pair.first)) << pair.first << " is cleared by none and not restored by auto";
    for (const auto& pair : written)
        EXPECT_TRUE(cleared.count(pair.first)) << pair.first << " is written by auto and not cleared by none";

    // colours as the rewriter writes them
    const auto normalized = [](std::string value){
        if (value.size() == 7 && value[0] == '#')
            value += "ff";
        return value;
    };
    const std::string source(UserAgentStyle::source());
    std::map<std::string, std::set<std::string>> fallbacks;
    for (size_t at = source.find("var(--html-"); at != std::string::npos; at = source.find("var(--html-", at + 1)){
        const size_t comma = source.find(',', at);
        const size_t close = source.find(')', at);
        if (comma == std::string::npos || close < comma)
            continue;   // no fallback
        const std::string name = source.substr(at + 4, comma - at - 4);
        // the fallback runs to the var()'s own closing parenthesis
        int depth = 1;
        bool quote = false;
        size_t i = comma + 1;
        for (; i < source.size(); i++){
            const char c = source[i];
            if (c == '"')
                quote = !quote;
            else if (!quote && c == '(')
                depth++;
            else if (!quote && c == ')' && --depth == 0)
                break;
        }
        std::string fallback = source.substr(comma + 1, i - comma - 1);
        fallback.erase(0, fallback.find_first_not_of(" \n"));
        fallback.erase(fallback.find_last_not_of(" \n") + 1);
        fallbacks[name].insert(normalized(fallback));
    }
    // the controls' reset rule (p4-gaps2: `appearance` is not inherited) declares each companion
    // with the look's value: the same as auto writes
    std::map<std::string, std::string> reset;
    const size_t rule = source.find("input, select, button, textarea, progress {\n    --html-appearance:");
    ASSERT_NE(rule, std::string::npos) << "no reset rule of the appearance companions on the controls";
    const size_t ruleEnd = source.find('}', rule);
    for (size_t at = source.find("--html-", rule); at != std::string::npos && at < ruleEnd; at = source.find("--html-", at + 1)){
        if (at >= 4 && source.compare(at - 4, 4, "var(") == 0)
            continue;
        const size_t colon = source.find(':', at);
        const size_t semicolon = source.find(';', at);
        std::string value = source.substr(colon + 1, semicolon - colon - 1);
        value.erase(0, value.find_first_not_of(' '));
        reset[source.substr(at, colon - at)] = normalized(value);
        at = semicolon;
    }
    for (const auto& pair : written){
        SCOPED_TRACE(pair.first);
        const auto declared = reset.find(pair.first);
        ASSERT_NE(declared, reset.end()) << "the controls' reset rule does not declare it: it would inherit";
        EXPECT_EQ(pair.second, declared->second);
        const auto read = fallbacks.find(pair.first);
        if (read == fallbacks.end()){
            // read by the runtime only (--html-appearance: HasNativeAppearance)
            EXPECT_EQ(pair.first, "--html-appearance");
            continue;
        }
        ASSERT_EQ(read->second.size(), 1u) << "read with different fallbacks: " << *read->second.begin() << " and "
                                           << *read->second.rbegin() << " (auto can write one)";
        EXPECT_EQ(pair.second, *read->second.begin());
    }
    for (const auto& pair : reset)
        EXPECT_TRUE(written.count(pair.first)) << pair.first << " is reset on the controls and not written by appearance";
}
