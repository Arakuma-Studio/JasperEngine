// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The declaration memo of the CSS rewriter (phase-2 §4 "p2-perf as implemented"): a memo hit
// writes exactly what the pipeline would write, and reports exactly what it would report.
// Every CSS case (in the three fork states), the user-agent sheet, a sheet that repeats its
// declarations and every HTML fixture and golden page are rewritten without the memo, with
// it, and with its Check mode, which runs the pipeline for every hit as well: a rewrite that
// reads its rule, its block or the custom properties without counting in Session::memoTaint
// shows as a mismatch here.

#include "CssCaseRunner.h"

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/ua/UserAgentStyle.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <regex>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test::compat;

namespace {

    using Memo = CssRewriter::DeclarationMemo;

    struct MemoMode {
        explicit MemoMode(Memo mode): previous(CssRewriter::declarationMemo()) { CssRewriter::setDeclarationMemo(mode); }
        ~MemoMode(){ CssRewriter::setDeclarationMemo(previous); }
        Memo previous;
    };

    // every field of every diagnostic, in the order they are given
    std::string diagnosticsText(const std::vector<Diagnostic>& diagnostics){
        std::string text;
        for (const Diagnostic& d : diagnostics){
            text += toText(d) + "|" + std::to_string((int)d.severity) + "|" + d.code.id() + "|" + d.message + "|" + d.suggestion + "|" +
                    d.replacement + "|" + d.subject + "|" + d.file + ":" + std::to_string(d.line) + ":" + std::to_string(d.column) + "+" +
                    std::to_string(d.length) + "|" + d.origin + "\n";
        }
        return text;
    }

    std::string sheetText(const StylesheetOutput& sheet){
        std::string text = sheet.rcss + "\n--lines\n";
        for (const auto& line : sheet.lines.lines)
            text += std::to_string(line.offset) + ":" + std::to_string(line.length) + " ";
        return text + "\n";
    }

    std::string listText(const std::vector<std::string>& list){
        std::string text;
        for (const std::string& item : list)
            text += item + "\n";
        return text;
    }

    std::string resultText(const RewriteResult& result, const std::vector<Diagnostic>& diagnostics){
        std::string text;
        for (const StylesheetOutput& sheet : result.sheets)
            text += sheetText(sheet);
        text += "--important\n" + sheetText(result.importantSheet);
        text += "--dependencies\n" + listText(result.dependencies);
        text += "--fonts\n" + listText(result.fontFamilies) + listText(result.declaredFontFamilies);
        text += "--fill\n" + listText(result.animationFillNames);
        text += "--remote " + std::to_string(result.remoteRefs.size()) + " virtual " + std::to_string(result.virtualFiles.size()) +
                " viewport " + std::to_string(result.dependsOnViewport) + "\n";
        return text + "--diagnostics\n" + diagnosticsText(diagnostics);
    }

    std::string rewriteSheet(const std::string& css, bool userAgent, const ForkFeatures& fork){
        SourceFiles files;
        const uint32_t file = files.add(userAgent ? "useragent.css" : "ui/memo.css", css);
        DiagnosticSink sink(files);
        CompatOptions options;
        options.fork = fork;
        RewriteContext context;
        context.files.read = [](const std::string&, std::string&){ return false; };
        context.files.exists = [](const std::string&){ return true; };
        CssRewriter rewriter(options, files, sink, userAgent ? RewriteContext() : context);
        StylesheetInput input;
        if (userAgent)
            input.kind = StylesheetInput::Kind::UserAgent;
        input.file = file;
        input.text = files.text(file);
        const RewriteResult result = rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
        return resultText(result, sink.diagnostics());
    }

    // rules that repeat their declarations, with what the memo must not keep among them:
    // declarations that report, that read their rule (shorthand families, currentColor, em),
    // custom properties, URLs, vendor prefixes, !important
    std::string repetitiveSheet(){
        std::string css = ":root { --accent: rgba(0, 200, 255, .8); --pad: 12px; --dur: 200ms; }\n"
                          "@keyframes fadeIn { from { opacity: 0; } to { opacity: 1; } }\n";
        const char* blocks[] = {
            "padding: 12px; border-radius: 8px;", "color: #fff;", "color: #fff; border: 1px solid;", "color: red; border: 1px solid;",
            "font: 700 18px/1.2 sans-serif;", "font-size: 2em; margin: 1.5em;", "font-size: 10px; margin: 1.5em;",
            "width: 24px; height: 24px;", "background: rgba(255,255,255,.08);", "background: url(a.png) no-repeat; background-size: cover;",
            "background-size: cover;", "transition: opacity .3s ease;", "transition: opacity .3s ease; transition-delay: 1s;",
            "position: fixed; top: 16px;", "display: flex; gap: 8px;", "box-shadow: 0 4px 12px rgba(0,0,0,.4);", "box-shadow: 0 4px 12px;",
            "text-shadow: 0 0 10px var(--accent);", "padding: var(--pad);", "animation: fadeIn .4s ease;",
            "animation: fadeIn .4s ease; animation-delay: var(--dur);", "-webkit-user-select: none;", "-webkit-user-select: none; user-select: text;",
            "color: #fff !important;", "margin: 0 !important; margin: 4px;", "border-style: dashed;", "border-style: dashed; border-width: 2px;",
            "float: left;", "width: 50vmin;", "outline: 2px solid;", "text-decoration: underline; color: blue;", "inset: 0; position: absolute;",
            "font-family: 'Press Start 2P', monospace;", "transform: rotate(10deg); scale: 2;", "transform: rotate(10deg);",
            "aspect-ratio: 16 / 9;", "color: currentColor; fill: currentColor;", "line-height: 1.5; font-size: 20px;",
        };
        const char* selectors[] = {".a-%d", "img.b-%d", "#c-%d > .x", ".d-%d:hover", "ul .e-%d::before"};
        for (int round = 0; round < 4; round++){
            int index = 0;
            for (const char* block : blocks){
                char selector[64];
                std::snprintf(selector, sizeof(selector), selectors[(index + round) % 5], round * 100 + index);
                css += std::string(selector) + " { " + block + " }\n";
                if (index % 7 == round)
                    css += "@media (max-width: 768px) { " + std::string(selector) + " { " + block + " } }\n";
                index++;
            }
        }
        return css;
    }

