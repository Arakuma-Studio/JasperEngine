// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/DiagnosticCodes.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/CssRewriter.h"

#include <gtest/gtest.h>

using namespace jasperengine::htmlui::compat;

TEST(WebFontCompat, KeepsWoffSourcesInCssOrderWithoutUnsupportedFontDiagnostics){
    for (const std::string format : {"woff", "woff2", "woff-variations", "woff2-variations"}){
        SCOPED_TRACE(format);
        SourceFiles files;
        const std::string css = "@font-face{font-family:Web;src:url(fonts/web." + format + ") format('" + format +
                                "'),url(fonts/fallback.ttf) format('truetype');}";
        const uint32_t file = files.add("ui/web.css", css);
        DiagnosticSink sink(files);
        CompatOptions options;
        RewriteContext context;
        context.files.exists = [](const std::string&){ return true; };
        CssRewriter rewriter(options, files, sink, context);
        StylesheetInput input;
        input.file = file;
        input.text = files.text(file);
        const auto result = rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
        ASSERT_EQ(result.sheets.size(), 1u);
        EXPECT_NE(result.sheets[0].rcss.find("ui/fonts/web." + format), std::string::npos);
        EXPECT_EQ(result.sheets[0].rcss.find("fallback.ttf"), std::string::npos);
        for (const Diagnostic& diagnostic : sink.diagnostics())
            EXPECT_NE(diagnostic.code, codes::FontSource) << diagnostic.message;
    }
}

TEST(WebFontCompat, SupportsQueriesAdvertiseBothWebFontFormats){
    SourceFiles files;
    const std::string css = "@supports font-format(woff){.woff{color:red}}@supports font-format(woff2){.woff2{color:blue}}";
    const uint32_t file = files.add("ui/formats.css", css);
    DiagnosticSink sink(files);
    CompatOptions options;
    CssRewriter rewriter(options, files, sink);
    StylesheetInput input;
    input.file = file;
    input.text = files.text(file);
    const auto result = rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
    ASSERT_EQ(result.sheets.size(), 1u);
    EXPECT_NE(result.sheets[0].rcss.find(".woff {"), std::string::npos);
    EXPECT_NE(result.sheets[0].rcss.find(".woff2 {"), std::string::npos);
}
