// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Diagnostics: text and JSON formats, sorting, deduplication, the limit (HTMLUI-GEN-0002),
// severities from the registry, and the "[HTML UI] " Console lines of logDiagnostics,
// captured through the test System.

#include "harness/TestSystem.h"

#include "htmlui/compat/DiagnosticsJson.h"
#include "htmlui/compat/SourceFiles.h"

#include "System.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;
using jasperengine::test::TestSystem;

namespace {

    Diagnostic make(Severity severity, DiagCode code, const char* file, uint32_t line, uint32_t column, const char* message){
        Diagnostic diagnostic;
        diagnostic.severity = severity;
        diagnostic.code = code;
        diagnostic.file = file;
        diagnostic.line = line;
        diagnostic.column = column;
        diagnostic.message = message;
        return diagnostic;
    }

}

TEST(CompatDiagnostics, CodeIds){
    EXPECT_EQ(codes::UnsupportedValue.id(), "HTMLUI-CSS-0004");
    EXPECT_EQ(codes::FileNotFound.id(), "HTMLUI-GEN-0001");
    EXPECT_EQ(codes::DialogNotModal.id(), "HTMLUI-HTML-0025");
    EXPECT_EQ(codes::FontFaceDescriptor.id(), "HTMLUI-AT-0011");
    EXPECT_EQ((DiagCode{DiagArea::SVG, 3}).id(), "HTMLUI-SVG-0003");
}

TEST(CompatDiagnostics, TextFormat){
    Diagnostic diagnostic = make(Severity::Warning, codes::ApproximatedVisible, "ui/menu.css", 12, 5, "'border-style: dashed' is drawn solid");
    diagnostic.suggestion = "use a solid border";
    EXPECT_EQ(toText(diagnostic), "ui/menu.css:12:5: warning: 'border-style: dashed' is drawn solid (fix: use a solid border) [HTMLUI-CSS-0006]");

    diagnostic.suggestion.clear();
    diagnostic.column = 0;
    EXPECT_EQ(toText(diagnostic), "ui/menu.css:12: warning: 'border-style: dashed' is drawn solid [HTMLUI-CSS-0006]");
    diagnostic.line = 0;
    EXPECT_EQ(toText(diagnostic), "ui/menu.css: warning: 'border-style: dashed' is drawn solid [HTMLUI-CSS-0006]");
    diagnostic.file.clear();
    EXPECT_EQ(toText(diagnostic), "warning: 'border-style: dashed' is drawn solid [HTMLUI-CSS-0006]");

    // one line, whatever the message holds
    Diagnostic multiline = make(Severity::Error, codes::LuaScriptError, "ui/a.html", 3, 1, "bad\nthing\r\n here");
    multiline.suggestion = "fix\tit";
    EXPECT_EQ(toText(multiline), "ui/a.html:3:1: error: bad thing here (fix: fix it) [HTMLUI-HTML-0022]");

    Diagnostic info = make(Severity::Info, codes::VendorPrefix, "a.css", 1, 2, "m");
    EXPECT_EQ(toText(info), "a.css:1:2: info: m [HTMLUI-CSS-0009]");
}

TEST(CompatDiagnostics, JsonFormat){
    Diagnostic diagnostic = make(Severity::Warning, codes::UnknownProperty, "ui/menu.css", 4, 3, "'colr' is not a CSS property");
    diagnostic.suggestion = "color";
    diagnostic.replacement = "color";
    diagnostic.subject = "colr";
    diagnostic.length = 4;
    diagnostic.origin = "imported by ui/main.css:2";
    const nlohmann::json json = toJson(diagnostic);
    EXPECT_EQ(json["severity"], "warning");
    EXPECT_EQ(json["code"], "HTMLUI-CSS-0001");
    EXPECT_EQ(json["name"], "unknown-property");
    EXPECT_EQ(json["message"], "'colr' is not a CSS property");
    EXPECT_EQ(json["suggestion"], "color");
    EXPECT_EQ(json["replacement"], "color");
    EXPECT_EQ(json["subject"], "colr");
    EXPECT_EQ(json["file"], "ui/menu.css");
    EXPECT_EQ(json["line"], 4);
    EXPECT_EQ(json["column"], 3);
    EXPECT_EQ(json["length"], 4);
    EXPECT_EQ(json["origin"], "imported by ui/main.css:2");
    EXPECT_EQ(json.size(), 12u);

    const nlohmann::json list = toJson(std::vector<Diagnostic>{diagnostic, diagnostic});
    ASSERT_TRUE(list.is_array());
    EXPECT_EQ(list.size(), 2u);
}

