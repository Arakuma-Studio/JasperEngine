// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// RmlLogTranslator (phase-2 §3.12.5, hook H3): the `unset` sentinel is swallowed; messages
// RmlUi writes about a virtual sheet, an inline <style> and the RML itself map back to the
// user's file through the line maps (real messages, produced by loading broken RCSS); checks
// capture messages; everything else reaches RmlSystem's own logging.

#include "harness/RecordingRenderInterface.h"
#include "harness/TestSystem.h"

#include "htmlui/compat/RmlLogTranslator.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/compat/css/RcssWriter.h"

#include "HtmlUI.h"
#include "RmlSystem.h"
#include "System.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test;

namespace {

    struct RmlUiSession {
        bool ok;
        RmlUiSession(): ok(HtmlUI::acquire()) {
            HtmlUI::getSystemInterface().resetCounters();
            TestSystem::get().clearLogs();
        }
        ~RmlUiSession(){
            Rml::Factory::ClearStyleSheetCache();
            if (ok)
                HtmlUI::release();
        }
    };

}

TEST(CompatLogTranslator, FindsTheLocationsRmlUiWrites){
    struct Case { const char* message; const char* url; uint32_t line; };
    const Case cases[] = {
        {"Syntax error parsing property declaration 'width: bogus;' in @htmlui/doc/0a/ui/menu.css.rcss: 3.", "@htmlui/doc/0a/ui/menu.css.rcss", 3},
        {"Syntax error parsing property declaration 'width: bogus;' in /@htmlui/doc/0a/ui/menu.css.rcss: 3.", "@htmlui/doc/0a/ui/menu.css.rcss", 3},
        {"Invalid property declaration 'a':'b' at ui/menu.html:40", "ui/menu.html", 40},
        {"Invalid selector 'a[' encountered while parsing stylesheet at @htmlui/ua.rcss:12.", "@htmlui/ua.rcss", 12},
        {"Invalid keyframes identifier 'x y' at ui|menu.css:7", "ui:menu.css", 7},
        {"XML parse error on line 9 of ui/menu.html.", "ui/menu.html", 9},
        {"Found unexpected XML close tag on line 2 of ui/menu.html", "ui/menu.html", 2},
        {"Closing tag 'div' mismatched on ui/menu.html:5 was expecting 'p'.", "ui/menu.html", 5},
        {"Spritesheet 'x' has no sprites defined, ignored. At a.rcss:4", "a.rcss", 4},
    };
    for (const Case& c : cases){
        const std::optional<RmlLogTranslator::MessageLocation> location = RmlLogTranslator::findLocation(c.message);
        ASSERT_TRUE(location.has_value()) << c.message;
        EXPECT_EQ(location->url, c.url) << c.message;
        EXPECT_EQ(location->line, c.line) << c.message;
    }
    EXPECT_FALSE(RmlLogTranslator::findLocation("Syntax error parsing property declaration 'a: b;' in : 0.").has_value());
    EXPECT_FALSE(RmlLogTranslator::findLocation("Failed to load style sheet x.rcss.").has_value());
    EXPECT_FALSE(RmlLogTranslator::findLocation("Invalid substitution, variable '--x' not defined. In element: div").has_value());
}

TEST(CompatLogTranslator, TranslateUsesTheNarrowestMapping){
    SourceFiles files;
    const uint32_t html = files.add("ui/page.html", "<html>\n<style>\n  a { colr: red }\n</style>\n<div>");
    const std::vector<SourceRef> tenLines(10, SourceRef{html, 0, 6});
    SourceRefList rmlLines(tenLines.begin(), tenLines.end());
    SourceRefList styleLines = {SourceRef{html, 21, 9}};
    const RmlLogTranslator::LineMapping mappings[] = {
        {"ui/page.html", 1, &rmlLines},
        {"ui/page.html", 4, &styleLines},
    };
    std::optional<Diagnostic> mapped = RmlLogTranslator::translate(
        "Syntax error parsing property declaration 'colr: red;' in ui/page.html: 4.", files, Span<const RmlLogTranslator::LineMapping>(mappings));
    ASSERT_TRUE(mapped.has_value());
    EXPECT_EQ(mapped->code, codes::RmluiMessage);
    EXPECT_EQ(mapped->severity, Severity::Warning);
    EXPECT_EQ(mapped->file, "ui/page.html");
    EXPECT_EQ(mapped->line, 3u);
    EXPECT_EQ(mapped->column, 7u);
    EXPECT_EQ(mapped->subject, "colr");
    EXPECT_EQ(mapped->message, "RmlUi: Syntax error parsing property declaration 'colr: red;'");
    EXPECT_EQ(toText(*mapped), "ui/page.html:3:7: warning: RmlUi: Syntax error parsing property declaration 'colr: red;' [HTMLUI-GEN-0006]");

    // a line outside every mapping, an unknown url
    EXPECT_FALSE(RmlLogTranslator::translate("x at ui/page.html:40", files, Span<const RmlLogTranslator::LineMapping>(mappings)).has_value());
    EXPECT_FALSE(RmlLogTranslator::translate("x at ui/other.html:2", files, Span<const RmlLogTranslator::LineMapping>(mappings)).has_value());
    // the RML mapping covers line 2
    mapped = RmlLogTranslator::translate("Found unexpected XML close tag on line 2 of ui/page.html", files, Span<const RmlLogTranslator::LineMapping>(mappings));
    ASSERT_TRUE(mapped.has_value());
    EXPECT_EQ(mapped->line, 1u);
}