    std::string fixturePath(const std::string& path){
        return cssCaseRoot() + "/" + path;
    }

    FileAccess fixtureAccess(){
        FileAccess access;
        access.read = [](const std::string& path, std::string& content){ return readCaseFile(cssCaseRoot(), path, content); };
        access.exists = [](const std::string& path){
            std::error_code error;
            return std::filesystem::is_regular_file(fixturePath(path), error);
        };
        return access;
    }

    std::string documentText(const std::string& path, const ForkFeatures& fork){
        CompatOptions options;
        options.fork = fork;
        const std::shared_ptr<const CompatDocument> document = DocumentLoader::compile(path, options, fixtureAccess());
        std::string text = document->rml + "\n";
        for (const StylesheetOutput& sheet : document->stylesheets)
            text += sheetText(sheet);
        text += "--hints\n" + sheetText(document->hints) + "--important\n" + sheetText(document->important);
        for (const auto& attribute : document->styleAttributes)
            text += std::to_string(attribute.first) + ": " + attribute.second + "\n";
        text += "--fonts\n" + listText(document->fontFamilies) + listText(document->declaredFontFamilies) + listText(document->animationFillNames);
        text += "--dependencies\n" + listText(document->dependencies);
        text += "--diagnostics\n" + diagnosticsText(document->diagnostics);
        // the per-compile virtual directory is a counter
        static const std::regex key("@htmlui/doc/[0-9a-f]{16}/");
        return std::regex_replace(text, key, "@htmlui/doc/KEY/");
    }

    // make() without the memo, with it and with its Check mode
    template<typename Make>
    void expectSame(const std::string& what, const Make& make){
        std::string off, on, check;
        {
            MemoMode mode(Memo::Off);
            off = make();
        }
        {
            MemoMode mode(Memo::On);
            on = make();
        }
        {
            MemoMode mode(Memo::Check);
            check = make();
        }
        EXPECT_FALSE(off.empty()) << what;
        EXPECT_EQ(off, on) << what << ": the memo changes the output";
        EXPECT_EQ(off, check) << what << ": the memo's check mode changes the output";
    }

    std::vector<std::string> htmlFiles(const std::string& directory){
        std::vector<std::string> paths;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(fixturePath(directory), error)){
            if (entry.is_regular_file() && entry.path().extension() == ".html")
                paths.push_back(directory + "/" + entry.path().filename().string());
        }
        std::sort(paths.begin(), paths.end());
        return paths;
    }

    const ForkFeatures kForks[] = {ForkFeatures::none(), ForkFeatures::present(), ForkFeatures::all()};

}

TEST(CompatCssMemo, CssCasesAreWrittenAndReportedTheSame){
    const uint64_t mismatches = CssRewriter::declarationMemoMismatches();
    const std::string root = cssCaseRoot();
    const std::vector<std::string> names = listCssCases(root);
    ASSERT_GT(names.size(), 50u);
    for (const ForkFeatures& fork : kForks){
        for (const std::string& name : names){
            expectSame("compat/css/" + name, [&](){
                const CssCaseResult result = runCssCase(root, name, fork);
                return result.rcss + "\n" + result.diagnostics + "\n" + resultText(result.result, result.list);
            });
        }
    }
    EXPECT_EQ(CssRewriter::declarationMemoMismatches(), mismatches);
}

TEST(CompatCssMemo, UserAgentAndRepetitiveSheets){
    const uint64_t mismatches = CssRewriter::declarationMemoMismatches();
    const uint64_t hits = CssRewriter::declarationMemoHits();
    const std::string userAgent(UserAgentStyle::source());
    ASSERT_GT(userAgent.size(), 10000u);
    const std::string repetitive = repetitiveSheet();
    for (const ForkFeatures& fork : kForks){
        expectSame("the user-agent sheet", [&](){ return rewriteSheet(userAgent, true, fork); });
        expectSame("the repetitive sheet", [&](){ return rewriteSheet(repetitive, false, fork); });
    }
    EXPECT_EQ(CssRewriter::declarationMemoMismatches(), mismatches);
    // (the check ran: the sheets do repeat declarations the memo keeps)
    EXPECT_GT(CssRewriter::declarationMemoHits(), hits + 100);
}

TEST(CompatCssMemo, PagesAndHtmlFixtures){
    const uint64_t mismatches = CssRewriter::declarationMemoMismatches();
    std::vector<std::string> paths = htmlFiles("compat/pages");
    ASSERT_GE(paths.size(), 8u);
    const std::vector<std::string> fixtures = htmlFiles("compat/html");
    ASSERT_GT(fixtures.size(), 10u);
    paths.insert(paths.end(), fixtures.begin(), fixtures.end());
    for (const ForkFeatures& fork : kForks){
        for (const std::string& path : paths)
            expectSame(path, [&](){ return documentText(path, fork); });
    }
    EXPECT_EQ(CssRewriter::declarationMemoMismatches(), mismatches);
}