TEST(CompatDiagnostics, SinkLocatesSortsAndDeduplicates){
    SourceFiles files;
    const uint32_t html = files.add("ui/menu.html", "<html>\n<body style=\"colr: red\">");
    const uint32_t css = files.add("ui/menu.css", "a {\n  colr: red;\n}\n", SourceRef{html, 7, 10});
    DiagnosticSink sink(files);

    sink.report(codes::UnknownProperty, SourceRef{css, 6, 4}, "'colr' is not a CSS property", "color", "color", "colr");
    sink.report(codes::UnknownProperty, SourceRef{css, 6, 4}, "'colr' is not a CSS property (again)", "color", "color", "colr");
    sink.report(codes::UnknownProperty, SourceRef{html, 20, 4}, "'colr' is not a CSS property", "color", "color", "colr");
    sink.report(codes::CssSyntaxError, SourceRef{css, 0, 1}, "first");
    Diagnostic located = make(Severity::Error, codes::FileNotFound, "ui/img.png", 0, 0, "missing");
    sink.report(located);
    sink.report(located);   // identical: dropped

    const std::vector<Diagnostic>& list = sink.diagnostics();
    ASSERT_EQ(list.size(), 4u);
    // sorted by file, line, column, code
    EXPECT_EQ(list[0].file, "ui/img.png");
    EXPECT_EQ(list[1].file, "ui/menu.css");
    EXPECT_EQ(list[1].line, 1u);
    EXPECT_EQ(list[1].code, codes::CssSyntaxError);
    EXPECT_EQ(list[2].file, "ui/menu.css");
    EXPECT_EQ(list[2].line, 2u);
    EXPECT_EQ(list[2].column, 3u);
    EXPECT_EQ(list[2].length, 4u);
    EXPECT_EQ(list[2].origin, "linked by ui/menu.html:2");
    EXPECT_EQ(list[2].message, "'colr' is not a CSS property");
    EXPECT_EQ(list[3].file, "ui/menu.html");
    EXPECT_EQ(list[3].line, 2u);
    EXPECT_EQ(list[3].column, 14u);
    EXPECT_EQ(list[3].origin, "");

    EXPECT_EQ(sink.count(Severity::Warning), 3u);
    EXPECT_EQ(sink.count(Severity::Error), 1u);
    EXPECT_FALSE(sink.truncated());

    std::vector<Diagnostic> taken = sink.take();
    EXPECT_EQ(taken.size(), 4u);
    EXPECT_EQ(sink.size(), 0u);
}

TEST(CompatDiagnostics, SeverityComesFromTheRegistry){
    SourceFiles files;
    const uint32_t file = files.add("a.css", "abc");
    DiagnosticSink sink(files);

    // a fixed-severity code keeps the registry's severity
    sink.reportAs(Severity::Info, codes::UnsupportedValue, SourceRef{file, 0, 1}, "value");
    // a "by case" code takes the given one
    sink.reportAs(Severity::Info, codes::UnsupportedProperty, SourceRef{file, 1, 1}, "property");
    sink.reportAs(Severity::Error, codes::ImportProblem, SourceRef{file, 2, 1}, "cycle");
    sink.report(codes::IgnoredProperty, SourceRef{file, 2, 1}, "ignored");

    const std::vector<Diagnostic>& list = sink.diagnostics();
    ASSERT_EQ(list.size(), 4u);
    EXPECT_EQ(list[0].severity, Severity::Warning);
    EXPECT_EQ(list[1].severity, Severity::Info);
    EXPECT_EQ(list[2].code, codes::IgnoredProperty);
    EXPECT_EQ(list[2].severity, Severity::Info);
    EXPECT_EQ(list[3].severity, Severity::Error);
}