TEST(CompatLogTranslator, SentinelIsSwallowed){
    RmlUiSession session;
    ASSERT_TRUE(session.ok);
    EXPECT_TRUE(RmlLogTranslator::isUnsetSentinel("Invalid substitution, variable '--html-unset' not defined. In element: div"));
    EXPECT_FALSE(RmlLogTranslator::isUnsetSentinel("Invalid substitution, variable '--x' not defined. In element: div"));

    RecordingRenderInterface recording;
    Rml::Context* context = Rml::CreateContext("compat-sentinel", Rml::Vector2i(200, 100), &recording);
    ASSERT_NE(context, nullptr);
    // what the rewriter writes for an inherited property's `inherit`/`unset`, and an undefined variable for comparison
    Rml::ElementDocument* document = context->LoadDocumentFromMemory(
        "<rml><head><style>body { display: block; color: #ff0000ff; } div { display: block; color: var(--html-unset); }"
        " p { display: block; width: var(--not-declared); }</style></head><body><div id=\"d\">x</div><p/></body></rml>",
        "[compat-sentinel]");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();

    // inherit semantics: the parent's computed color (the reason for the sentinel)
    Rml::Element* div = document->GetElementById("d");
    ASSERT_NE(div, nullptr);
    EXPECT_EQ(div->GetComputedValues().color(), Rml::Colourb(255, 0, 0, 255));

    RmlSystem& system = HtmlUI::getSystemInterface();
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "--html-unset"), 0u);
    // the real undefined variable still reaches the log
    EXPECT_GE(TestSystem::get().countLogs(D_LOG_ERROR, "'--not-declared' not defined"), 1u);
    EXPECT_GE(system.getErrorCount(), 1u);

    Rml::RemoveContext("compat-sentinel");
}

