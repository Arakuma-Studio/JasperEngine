// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SelectorRewriter (phase-2 §3.8, §3.9.5, Appendix B), table-driven: CSS selector list ->
// RCSS selectors per origin and fork-feature set.

#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssRewriter.h"
#include "htmlui/compat/css/Nesting.h"
#include "htmlui/compat/css/SelectorRewriter.h"

#include <gtest/gtest.h>

using namespace jasperengine::htmlui::compat;

namespace {

    // the options of a case: the phase-2 rewrite (no fork feature) unless the case asks for the fork's
    // (every feature is present since phase 3, so the default options cannot tell them apart)
    struct Options {
        SelectorOptions options;
        bool fork = false;
        Options(SelectorOptions o = SelectorOptions(), bool f = false) : options(std::move(o)), fork(f) {}
    };

    std::string run(const char* css, Options caseOptions = Options(), std::vector<Diagnostic>* diagnostics = nullptr){
        SelectorOptions options = caseOptions.options;
        SourceFiles files;
        const uint32_t file = files.add("t.css", css);
        DiagnosticSink sink(files);
        std::string error;
        std::optional<SelectorList> list = SelectorRewriter::parseList(parseComponentValueList(files.text(file)), file, &error);
        if (!list)
            return "parse error";
        if (!caseOptions.fork)
            options.fork = ForkFeatures::none();
        const SelectorRewriteResult result = SelectorRewriter::rewrite(*list, options, &sink, SourceRef{file, 0, 1});
        if (diagnostics)
            *diagnostics = sink.take();
        if (result.invalid)
            return "invalid";
        std::string text;
        for (const RewrittenSelector& s : result.selectors)
            text += (text.empty() ? "" : " | ") + s.text;
        return text;
    }

    SelectorOptions ua(){
        SelectorOptions o;
        o.origin = SelectorOptions::Origin::UserAgent;
        return o;
    }

    Options withFork(){
        SelectorOptions o;
        o.fork = ForkFeatures::all();
        return Options(o, true);
    }

}

TEST(CompatSelector, AuthorTable){
    const std::pair<const char*, const char*> cases[] = {
        {"div", "#htmlui-root div"},
        {".a", "#htmlui-root .a"},
        {"#x", "#htmlui-root #x"},
        {"*", "#htmlui-root * | body#htmlui-root"},
        {"html", "body#htmlui-root"},
        {":root", "body#htmlui-root"},
        {"body", "#htmlui-root htmlbody"},
        {"html body", "body#htmlui-root htmlbody"},
        {"html > body > main", "body#htmlui-root > htmlbody > main"},
        {"template", "#htmlui-root htmltemplate"},
        {"a, b", "#htmlui-root a | #htmlui-root b"},
        {"[data-x=\"1\"] > .y", "#htmlui-root [data-x=\"1\"] > .y"},
        {"a:focus", "#htmlui-root a:html-focus"},
        {"a:focus-visible", "#htmlui-root a:html-focus-visible"},
        {"nav:focus-within", "#htmlui-root nav:focus"},
        {"input:enabled", "#htmlui-root input:not(:disabled)"},
        {"a:link", "#htmlui-root a[href]"},
        {"a:any-link", "#htmlui-root a[href]"},
        {"input:required", "#htmlui-root input[required]"},
        {"input:optional", "#htmlui-root input:not([required])"},
        {"input:read-only", "#htmlui-root input[readonly]"},
        {"input:read-write", "#htmlui-root input:not([readonly])"},
        {"li:nth-child(2n+1)", "#htmlui-root li:nth-child(2n+1)"},
        {"li:nth-child(-n+3)", "#htmlui-root li:nth-child(-n+3)"},
        {"li:nth-last-child(odd)", "#htmlui-root li:nth-last-child(odd)"},
        {"p:not(.a, .b)", "#htmlui-root p:not(.a, .b)"},
        {"p:not(:focus)", "#htmlui-root p:not(:html-focus)"},
        {":is(h1, h2) span", "#htmlui-root h1 span | #htmlui-root h2 span"},
        {":where(.a) .b", "#htmlui-root .a .b"},
        {".card:is(.x, :hover)", "#htmlui-root .card.x | #htmlui-root .card:hover"},
        {"a:visited", ""},
        {"a:has(b)", ""},
        {"p::before", ""},
        {"p:before", ""},
        {"input::placeholder", "#htmlui-root input:placeholder-shown"},
        {"p::selection", "#htmlui-root p selection"},
        {"li::marker", "#htmlui-root li > marker"},
        {"div::-webkit-scrollbar", "#htmlui-root div > scrollbarvertical | #htmlui-root div > scrollbarhorizontal"},
        {"::-webkit-scrollbar", "#htmlui-root scrollbarvertical | #htmlui-root scrollbarhorizontal"},
        {"div::-webkit-scrollbar-thumb:hover", "#htmlui-root div > scrollbarvertical > sliderbar:hover | #htmlui-root div > scrollbarhorizontal > sliderbar:hover"},
        {"input::-webkit-slider-thumb", "#htmlui-root input sliderbar"},
        {"progress::-webkit-progress-value", "#htmlui-root progress fill"},
        {"progress::-webkit-progress-bar", "#htmlui-root progress"},
        {"p::first-line", ""},
        {":lang(fr)", "#htmlui-root [lang|=\"fr\"]"},
        {"x-el:defined", "#htmlui-root x-el"},
        {"[type=a i]", "#htmlui-root [type=\"a\"]"},
        {"svg|rect", ""},
        {"td || col", ""},
        {"p:bogus", "invalid"},
        {"a::-moz-focus-inner", "invalid"},
        {"> a", "invalid"},
        {"a:hover:focus", "#htmlui-root a:hover:html-focus"},
        {"div:root", ""},
        {"#htmlui-root", ""},
    };
    for (const auto& c : cases)
        EXPECT_EQ(run(c.first), c.second) << c.first;
}