TEST(CompatDiagnostics, LimitAddsOneTruncatedDiagnostic){
    SourceFiles files;
    const uint32_t file = files.add("a.css", std::string(100, 'x'));
    DiagnosticSink sink(files, 5);
    for (uint32_t i = 0; i < 20; i++)
        sink.report(codes::UnsupportedValue, SourceRef{file, i, 1}, "value " + std::to_string(i));
    EXPECT_TRUE(sink.truncated());
    const std::vector<Diagnostic>& list = sink.diagnostics();
    ASSERT_EQ(list.size(), 6u);
    size_t truncated = 0;
    for (const Diagnostic& diagnostic : list){
        if (diagnostic.code == codes::DiagnosticsTruncated){
            truncated++;
            EXPECT_EQ(diagnostic.severity, Severity::Info);
            EXPECT_EQ(diagnostic.file, "");
        }
    }
    EXPECT_EQ(truncated, 1u);
    // unlocated diagnostics sort first
    EXPECT_EQ(list[0].code, codes::DiagnosticsTruncated);
}


TEST(CompatDiagnostics, LogLinesCarryThePrefix){
    TestSystem& system = TestSystem::get();
    system.clearLogs();

    std::vector<Diagnostic> list = {
        make(Severity::Error, codes::FileNotFound, "ui/menu.html", 3, 5, "stylesheet 'x.css' not found"),
        make(Severity::Warning, codes::ApproximatedVisible, "ui/menu.css", 12, 5, "'border-style: dashed' is drawn solid"),
        make(Severity::Info, codes::VendorPrefix, "ui/menu.css", 2, 1, "'-webkit-transition' mapped to 'transition'"),
        make(Severity::Info, codes::MediaNormalized, "ui/menu.css", 8, 1, "media query rewritten"),
    };

    logDiagnostics("ui/menu.html", list, false);
    EXPECT_EQ(system.countLogs(D_LOG_ERROR, "[HTML UI] ui/menu.html:3:5: error: stylesheet 'x.css' not found [HTMLUI-GEN-0001]"), 1u);
    EXPECT_EQ(system.countLogs(D_LOG_WARN, "[HTML UI] ui/menu.css:12:5: warning: 'border-style: dashed' is drawn solid [HTMLUI-CSS-0006]"), 1u);
    EXPECT_EQ(system.countLogs(0, "[HTMLUI-CSS-0009]"), 0u);
    EXPECT_EQ(system.countLogs(0, "[HTML UI] ui/menu.html: 1 error, 1 warning, 2 notes"), 1u);

    system.clearLogs();
    logDiagnostics("ui/menu.html", list, true);
    // asked for (Verbose Diagnostics): Info lines like the summary (Log::verbose is Trace since S09,
    // which no sink shows by default)
    EXPECT_EQ(system.countLogs(0, "[HTML UI] ui/menu.css:2:1: info: '-webkit-transition' mapped to 'transition' [HTMLUI-CSS-0009]"), 1u);
    EXPECT_EQ(system.countLogs(0, "[HTMLUI-AT-0005]"), 1u);
    EXPECT_EQ(system.countLogs(D_LOG_VERBOSE, "[HTMLUI-AT-0005]"), 0u);

    system.clearLogs();
    logDiagnostics("ui/empty.html", {}, false);
    EXPECT_EQ(system.countLogs(0, "[HTML UI] ui/empty.html: 0 errors, 0 warnings, 0 notes"), 1u);
    for (const TestSystem::LogEntry& entry : system.getLogs())
        EXPECT_EQ(entry.message.rfind("[HTML UI] ", 0), 0u) << entry.message;
    system.clearLogs();
}
