// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The rest of p2-core: CSS serialization, the RCSS writer and its line maps, hashes, virtual
// files served through RmlFile (hook H3b), the pass-through CssRewriter seam, the embedded
// user-agent sheet, and the CompatApi facade (checkStylesheet with RmlUi verification,
// cssPropertyName, the checkDocument stub).

#include "CompatTestCommon.h"

#include "harness/RecordingRenderInterface.h"
#include "harness/TestSystem.h"

#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/Hash.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/compat/css/CssRewriter.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/ua/UserAgentStyle.h"

#include "HtmlUI.h"
#include "RmlFile.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>
#include <algorithm>
#include <set>

#include <map>

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

    FileAccess memoryFiles(const std::map<std::string, std::string>& files){
        FileAccess access;
        access.read = [files](const std::string& path, std::string& content){
            const auto it = files.find(path);
            if (it == files.end())
                return false;
            content = it->second;
            return true;
        };
        access.exists = [files](const std::string& path){ return files.count(path) > 0; };
        return access;
    }

    std::string roundTrip(const std::string& css){
        return serialize(parseComponentValueList(css));
    }

}

// ---- serialization

TEST(CompatSerialize, TokensAndValues){
    EXPECT_EQ(roundTrip("rgba(255, 0, 0, .5)"), "rgba(255, 0, 0, 0.5)");
    EXPECT_EQ(roundTrip("'single' \"double\""), "\"single\" \"double\"");
    EXPECT_EQ(roundTrip("'it\\'s \"x\"'"), "\"it's \\\"x\\\"\"");
    EXPECT_EQ(roundTrip("10px  1.50em -0.0 1e3 50%"), "10px 1.5em 0 1000 50%");
    EXPECT_EQ(roundTrip("url(a b.png) url(\"x.png\")"), " url(\"x.png\")");   // the bad url is dropped
    EXPECT_EQ(roundTrip("url(img/a.png)"), "url(\"img/a.png\")");
    EXPECT_EQ(roundTrip("#fff #1a"), "#fff #1a");
    EXPECT_EQ(roundTrip("a/**/b"), "a b");
    EXPECT_EQ(roundTrip("1/**/2"), "1 2");
    EXPECT_EQ(roundTrip("[data-x=\"1\"] > .a:hover"), "[data-x=\"1\"] > .a:hover");
    EXPECT_EQ(roundTrip("calc( 100%  -  2px )"), "calc( 100% - 2px )");
    EXPECT_EQ(roundTrip("@media"), "@media");

    SerializeOptions exact;
    exact.exactSeparators = true;
    EXPECT_EQ(serialize(parseComponentValueList("a/**/b"), exact), "a/**/b");
}

TEST(CompatSerialize, IdentsStringsNumbers){
    EXPECT_EQ(serializeIdent("abc"), "abc");
    EXPECT_EQ(serializeIdent("1a"), "\\31 a");
    EXPECT_EQ(serializeIdent("-1a"), "-\\31 a");
    EXPECT_EQ(serializeIdent("-"), "\\-");
    EXPECT_EQ(serializeIdent("a b"), "a\\ b");
    EXPECT_EQ(serializeIdent("caf\xC3\xA9"), "caf\xC3\xA9");
    EXPECT_EQ(serializeString("a\"b\\c"), "\"a\\\"b\\\\c\"");
    EXPECT_EQ(serializeString("line\nbreak"), "\"line\\a break\"");
    EXPECT_EQ(serializeUrl("a.png"), "url(\"a.png\")");
    EXPECT_EQ(formatNumber(0.2), "0.2");
    EXPECT_EQ(formatNumber(1), "1");
    EXPECT_EQ(formatNumber(-3.5), "-3.5");
    EXPECT_EQ(formatNumber(1.0 / 3.0), "0.333333");
    EXPECT_EQ(formatNumber(-0.0000001), "0");
    EXPECT_EQ(formatNumber(2.0 / 3.0, 2), "0.67");
    EXPECT_EQ(formatNumber(1e20), "100000000000000000000");
}