TEST(CompatLogTranslator, RealRmlUiMessagesMapToTheSource){
    RmlUiSession session;
    ASSERT_TRUE(session.ok);

    // the user's files
    auto files = std::make_shared<SourceFiles>();
    const std::string css = "body {\n  display: block;\n  width: bogus;\n}\n";
    const uint32_t cssFile = files->add("ui/broken.css", css);
    const std::string html = "<html>\n<head>\n<style>\n  p { height: nonsense }\n</style>\n</head>\n</div>\n</html>\n";
    const uint32_t htmlFile = files->add("ui/broken.html", html);

    // the compat output: a virtual sheet with a line map, an inline style, the RML
    RcssWriter writer;
    const SourceRef bodyRule{cssFile, 0, 4};
    writer.beginRule("body", bodyRule);
    writer.declaration("display", "block", SourceRef{cssFile, 9, 14});
    writer.declaration("width", "bogus", SourceRef{cssFile, 27, 12});
    writer.endRule();
    auto sheetLines = std::make_shared<RcssLineMap>();
    const std::string rcss = writer.take(*sheetLines);
    const std::string virtualPath = "@htmlui/doc/00c0ffee/ui/broken.css.rcss";
    VirtualFiles::add(virtualPath, rcss, 0xc0ffee);

    RcssWriter inlineWriter;
    inlineWriter.beginRule("p", SourceRef{htmlFile, 24, 1});
    inlineWriter.declaration("height", "nonsense", SourceRef{htmlFile, 28, 16});
    inlineWriter.endRule();
    auto inlineLines = std::make_shared<RcssLineMap>();
    const std::string inlineRcss = inlineWriter.take(*inlineLines);

    // RML: line 1 <rml>, 2 <head>, 3 <link>, 4 <style>, inline rcss from line 5, then a stray close tag
    const std::string rml = "<rml>\n<head>\n<link type=\"text/rcss\" href=\"/" + virtualPath + "\"/>\n<style>\n" + inlineRcss +
                            "</style>\n</head>\n<body>\n</div>\n</body>\n</rml>\n";
    std::vector<SourceRef> rmlRefs(20, SourceRef{htmlFile, 0, 6});
    const size_t strayLine = 5 + inlineLines->lines.size() + 3;   // the "</div>" line
    rmlRefs[strayLine - 1] = SourceRef{htmlFile, (uint32_t)html.find("</div>"), 6};
    auto rmlLines = std::make_shared<SourceRefList>(rmlRefs.begin(), rmlRefs.end());

    std::vector<Diagnostic> runtime;
    RmlLogTranslator::Registration registration;
    registration.key = 0xc0ffee;
    registration.owner = files;
    registration.files = files.get();
    registration.mappings.push_back({"/" + virtualPath, 1, &sheetLines->lines});
    registration.mappings.push_back({"ui/broken.html", 5, &inlineLines->lines});
    registration.mappings.push_back({"ui/broken.html", 1, rmlLines.get()});
    registration.sink = [&runtime](const Diagnostic& diagnostic){ runtime.push_back(diagnostic); };
    const uint64_t handle = RmlLogTranslator::add(registration);
    EXPECT_GE(RmlLogTranslator::registrationCount(), 1u);

    RecordingRenderInterface recording;
    Rml::Context* context = Rml::CreateContext("compat-translate", Rml::Vector2i(200, 100), &recording);
    ASSERT_NE(context, nullptr);
    Rml::ElementDocument* document = context->LoadDocumentFromMemory(rml, "ui/broken.html");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();

    // real RmlUi messages, each at the user's line (the stray close tag also makes RmlUi
    // complain about the next one, which maps to the RML line's default ref)
    std::vector<std::string> lines;
    for (const Diagnostic& diagnostic : runtime)
        lines.push_back(toText(diagnostic));
    ASSERT_EQ(runtime.size(), 4u) << ::testing::PrintToString(lines);
    sortDiagnostics(runtime);
    runtime.erase(runtime.begin() + 1);   // ui/broken.html:1:1, the follow-up mismatch
    EXPECT_EQ(runtime[0].file, "ui/broken.css");
    EXPECT_EQ(runtime[0].line, 3u);
    EXPECT_EQ(runtime[0].column, 3u);
    EXPECT_EQ(runtime[0].subject, "width");
    EXPECT_EQ(runtime[1].file, "ui/broken.html");
    EXPECT_EQ(runtime[1].line, 4u);
    EXPECT_EQ(runtime[1].column, 7u);
    EXPECT_EQ(runtime[1].subject, "height");
    EXPECT_EQ(runtime[2].file, "ui/broken.html");
    EXPECT_EQ(runtime[2].line, 7u);
    EXPECT_EQ(runtime[2].message, "RmlUi: Closing tag 'div' mismatched was expecting 'body'");
    for (const Diagnostic& diagnostic : runtime){
        EXPECT_EQ(diagnostic.code, codes::RmluiMessage);
        EXPECT_EQ(diagnostic.message.rfind("RmlUi: ", 0), 0u);
    }

    // logged once each with the prefix and the mapped location, not as raw RmlUi lines
    TestSystem& system = TestSystem::get();
    EXPECT_EQ(system.countLogs(D_LOG_WARN, "[HTML UI] ui/broken.css:3:3: warning: RmlUi: "), 1u);
    EXPECT_EQ(system.countLogs(D_LOG_WARN, "[HTML UI] ui/broken.html:4:7: warning: RmlUi: "), 1u);
    EXPECT_EQ(system.countLogs(D_LOG_WARN, "@htmlui/doc/00c0ffee"), 0u);

    Rml::RemoveContext("compat-translate");
    RmlLogTranslator::remove(handle);
    VirtualFiles::removeOwner(0xc0ffee);
}

TEST(CompatLogTranslator, CaptureCollectsAndUnmappedMessagesPassThrough){
    RmlUiSession session;
    ASSERT_TRUE(session.ok);
    RmlSystem& system = HtmlUI::getSystemInterface();
    {
        RmlLogTranslator::Capture outer;
        {
            RmlLogTranslator::Capture inner;
            Rml::Factory::InstanceStyleSheetString("a { width: bogus; }");
            EXPECT_EQ(inner.count(Severity::Warning), 1u);
            EXPECT_EQ(inner.problems(), 1u);
        }
        EXPECT_EQ(outer.messages().size(), 0u);   // the innermost capture collects
        Rml::Factory::InstanceStyleSheetString("a { height: bogus; }");
        EXPECT_EQ(outer.problems(), 1u);
    }
    EXPECT_EQ(system.getWarningCount(), 0u);

    // no capture, no registration: RmlSystem logs it as before
    Rml::Factory::InstanceStyleSheetString("a { margin: bogus; }");
    EXPECT_EQ(system.getWarningCount(), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI] Syntax error parsing property declaration 'margin: bogus;'"), 1u);
}