TEST(CompatSelector, UserAgentHasNoBand){
    EXPECT_EQ(run("html", ua()), "body");
    EXPECT_EQ(run("body", ua()), "htmlbody");
    EXPECT_EQ(run("button:focus-visible", ua()), "button:html-focus-visible");
    EXPECT_EQ(run("*", ua()), "*");
    EXPECT_EQ(run("details:not([open]) > :not(summary)", ua()), "details:not([open]) > :not(summary)");
}

TEST(CompatSelector, RootAttributes){
    SelectorOptions o;
    o.rootId = "app";
    o.rootClasses = {"dark"};
    o.rootAttributes = {"data-theme"};
    EXPECT_EQ(run("#app .x", o), "body#htmlui-root .x");
    EXPECT_EQ(run(".dark .x", o), "#htmlui-root .dark .x | body#htmlui-root.dark .x");
    EXPECT_EQ(run("[data-theme=dark] .x", o), "#htmlui-root [data-theme=\"dark\"] .x | body#htmlui-root[data-theme=\"dark\"] .x");
    EXPECT_EQ(run(".light .x", o), "#htmlui-root .light .x");
    EXPECT_EQ(run("div .x", o), "#htmlui-root div .x");
}

TEST(CompatSelector, ForkFeatures){
    EXPECT_EQ(run(":is(h1, h2) span", withFork()), "#htmlui-root :is(h1, h2) span");
    EXPECT_EQ(run(":where(.a) .b", withFork()), "#htmlui-root :where(.a) .b");
    EXPECT_EQ(run(":matches(.a, .b)", withFork()), "#htmlui-root :is(.a, .b)");
    EXPECT_EQ(run("p::before", withFork()), "#htmlui-root p::before");
    EXPECT_EQ(run("p:after", withFork()), "#htmlui-root p::after");
    // phase 3, p3-cascade: Has, FocusPseudoClasses, SelectorsLevel4
    EXPECT_EQ(run("li:has(> input:checked)", withFork()), "#htmlui-root li:has(> input:checked)");
    EXPECT_EQ(run(".card:has(+ .badge, body .x)", withFork()), "#htmlui-root .card:has(+ .badge, htmlbody .x)");
    EXPECT_EQ(run("div:has(:has(p))", withFork()), "invalid");
    EXPECT_EQ(run("a:focus", withFork()), "#htmlui-root a:focus");
    EXPECT_EQ(run("a:focus-visible", withFork()), "#htmlui-root a:focus-visible");
    EXPECT_EQ(run("form:focus-within", withFork()), "#htmlui-root form:focus-within");
    EXPECT_EQ(run("input:enabled", withFork()), "#htmlui-root input:enabled");
    EXPECT_EQ(run("a:any-link, a:link", withFork()), "#htmlui-root a:any-link | #htmlui-root a:link");
    EXPECT_EQ(run("input:-moz-read-only", withFork()), "#htmlui-root input:read-only");
    EXPECT_EQ(run("input:required:read-write, :optional", withFork()), "#htmlui-root input:required:read-write | #htmlui-root :optional | body#htmlui-root:optional");
    EXPECT_EQ(run("input:default, :target, :indeterminate", withFork()),
              "#htmlui-root input:default | #htmlui-root :target | body#htmlui-root:target | #htmlui-root :indeterminate | body#htmlui-root:indeterminate");
    EXPECT_EQ(run("[type=a i]", withFork()), "#htmlui-root [type=\"a\" i]");
    EXPECT_EQ(run("li:nth-child(2n+1 of .done, body .x)", withFork()), "#htmlui-root li:nth-child(2n+1 of .done, htmlbody .x)");
    EXPECT_EQ(run("li:nth-child(2 of ::before)", withFork()), "");
    // absent: phase 2's rewrites
    EXPECT_EQ(run("li:has(> input:checked)"), "");
    EXPECT_EQ(run("input:enabled"), "#htmlui-root input:not(:disabled)");
    EXPECT_EQ(run("li:nth-child(2 of .x)"), "");
}

