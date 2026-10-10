// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// MediaQuery (phase-2 §3.9.4), table-driven: CSS query list -> RmlUi @media blocks.

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/MediaQuery.h"

#include <gtest/gtest.h>

using namespace jasperengine::htmlui::compat;

namespace {

    struct Case {
        const char* query;
        const char* blocks;    // "|"-separated RCSS conditions; "all" = unconditional; "never"
        bool touch = false;
    };

    std::string run(const char* query, bool touch, std::vector<Diagnostic>* diagnostics = nullptr){
        SourceFiles files;
        const uint32_t file = files.add("t.css", query);
        DiagnosticSink sink(files);
        CompatOptions options;
        options.touchDevice = touch;
        const MediaResult result = MediaQuery::normalize(query, options, &sink, SourceRef{file, 0, (uint32_t)std::string(query).size()});
        if (diagnostics)
            *diagnostics = sink.take();
        if (result.never)
            return "never";
        std::string text;
        for (const std::string& block : MediaQuery::blockTexts(result))
            text += (text.empty() ? "" : "|") + block;
        return text;
    }

}

TEST(CompatMedia, Table){
    const Case cases[] = {
        {"(min-width: 600px)", "(min-width: 600px)"},
        {"screen", "all"},
        {"all and (max-width: 100px)", "(max-width: 100px)"},
        {"only screen and (orientation: landscape)", "(orientation: landscape)"},
        {"print", "never"},
        {"not print", "all"},
        {"not screen", "never"},
        {"not print and (min-width: 10px)", "all"},
        {"speech, (min-width: 1px)", "(min-width: 1px)"},
        {"(width >= 600px)", "(min-width: 600px)"},
        {"(width > 600px)", "(min-width: 600.02px)"},
        {"(width <= 600px)", "(max-width: 600px)"},
        {"(width < 600px)", "(max-width: 599.98px)"},
        {"(600px <= width)", "(min-width: 600px)"},
        {"(400px <= width <= 800px)", "(min-width: 400px) and (max-width: 800px)"},
        {"(400px < height < 800px)", "(min-height: 400.02px) and (max-height: 799.98px)"},
        {"(width = 300px)", "(width: 300px)"},
        {"(min-width: 40em)", "(min-width: 640px)"},
        {"(max-width: 2rem)", "(max-width: 32px)"},
        {"(min-width: 1in)", "(min-width: 96px)"},
        {"(min-resolution: 192dpi)", "(min-resolution: 2x)"},
        {"(max-resolution: 2dppx)", "(max-resolution: 2x)"},
        {"(min-aspect-ratio: 16/9)", "(min-aspect-ratio: 16/9)"},
        {"(aspect-ratio: 1)", "(aspect-ratio: 1/1)"},
        {"(prefers-color-scheme: dark)", "(theme: dark)"},
        {"(prefers-color-scheme: light)", "(theme: light)"},
        {"(prefers-reduced-motion: reduce)", "(theme: reduced-motion)"},
        {"(prefers-reduced-motion)", "(theme: reduced-motion)"},
        {"(prefers-reduced-motion: no-preference)", "not (theme: reduced-motion)"},
        {"(theme: retro)", "(theme: retro)"},
        {"(hover: hover)", "all"},
        {"(hover: hover)", "never", true},
        {"(hover: none)", "all", true},
        {"(pointer: coarse)", "never"},
        {"(pointer: coarse)", "all", true},
        {"(any-hover)", "all"},
        {"(color)", "all"},
        {"(min-color: 4)", "all"},
        {"(monochrome)", "never"},
        {"(color-gamut: p3)", "never"},
        {"(forced-colors: none) and (inverted-colors: none)", "all"},
        {"(scripting: none)", "all"},
        {"(display-mode: standalone)", "never"},
        {"(width)", "all"},
        {"(min-width: 600px) and (max-width: 500px)", "never"},
        {"(min-width: 600px) and (min-width: 700px)", "(min-width: 700px)"},
        {"not (min-width: 600px)", "(max-width: 599.98px)"},
        {"not (orientation: portrait)", "(orientation: landscape)"},
        {"not ((min-width: 600px) and (orientation: landscape))", "(max-width: 599.98px)|(orientation: portrait)"},
        {"(min-width: 600px) or (orientation: portrait)", "(min-width: 600px)|(orientation: portrait)"},
        {"((min-width: 1px) or (min-height: 1px)) and (orientation: landscape)",
         "(min-width: 1px) and (orientation: landscape)|(min-height: 1px) and (orientation: landscape)"},
        {"not (prefers-color-scheme: dark)", "not (theme: dark)"},
        {"(prefers-color-scheme: dark) and (prefers-reduced-motion: reduce)", "never"},
        {"(prefers-reduced-motion: no-preference) and (min-width: 1px)", "never"},
        {"(unknown: 1)", "never"},
        {"(device-width: 100px)", "never"},
        {"(grid)", "never"},
        {"(min-width: 600px) and", "never"},
        {"(min-width: 600px) and (max-width: 900px) or (orientation: portrait)", "never"},
        {"", "never"},
    };
    for (const Case& c : cases)
        EXPECT_EQ(run(c.query, c.touch), c.blocks) << c.query << (c.touch ? " (touch)" : "");
}

TEST(CompatMedia, Diagnostics){
    std::vector<Diagnostic> diagnostics;
    run("(min-width: 600px)", false, &diagnostics);
    EXPECT_TRUE(diagnostics.empty());

    run("screen and (min-width: 40em)", false, &diagnostics);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, codes::MediaNormalized);

    run("(hover: hover)", false, &diagnostics);
    bool staticReported = false;
    for (const Diagnostic& d : diagnostics)
        staticReported = staticReported || d.code == codes::MediaStatic;
    EXPECT_TRUE(staticReported);

    run("(unknown: 1)", false, &diagnostics);
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_EQ(diagnostics[0].code, codes::MediaFeatureUnsupported);
    EXPECT_EQ(diagnostics[0].severity, Severity::Warning);
}

TEST(CompatMedia, Conjoin){
    CompatOptions options;
    const MediaResult a = MediaQuery::normalize("(min-width: 600px), (orientation: portrait)", options);
    const MediaResult b = MediaQuery::normalize("(max-width: 900px)", options);
    const MediaResult both = MediaResult::conjoin(a, b);
    const std::vector<std::string> texts = MediaQuery::blockTexts(both);
    ASSERT_EQ(texts.size(), 2u);
    EXPECT_EQ(texts[0], "(min-width: 600px) and (max-width: 900px)");
    EXPECT_EQ(texts[1], "(max-width: 900px) and (orientation: portrait)");
    EXPECT_TRUE(MediaResult::conjoin(MediaResult::unconditional(), b).blocks.size() == 1);
    EXPECT_TRUE(MediaResult::conjoin(MediaQuery::normalize("print", options), b).never);
}
