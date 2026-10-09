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
#include "htmlui/compat/css/RcssWriter.h"
#include "htmlui/compat/ua/UserAgentStyle.h"

#include "HtmlUI.h"
#include "RmlFile.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

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

// ---- RCSS writer

TEST(CompatRcssWriter, LinesMapToSources){
    RcssWriter writer;
    const SourceRef rule{1, 0, 5}, color{1, 10, 6}, media{1, 30, 6}, inner{1, 40, 3}, font{1, 60, 9};
    writer.beginRule(".a", rule);
    writer.declaration("color", "#ff0000ff", color);
    RcssMediaCondition condition;
    condition.features = {"min-width: 600px", "theme: dark"};
    writer.beginMedia(condition, media);   // closes the open rule
    writer.beginRule(".b", inner);
    writer.declaration("width", "10px", inner);
    writer.atRule("@font-face {\nfont-family: Inter;\nsrc: fonts/inter.ttf;\n}", font);
    RcssLineMap lines;
    const std::string rcss = writer.take(lines);
    EXPECT_EQ(rcss,
              ".a {\n"
              "color: #ff0000ff;\n"
              "}\n"
              "@media (min-width: 600px) and (theme: dark) {\n"
              ".b {\n"
              "width: 10px;\n"
              "}\n"
              "@font-face {\n"
              "font-family: Inter;\n"
              "src: fonts/inter.ttf;\n"
              "}\n"
              "}\n");
    ASSERT_EQ(lines.lines.size(), 12u);
    EXPECT_EQ(lines.at(1), rule);
    EXPECT_EQ(lines.at(2), color);
    EXPECT_EQ(lines.at(3), rule);
    EXPECT_EQ(lines.at(4), media);
    EXPECT_EQ(lines.at(6), inner);
    EXPECT_EQ(lines.at(9), font);
    EXPECT_EQ(lines.at(12), media);
    EXPECT_TRUE(writer.empty());

    RcssMediaCondition negated;
    negated.negated = true;
    negated.features = {"theme: reduced-motion"};
    EXPECT_EQ(negated.text(), "not (theme: reduced-motion)");
    RcssMediaCondition raw;
    raw.raw = "(orientation: landscape)";
    EXPECT_EQ(raw.text(), "(orientation: landscape)");
}

TEST(CompatRcssWriter, DeclarationListOnOneLine){
    RcssWriter writer(RcssWriter::Mode::DeclarationList);
    writer.declaration("color", "#000000ff", SourceRef{1, 3, 4});
    writer.declaration("width", "10px", SourceRef{1, 9, 4});
    writer.beginRule("ignored", SourceRef{});
    RcssLineMap lines;
    EXPECT_EQ(writer.take(lines), "color: #000000ff; width: 10px;");
    ASSERT_EQ(lines.lines.size(), 1u);
    EXPECT_EQ(lines.lines[0], (SourceRef{1, 3, 4}));
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
    EXPECT_NE(source.find("display: block"), std::string_view::npos);
    EXPECT_EQ(source.find('\r'), std::string_view::npos);

    std::string onDisk;
    ASSERT_TRUE(readFixture("../../../core/htmlui/compat/ua/useragent.css", onDisk));
    onDisk.erase(std::remove(onDisk.begin(), onDisk.end(), '\r'), onDisk.end());
    EXPECT_EQ(source, onDisk);

    UserAgentStyle::clearCache();
    const std::shared_ptr<const UserAgentStyle::Sheet> sheet = UserAgentStyle::get();
    ASSERT_NE(sheet, nullptr);
    EXPECT_EQ(sheet->virtualPath, "@htmlui/ua.rcss");
    EXPECT_TRUE(sheet->diagnostics.empty());
    EXPECT_FALSE(sheet->rcss.empty());
    EXPECT_EQ(UserAgentStyle::get(), sheet);   // once per process and fork set
    ASSERT_NE(VirtualFiles::find("/@htmlui/ua.rcss"), nullptr);
    EXPECT_EQ(*VirtualFiles::find("@htmlui/ua.rcss"), sheet->rcss);

    const ForkFeatures all = ForkFeatures::all();
    const std::shared_ptr<const UserAgentStyle::Sheet> forked = UserAgentStyle::get(all);
    EXPECT_NE(forked, sheet);
    EXPECT_EQ(forked->virtualPath, UserAgentStyle::virtualPath(all));
    EXPECT_NE(forked->virtualPath, sheet->virtualPath);

    // RmlUi accepts it without a warning
    RmlUiSession rml;
    ASSERT_TRUE(rml.ok);
    const std::vector<Diagnostic> verified = verifyRcssWithRmlUi(sheet->rcss, sheet->virtualPath, sheet->lines, *sheet->files);
    EXPECT_TRUE(verified.empty()) << (verified.empty() ? "" : toText(verified[0]));

    UserAgentStyle::clearCache();
    EXPECT_EQ(VirtualFiles::find("@htmlui/ua.rcss"), nullptr);
}

TEST(CompatUserAgent, EmbeddedHeaderMatchesTheCMakeOne){
    // the Apple export writes the header from C++: it must be the one embed_ua.cmake makes
    std::ifstream generated(std::string(JASPER_HTMLUI_TEST_BINARY_DIR) + "/../../generated/htmlui/htmlui_ua.h", std::ios::binary);
    ASSERT_TRUE(generated.good());
    std::ostringstream stream;
    stream << generated.rdbuf();
    EXPECT_EQ(UserAgentStyle::buildEmbeddedHeader(), stream.str());

    EXPECT_NE(UserAgentStyle::buildEmbeddedHeader({}).find("static const size_t uaFileCount = 0;"), std::string::npos);
}

// ---- CompatApi

TEST(CompatApi, CheckStylesheet){
    const FileAccess files = memoryFiles({
        {"ui/menu.css", "a { color: red; width: bogus; }\n.b { height: 2px; \"unclosed\n}\n.c { background: url(//cdn.example.com/x.png) }\n"},
        {"ui/bad.css", std::string("a { color: red; }\n/* \xFF */")},
    });

    CheckOptions options;
    options.files = files;
    options.verifyWithRmlUi = false;
    CheckReport report = checkStylesheet("ui/menu.css", options);
    std::vector<std::string> lines;
    for (const Diagnostic& diagnostic : report.diagnostics)
        lines.push_back(toText(diagnostic));
    // the rewriter (p2-css) reports the bad width itself, the parser the open string, the URL
    // resolver the remote image that is not imported
    ASSERT_EQ(lines.size(), 3u) << ::testing::PrintToString(lines);
    EXPECT_EQ(lines[0].rfind("ui/menu.css:1:17: warning: 'width: bogus' is not supported", 0), 0u) << lines[0];
    EXPECT_NE(lines[0].find("[HTMLUI-CSS-0004]"), std::string::npos) << lines[0];
    EXPECT_EQ(lines[1].rfind("ui/menu.css:2:19: warning: a string is not closed", 0), 0u) << lines[1];
    EXPECT_NE(lines[2].find("[HTMLUI-IMP-0001]"), std::string::npos) << lines[2];
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

// p2-html implements checkDocument (CompatApiDocument.cpp) and replaces this test with its own
TEST(CompatApi, CheckDocumentIsAStubUntilTheHtmlStage){
    CheckOptions options;
    options.verifyWithRmlUi = false;
    const CheckReport report = checkDocument("ui/menu.html", options);
    ASSERT_EQ(report.diagnostics.size(), 1u);
    EXPECT_EQ(report.diagnostics[0].code, codes::InternalError);
    EXPECT_EQ(report.diagnostics[0].file, "ui/menu.html");
}
