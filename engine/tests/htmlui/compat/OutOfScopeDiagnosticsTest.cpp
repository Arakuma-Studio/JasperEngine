// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Phase-3 spec 2.2 and 6 ("Compat layer"): every CSS feature phase 3 leaves out is reported by
// the compat diagnostics with the file, line and column of the declaration and the hint the spec
// lists, with every fork feature present (the shipped state).

#include "CompatTestCommon.h"
#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/CssRewriter.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>

using namespace jasperengine::htmlui::compat;

namespace {

    std::vector<Diagnostic> diagnosticsOf(const std::string& sheet){
        SourceFiles files;
        DiagnosticSink sink(files);
        CompatOptions options;
        options.fork = ForkFeatures::present();
        const uint32_t file = files.add("ui/sample.css", sheet);
        CssRewriter rewriter(options, files, sink);
        StylesheetInput input;
        input.kind = StylesheetInput::Kind::Inline;
        input.file = file;
        input.text = files.text(file);
        rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
        return sink.diagnostics();
    }

}

// One declaration, rule or at-rule per spec 2.2 row (several for a row naming several features): the
// diagnostic's code and a piece of its hint. Rows phase 2 already handles better than "diagnosed",
// as the spec keeps it, are below.
TEST(OutOfScopeDiagnostics, EveryItemIsReportedWithItsHint){
    struct Item {
        const char* sheet;
        const char* code;
        const char* hint;
    };
    const Item items[] = {
        // writing-mode, vertical text, direction: rtl layout mirroring
        {".a {\n  writing-mode: vertical-rl;\n}", "HTMLUI-CSS-0002", "rotate the element"},
        {".a {\n  direction: rtl;\n}", "HTMLUI-CSS-0005", "Mirrored layout is not supported"},
        // subgrid, masonry
        {".a {\n  grid-template-columns: subgrid;\n}", "HTMLUI-CSS-0004", "Use nested grids with explicit track sizes."},
        {".a {\n  grid-template-rows: masonry;\n}", "HTMLUI-CSS-0004", "Use nested grids with explicit track sizes."},
        // @container, container-type
        {".a {\n  container-type: inline-size;\n}", "HTMLUI-CSS-0002", "@media"},
        {"@container (min-width: 10px) {\n  .a { color: red; }\n}", "HTMLUI-AT-0001", "@media"},
        // ::first-line, ::first-letter
        {".a::first-line {\n  color: red;\n}", "HTMLUI-SEL-0001", "span"},
        {".a::first-letter {\n  color: red;\n}", "HTMLUI-SEL-0001", "span"},
        // line-clamp and its longhands: Chromium 153 implements only -webkit-line-clamp (native since p6-line-clamp)
        {".a {\n  line-clamp: 2;\n}", "HTMLUI-CSS-0002", "-webkit-line-clamp"},
        {".a {\n  max-lines: 2;\n}", "HTMLUI-CSS-0002", "-webkit-line-clamp"},
        {".a {\n  block-ellipsis: auto;\n}", "HTMLUI-CSS-0002", "-webkit-line-clamp"},
        {".a {\n  continue: discard;\n}", "HTMLUI-CSS-0002", "-webkit-line-clamp"},
        // static text selection
        {".a {\n  user-select: text;\n}", "HTMLUI-CSS-0005", "text inputs"},
        {".a {\n  user-select: all;\n}", "HTMLUI-CSS-0005", "text inputs"},
        // scroll-snap-*, scroll-behavior: smooth
        {".a {\n  scroll-snap-type: x mandatory;\n}", "HTMLUI-CSS-0002", "Lua"},
        {".a {\n  scroll-snap-align: start;\n}", "HTMLUI-CSS-0002", "Lua"},
        {".a {\n  scroll-behavior: smooth;\n}", "HTMLUI-CSS-0003", "Use focus navigation or Lua scrolling"},
        // multi-column layout
        {".a {\n  columns: 2;\n}", "HTMLUI-CSS-0002", "flex columns"},
        {".a {\n  column-count: 3;\n}", "HTMLUI-CSS-0002", "flex columns"},
        // counter styles beyond the list, @counter-style
        {".a {\n  list-style-type: hiragana;\n}", "HTMLUI-CSS-0005", "use one of: decimal"},
        {"@counter-style thumbs {\n  system: cyclic;\n  symbols: x;\n}", "HTMLUI-AT-0001", "use one of: decimal"},
        // wavy, text-emphasis, hyphens: auto (text-indent with hanging and each-line is native: P6-J2)
        {".a {\n  text-decoration-style: wavy;\n}", "HTMLUI-CSS-0005", "solid"},
        {".a {\n  text-emphasis: dot;\n}", "HTMLUI-CSS-0002", "span"},
        {".a {\n  hyphens: auto;\n}", "HTMLUI-CSS-0003", "soft hyphens"},
        // text-align: justify is native (p6-justify); justify-all is dropped, as Chromium does not parse it
        {".a {\n  text-align: justify-all;\n}", "HTMLUI-CSS-0004", "text-align-last: justify"},
        // mix-blend-mode, background-blend-mode
        {".a {\n  mix-blend-mode: multiply;\n}", "HTMLUI-CSS-0002", "opacity"},
        {".a {\n  background-blend-mode: screen;\n}", "HTMLUI-CSS-0002", "pre-blend"},
        // tables
        {".a {\n  display: ruby;\n}", "HTMLUI-CSS-0005", ""},
        {".a {\n  border-collapse: collapse;\n}", "HTMLUI-CSS-0006", "borders on one side"},
        {".a {\n  caption-side: bottom;\n}", "HTMLUI-CSS-0002", "caption"},
        {".a {\n  table-layout: auto;\n}", "HTMLUI-CSS-0003", ""},
        // font-variant*, font-stretch, font-feature-settings, font-variation-settings
        {".a {\n  font-variant: small-caps;\n}", "HTMLUI-CSS-0002", "font"},
        {".a {\n  font-variant-numeric: tabular-nums;\n}", "HTMLUI-CSS-0002", "font"},
        {".a {\n  font-stretch: condensed;\n}", "HTMLUI-CSS-0002", "@font-face"},
        {".a {\n  font-feature-settings: \"liga\" 0;\n}", "HTMLUI-CSS-0002", "font"},
        {".a {\n  font-variation-settings: \"wght\" 400;\n}", "HTMLUI-CSS-0002", "font"},
        // cursor: url()
        {".a {\n  cursor: url(x.png), pointer;\n}", "HTMLUI-CSS-0005", ""},
        // background-clip: text (phase 2's gradient text: the text in a solid colour of the gradient)
        {".a {\n  background: linear-gradient(red, blue);\n  background-clip: text;\n  color: transparent;\n}", "HTMLUI-CSS-0006", "solid"},
    };
    for (const Item& item : items){
        SCOPED_TRACE(item.sheet);
        const std::vector<Diagnostic> diagnostics = diagnosticsOf(item.sheet);
        const auto found = std::find_if(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& d){ return d.code.id() == item.code; });
        ASSERT_NE(found, diagnostics.end()) << item.code << " expected; got " << diagnostics.size() << " diagnostics"
                                            << (diagnostics.empty() ? std::string() : ": " + toText(diagnostics[0]));
        // file:line:column of the declaration (the at-rule or selector)
        EXPECT_EQ(found->file, "ui/sample.css");
        EXPECT_GE(found->line, 1u);
        EXPECT_GE(found->column, 1u);
        EXPECT_NE(found->suggestion.find(item.hint), std::string::npos) << toText(*found);
    }
}