// ---- hashes

TEST(CompatHash, KnownVectors){
    EXPECT_EQ(sha1Hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    EXPECT_EQ(sha1Hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
    EXPECT_EQ(sha1Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    EXPECT_EQ(sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(fnv1a64(""), 0xcbf29ce484222325ull);
    EXPECT_EQ(fnv1a64("a"), 0xaf63dc4c8601ec8cull);
    EXPECT_EQ(toHex(0xc0ffee, 8), "00c0ffee");
}

// ---- virtual files (hook H3b)

TEST(CompatVirtualFiles, StoreAndOwners){
    VirtualFiles::add("@htmlui/test/a.rcss", "a {}", 7);
    VirtualFiles::add("/@htmlui/test/b.rcss", "b {}", 7);
    VirtualFiles::add("@htmlui/test/c.svg", "<svg/>");
    EXPECT_TRUE(VirtualFiles::isVirtual("@htmlui/x"));
    EXPECT_TRUE(VirtualFiles::isVirtual("/@htmlui/x"));
    EXPECT_FALSE(VirtualFiles::isVirtual("ui/@htmlui/x"));
    EXPECT_FALSE(VirtualFiles::isVirtual("@html/x"));
    EXPECT_EQ(VirtualFiles::normalize("/@htmlui/x"), "@htmlui/x");
    ASSERT_NE(VirtualFiles::find("/@htmlui/test/a.rcss"), nullptr);
    EXPECT_EQ(*VirtualFiles::find("@htmlui/test/b.rcss"), "b {}");
    EXPECT_EQ(VirtualFiles::find("@htmlui/test/none"), nullptr);
    EXPECT_EQ(VirtualFiles::find("ui/a.css"), nullptr);
    EXPECT_EQ(VirtualFiles::list("@htmlui/test/").size(), 3u);

    EXPECT_EQ(VirtualFiles::removeOwner(7), 2u);
    EXPECT_FALSE(VirtualFiles::exists("@htmlui/test/a.rcss"));
    EXPECT_TRUE(VirtualFiles::exists("@htmlui/test/c.svg"));
    EXPECT_EQ(VirtualFiles::removeOwner(0), 0u);   // process-lifetime files are never removed by owner
    EXPECT_TRUE(VirtualFiles::remove("@htmlui/test/c.svg"));
    EXPECT_FALSE(VirtualFiles::remove("@htmlui/test/c.svg"));

    // reads through FileAccess fall back to virtual files first
    VirtualFiles::add("@htmlui/test/d.css", "d {}");
    std::string content;
    EXPECT_TRUE(readAssetFile(memoryFiles({}), "/@htmlui/test/d.css", content));
    EXPECT_EQ(content, "d {}");
    EXPECT_TRUE(assetFileExists(FileAccess(), "@htmlui/test/d.css"));
    EXPECT_FALSE(assetFileExists(FileAccess(), "@htmlui/test/e.css"));
    // and FileData without an access: the test assets
    EXPECT_TRUE(readAssetFile(FileAccess(), "compat/css-properties.txt", content));
    EXPECT_FALSE(readAssetFile(FileAccess(), "compat/no-such-file.txt", content));
    VirtualFiles::remove("@htmlui/test/d.css");
}

TEST(CompatVirtualFiles, ServedThroughRmlFile){
    const std::string sheet = "body { display: block; } div { display: block; width: 13px; }";
    VirtualFiles::add("@htmlui/test/served.rcss", sheet);
    RmlFile& files = HtmlUI::getFileInterface();
    const Rml::FileHandle handle = files.Open("/@htmlui/test/served.rcss");
    ASSERT_NE(handle, (Rml::FileHandle)0);
    EXPECT_EQ(files.Length(handle), sheet.size());
    char buffer[128] = {};
    EXPECT_EQ(files.Read(buffer, sizeof(buffer), handle), sheet.size());
    EXPECT_EQ(std::string(buffer), sheet);
    files.Close(handle);
    Rml::String whole;
    EXPECT_TRUE(files.LoadFile("@htmlui/test/served.rcss", whole));
    EXPECT_EQ(whole, sheet);
    EXPECT_EQ(files.Open("@htmlui/test/missing.rcss"), (Rml::FileHandle)0);

    // a document links it like any sheet
    RmlUiSession rml;
    ASSERT_TRUE(rml.ok);
    test::RecordingRenderInterface recording;
    Rml::Context* context = Rml::CreateContext("compat-virtual", Rml::Vector2i(100, 100), &recording);
    ASSERT_NE(context, nullptr);
    Rml::ElementDocument* document = context->LoadDocumentFromMemory(
        "<rml><head><link type=\"text/rcss\" href=\"/@htmlui/test/served.rcss\"/></head><body><div id=\"d\"/></body></rml>", "ui/virtual.html");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    EXPECT_FLOAT_EQ(document->GetElementById("d")->GetClientWidth(), 13.0f);
    Rml::RemoveContext("compat-virtual");
    VirtualFiles::remove("@htmlui/test/served.rcss");
}

// ---- the CssRewriter seam

TEST(CompatRewriterSeam, PassThroughSession){
    SourceFiles files;
    const std::string css =
        "@import url(\"https://fonts.googleapis.com/css2?family=Inter\");\n"
        ".card { color: red; font-family: 'Inter', sans-serif; & .title { width: 10px } }\n"
        "@media (min-width: 600px) { .card { padding: 4px } }\n"
        "@keyframes fade { from { opacity: 0 } to { opacity: 1 } }\n"
        ".bg { background: url(https://images.example.com/a.png) }\n";
    const uint32_t file = files.add("ui/menu.css", css);
    const std::string html = "<div style=\"color: blue; width: 'x'\">";
    const uint32_t htmlFile = files.add("ui/menu.html", html);
    DiagnosticSink sink(files);

    RewriteContext context;
    context.virtualDirectory = "@htmlui/doc/0123abcd/";
    context.documentPath = "ui/menu.html";
    CssRewriter rewriter(CompatOptions(), files, sink, context);
    StylesheetInput inputs[2];
    inputs[0].kind = StylesheetInput::Kind::File;
    inputs[0].file = file;
    inputs[0].text = files.text(file);
    inputs[1].kind = StylesheetInput::Kind::StyleAttribute;
    inputs[1].file = htmlFile;
    inputs[1].text = std::string_view(html).substr(12, 23);
    inputs[1].baseOffset = 12;
    inputs[1].elementLoc = 4;
    const RewriteResult result = rewriter.rewrite(Span<const StylesheetInput>(inputs));

    ASSERT_EQ(result.sheets.size(), 2u);
    const StylesheetOutput& sheet = result.sheets[0];
    EXPECT_EQ(sheet.virtualPath, "@htmlui/doc/0123abcd/ui/menu.css.rcss");
    EXPECT_EQ(sheet.sourcePath, "ui/menu.css");
    // the p2-css rewriter: the #htmlui-root band, #rrggbbaa colors, the currentColor companion
    EXPECT_NE(sheet.rcss.find("#htmlui-root .card {\ncolor: #ff0000ff;\n--html-current-color: #ff0000ff;\nfont-family: Inter, sans-serif;\n}\n"),
              std::string::npos) << sheet.rcss;
    EXPECT_NE(sheet.rcss.find("#htmlui-root .card .title {\nwidth: 10px;\n}"), std::string::npos) << sheet.rcss;
    EXPECT_NE(sheet.rcss.find("@media (min-width: 600px) {\n#htmlui-root .card {\npadding: 4px;\n}\n}"), std::string::npos) << sheet.rcss;
    EXPECT_NE(sheet.rcss.find("@keyframes fade {"), std::string::npos) << sheet.rcss;
    // every output line maps into the stylesheet
    for (const SourceRef& ref : sheet.lines.lines)
        EXPECT_EQ(ref.file, file);

    const StylesheetOutput& inlineStyle = result.sheets[1];
    // width: 'x' is not a width: dropped (HTMLUI-CSS-0004)
    EXPECT_EQ(inlineStyle.rcss, "color: #0000ffff; --html-current-color: #0000ffff;");
    EXPECT_EQ(inlineStyle.elementLoc, 4u);
    EXPECT_EQ(inlineStyle.lines.lines[0].offset, 12u);

    ASSERT_EQ(result.remoteRefs.size(), 2u);
    EXPECT_EQ(result.remoteRefs[0].kind, "google-fonts-css");
    EXPECT_EQ(result.remoteRefs[1].url, "https://images.example.com/a.png");
    EXPECT_EQ(result.remoteRefs[1].kind, "image");
    EXPECT_EQ(result.dependencies, std::vector<std::string>{"ui/menu.css"});
    // the two remote files are not imported (no manifest), nesting is flattened, the bad width
    std::vector<std::string> codes;
    for (const Diagnostic& d : sink.diagnostics())
        codes.push_back(d.code.id());
    EXPECT_EQ(codes, (std::vector<std::string>{"HTMLUI-IMP-0001", "HTMLUI-AT-0008", "HTMLUI-IMP-0001", "HTMLUI-CSS-0004"}));

    EXPECT_EQ(CssRewriter::rewriteSelector("  .a  >  p:hover ", nullptr), ".a > p:hover");
    EXPECT_EQ(CssRewriter::rewriteDeclarations("color: red; margin : 0", nullptr), "color: #ff0000ff; --html-current-color: #ff0000ff; margin: 0;");
}

// ---- user-agent sheet

TEST(CompatUserAgent, EmbeddedRewrittenAndRegistered){
    ASSERT_FALSE(UserAgentStyle::embeddedFiles().empty());
    const std::string_view source = UserAgentStyle::source();
    EXPECT_FALSE(source.empty());

    UserAgentStyle::clearCache();
    const std::shared_ptr<const UserAgentStyle::Sheet> sheet = UserAgentStyle::get();
    ASSERT_NE(sheet, nullptr);
    EXPECT_EQ(sheet->virtualPath, "@htmlui/ua.rcss");
    EXPECT_TRUE(sheet->diagnostics.empty());
    EXPECT_FALSE(sheet->rcss.empty());
    EXPECT_EQ(UserAgentStyle::get(), sheet);   // once per process and fork set
    ASSERT_NE(VirtualFiles::find("/@htmlui/ua.rcss"), nullptr);
    EXPECT_EQ(*VirtualFiles::find("@htmlui/ua.rcss"), sheet->rcss);

    // another fork set has its own sheet (every feature is present since phase 3: the phase-2 set)
    const ForkFeatures other = ForkFeatures::present() == ForkFeatures::all() ? ForkFeatures::none() : ForkFeatures::all();
    const std::shared_ptr<const UserAgentStyle::Sheet> forked = UserAgentStyle::get(other);
    EXPECT_NE(forked, sheet);
    EXPECT_EQ(forked->virtualPath, UserAgentStyle::virtualPath(other));
    EXPECT_NE(forked->virtualPath, sheet->virtualPath);

    // RmlUi accepts it without a warning
    RmlUiSession rml;
    ASSERT_TRUE(rml.ok);
    const std::vector<Diagnostic> verified = verifyRcssWithRmlUi(sheet->rcss, sheet->virtualPath, sheet->lines, *sheet->files);
    EXPECT_TRUE(verified.empty()) << (verified.empty() ? "" : toText(verified[0]));

    UserAgentStyle::clearCache();
    EXPECT_EQ(VirtualFiles::find("@htmlui/ua.rcss"), nullptr);
}


// ---- CompatApi

// p4-gaps: a stylesheet on its own does not know the variables its documents define: a
// declaration that needs the value of a variable it does not declare (nor its imports) is not
// judged; one it declares with two values is; its document judges both
TEST(CompatApi, CheckStylesheetLeavesUndeclaredVariablesToItsDocuments){
    const FileAccess files = memoryFiles({
        {"ui/vars.css", "@import \"base.css\";\n.card { border: var(--edge); }\n.mixed { border: var(--line); }\n"
                        ".based { border: var(--base); }\n"},
        {"ui/base.css", ":root { --line: 1px solid #000; --base: 2px solid #f00; }\n.alt { --line: 2px dashed #00f; }\n"},
        {"ui/vars.html", "<html><head><link rel=\"stylesheet\" href=\"vars.css\"></head><body>"
                         "<div class=\"card\" style=\"--edge: 3px solid #0f0\">x</div></body></html>\n"},
        {"ui/typo.html", "<html><head><link rel=\"stylesheet\" href=\"vars.css\"></head><body>x</body></html>\n"},
    });
    CheckOptions options;
    options.files = files;
    options.verifyWithRmlUi = false;
    const auto untyped = [](const CheckReport& report){
        std::vector<uint32_t> lines;
        for (const Diagnostic& diagnostic : report.diagnostics){
            if (diagnostic.code == codes::VarUntyped && diagnostic.file == "ui/vars.css")
                lines.push_back(diagnostic.line);
        }
        return lines;
    };
    // alone: --edge is not declared (line 2 is not judged), --line is, with two values (line 3), and
    // --base once (in the imported sheet: line 4 is fine)
    EXPECT_EQ(untyped(checkStylesheet("ui/vars.css", options)), std::vector<uint32_t>{3});
    // a document that defines --edge: the same; one that does not: --edge is a finding there
    EXPECT_EQ(untyped(checkDocument("ui/vars.html", options)), std::vector<uint32_t>{3});
    EXPECT_EQ(untyped(checkDocument("ui/typo.html", options)), (std::vector<uint32_t>{2, 3}));
}

TEST(CompatApi, CheckStylesheet){
    const FileAccess files = memoryFiles({
        {"ui/menu.css", "a { color: red; width: bogus; }\n.b { height: 2px; \"unclosed\n}\n.c { background: url(//cdn.example.com/x.png) }\n"},
        {"ui/bad.css", std::string("a { color: red; }\n/* \xFF */")},
    });

    CheckOptions options;
    options.files = files;
    options.verifyWithRmlUi = false;
    CheckReport report = checkStylesheet("ui/menu.css", options);
    // The bad width, unclosed string and unimported remote image are distinct failures.
    ASSERT_EQ(report.diagnostics.size(), 3u);
    EXPECT_EQ(report.diagnostics[0].code, codes::UnsupportedValue);
    EXPECT_EQ(report.diagnostics[0].line, 1u);
    EXPECT_EQ(report.diagnostics[0].column, 17u);
    EXPECT_EQ(report.diagnostics[1].code, codes::CssSyntaxError);
    EXPECT_EQ(report.diagnostics[1].line, 2u);
    EXPECT_EQ(report.diagnostics[1].column, 19u);
    EXPECT_EQ(report.diagnostics[2].code, codes::NotImported);
    EXPECT_EQ(report.files, std::vector<std::string>{"ui/menu.css"});
    ASSERT_EQ(report.remoteRefs.size(), 1u);
    EXPECT_EQ(report.remoteRefs[0].url, "//cdn.example.com/x.png");

    // RmlUi verification: the rewriter passes nothing invalid on, so RmlUi reports nothing
    options.verifyWithRmlUi = true;
    ASSERT_FALSE(HtmlUI::isInitialized());
    report = checkStylesheet("ui/menu.css", options);
    EXPECT_FALSE(HtmlUI::isInitialized());   // acquired for the check only
    for (const Diagnostic& diagnostic : report.diagnostics)
        EXPECT_NE(diagnostic.code, codes::RmluiMessage) << toText(diagnostic);
    EXPECT_EQ(report.diagnostics.size(), 3u);

    // invalid UTF-8
    options.verifyWithRmlUi = false;
    report = checkStylesheet("ui/bad.css", options);
    ASSERT_EQ(report.diagnostics.size(), 1u);
    EXPECT_EQ(report.diagnostics[0].code, codes::InvalidUtf8);
    EXPECT_EQ(report.diagnostics[0].line, 2u);

    // missing file
    report = checkStylesheet("ui/none.css", options);
    ASSERT_EQ(report.diagnostics.size(), 1u);
    EXPECT_EQ(report.diagnostics[0].code, codes::FileNotFound);
    EXPECT_EQ(report.diagnostics[0].severity, Severity::Error);
    EXPECT_TRUE(report.files.empty());

    // remote references of a stylesheet
    const std::vector<RemoteReference> refs = collectRemoteReferences("ui/menu.css", files);
    ASSERT_EQ(refs.size(), 1u);
}

// checkDocument (p2-html, CompatApiDocument.cpp): the document compiled through the given file
// access, no RmlUi needed (phase 4's CLI)
TEST(CompatApi, CheckDocumentCompilesTheDocument){
    std::map<std::string, std::string> disk = {
        {"ui/menu.html", "<!DOCTYPE html><html><head><link rel=\"stylesheet\" href=\"menu.css\">"
                         "<link rel=\"stylesheet\" href=\"https://cdn.example.com/x.css\"></head>"
                         "<body><img src=\"missing.png\"><script>alert(1)</script></body></html>"},
        {"ui/menu.css", "p { color: red; }"},
    };
    CheckOptions options;
    options.verifyWithRmlUi = false;
    options.files.read = [&disk](const std::string& path, std::string& content){
        auto it = disk.find(path);
        if (it == disk.end())
            return false;
        content = it->second;
        return true;
    };
    options.files.exists = [&disk](const std::string& path){ return disk.count(path) > 0; };
    const CheckReport report = checkDocument("ui/menu.html", options);
    std::set<std::string> found;
    for (const Diagnostic& diagnostic : report.diagnostics){
        found.insert(diagnostic.code.id());
        EXPECT_EQ(diagnostic.file, "ui/menu.html");
    }
    EXPECT_TRUE(found.count("HTMLUI-GEN-0001"));   // missing.png
    EXPECT_TRUE(found.count("HTMLUI-IMP-0001"));   // the remote stylesheet
    EXPECT_TRUE(found.count("HTMLUI-HTML-0006"));  // JavaScript
    EXPECT_NE(std::find(report.files.begin(), report.files.end(), "ui/menu.html"), report.files.end());
    EXPECT_NE(std::find(report.files.begin(), report.files.end(), "ui/menu.css"), report.files.end());
    ASSERT_EQ(report.remoteRefs.size(), 1u);
    EXPECT_EQ(report.remoteRefs[0].url, "https://cdn.example.com/x.css");
    EXPECT_EQ(report.remoteRefs[0].kind, "stylesheet");
    EXPECT_EQ(collectRemoteReferences("ui/menu.html", options.files).size(), 1u);

    // a missing document: one error naming it
    const CheckReport missing = checkDocument("ui/none.html", options);
    ASSERT_EQ(missing.diagnostics.size(), 1u);
    EXPECT_EQ(missing.diagnostics[0].code, codes::FileNotFound);
    EXPECT_EQ(missing.diagnostics[0].file, "ui/none.html");
    EXPECT_TRUE(isHtmlDocumentPath("ui/a.HTML"));
    EXPECT_FALSE(isHtmlDocumentPath("ui/a.rml"));
}