TEST(CompatSelector, SpecificityOfLevel4){
    auto spec = [](const char* css){
        std::optional<SelectorList> list = SelectorRewriter::parseList(css);
        return list ? SelectorRewriter::specificity((*list)[0]) : 0u;
    };
    EXPECT_EQ(spec(":where(#a, .b)"), 0u);
    EXPECT_EQ(spec(":is(#a, .b)"), 1000000u);
    EXPECT_EQ(spec("li:has(> .a, #b)"), 10000u + 1000000u);
    EXPECT_EQ(spec(":nth-child(2 of .a, #b)"), 100000u + 1000000u);
}

TEST(CompatSelector, Specificity){
    std::optional<SelectorList> list = SelectorRewriter::parseList("#a .b c:hover [d] :not(#e, .f)");
    ASSERT_TRUE(list);
    // id 1 000 000, class/attribute/pseudo 100 000 each, tag 10 000, :not() the max of its arguments
    EXPECT_EQ(SelectorRewriter::specificity((*list)[0]), 1000000u + 100000u + 10000u + 100000u + 100000u + 1000000u);
}

TEST(CompatSelector, Diagnostics){
    std::vector<Diagnostic> d;
    run("a:visited", SelectorOptions(), &d);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0].code, codes::VisitedDropped);
    run("p::after", SelectorOptions(), &d);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0].code, codes::PseudoElementPending);
    run(":where(.a)", SelectorOptions(), &d);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0].code, codes::IsWhereExpanded);
    run("p:bogus", SelectorOptions(), &d);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0].code, codes::InvalidSelector);
    run(":dir(rtl)", SelectorOptions(), &d);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0].code, codes::SelectorIgnored);
    EXPECT_EQ(d[0].severity, Severity::Info);
}

TEST(CompatSelector, Nesting){
    auto combine = [](const char* parents, const char* children, ForkFeatures fork = ForkFeatures::none()){
        std::optional<SelectorList> p = SelectorRewriter::parseList(parents);
        std::optional<SelectorList> c = SelectorRewriter::parseList(children);
        std::optional<SelectorList> r = Nesting::combine(*p, *c, fork);
        if (!r)
            return std::string("limit");
        std::string text;
        for (const ComplexSelector& s : *r)
            text += (text.empty() ? "" : " | ") + SelectorRewriter::serialize(s);
        return text;
    };
    EXPECT_EQ(combine(".a", "&:hover"), ".a:hover");
    EXPECT_EQ(combine(".a", ".b"), ".a .b");
    EXPECT_EQ(combine(".a", "> .b"), ".a > .b");
    EXPECT_EQ(combine(".a", "& + &"), ".a + .a");
    EXPECT_EQ(combine(".a .b", "div&"), ".a div.b");
    EXPECT_EQ(combine(".a", ".x &"), ".x .a");
    EXPECT_EQ(combine(".a, .b", ".c, .d"), ".a .c | .a .d | .b .c | .b .d");
    EXPECT_EQ(combine(".a, .b", "&.c", ForkFeatures::all()), ":is(.a, .b).c");
}

TEST(CompatSelector, QuerySelector){
    EXPECT_EQ(CssRewriter::rewriteSelector("body > .menu", nullptr), "htmlbody > .menu");
    EXPECT_EQ(CssRewriter::rewriteSelector("html", nullptr), "body");
    // FocusPseudoClasses present: the fork's :focus; absent, phase 2's :html-focus
    EXPECT_EQ(CssRewriter::rewriteSelector("input:focus", nullptr), "input:focus");
    CompatOptions absent;
    absent.fork = ForkFeatures::present().with(ForkFeature::FocusPseudoClasses, false);
    EXPECT_EQ(CssRewriter::rewriteSelector("input:focus", nullptr, absent), "input:html-focus");
    EXPECT_EQ(CssRewriter::rewriteSelector(".a,  .b", nullptr), ".a, .b");
}