// The rows of spec 2.2 that phase 2 handles better than "diagnosed" (kept by the spec): mapped to
// RmlUi's own widget elements (::selection in inputs, ::placeholder, ::-webkit-scrollbar), an exact
// rewrite (overscroll-behavior: none is contain: no overscroll effects exist), and @layer flattened
// in order (reported only where the order changes a result: HTMLUI-AT-0002). Fractional font sizes
// are in the guide's limits list (a diagnostic per computed 13.333px would be noise).
TEST(OutOfScopeDiagnostics, PhaseTwoHandlesTheRest){
    const char* sheets[] = {
        "input::selection {\n  color: red;\n}",
        "input::placeholder {\n  color: red;\n}",
        ".a::-webkit-scrollbar {\n  width: 4px;\n}",
        ".a {\n  overscroll-behavior: none;\n}",
        "@layer base {\n  .a { color: red; }\n}",
    };
    for (const char* sheet : sheets){
        SCOPED_TRACE(sheet);
        for (const Diagnostic& d : diagnosticsOf(sheet))
            EXPECT_EQ(d.severity, Severity::Info) << toText(d);
    }
}

// S51 R15 (check --ui): a rule that sets a bitmap face (@font-face of a .bitmapfont or .fnt) and a
// glow, a blur or a blurred text-shadow reports HTMLUI-CSS-0024; a sharp shadow, whose colour may
// be a function with commas, does not
TEST(OutOfScopeDiagnostics, BitmapFontBlurIsReported){
    const std::string face = "@font-face {\n  font-family: Hud;\n  src: url(fonts/hud.bitmapfont);\n}\n";
    struct Item { const char* rule; bool reported; };
    const Item items[] = {
        {".a {\n  font-family: Hud;\n  font-effect: glow(1px 2px 0px 0px #00ff00);\n}", true},
        {".a {\n  font-family: Hud;\n  text-shadow: 0 0 4px #00ff00;\n}", true},
        {".a {\n  font-family: Hud;\n  text-shadow: 1px 1px 0 #000, 2px 2px 3px rgba(0, 0, 0, 0.5);\n}", true},
        {".a {\n  font-family: Hud;\n  text-shadow: rgba(0, 0, 0, 1) 1px 1px;\n}", false},
        {".a {\n  font-family: Hud;\n  text-shadow: 2px 2px 0px rgb(0, 0, 255);\n}", false},
        {".a {\n  font-family: Hud;\n  font-effect: outline(1px #ff0000);\n}", false},
        {".a {\n  font-family: Other;\n  text-shadow: 0 0 4px #00ff00;\n}", false},
    };
    for (const Item& item : items){
        SCOPED_TRACE(item.rule);
        const std::vector<Diagnostic> diagnostics = diagnosticsOf(face + item.rule);
        const bool found = std::any_of(diagnostics.begin(), diagnostics.end(),
                                       [](const Diagnostic& d){ return d.code.id() == "HTMLUI-CSS-0024"; });
        EXPECT_EQ(found, item.reported);
    }
}
