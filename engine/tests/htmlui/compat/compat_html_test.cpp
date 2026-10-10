// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The HTML stage (phase-2 §3.7, §5.2): every fixtures/compat/html/<case>.html compiles to
// the committed <case>.rml and <case>.diag.txt (toText lines, sorted). Regenerate with
// JASPER_HTMLUI_UPDATE_GOLDEN=1 and review every diff. Plus the parser's own guarantees
// (depth limit, fragments, templates) and the transforms' helpers.

#include "CompatTestCommon.h"

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/html/HtmlParser.h"
#include "htmlui/compat/html/HtmlTransform.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test::compat;

namespace {

    bool updateGoldens(){
        const char* value = std::getenv("JASPER_HTMLUI_UPDATE_GOLDEN");
        return value && value[0] && std::string(value) != "0";
    }

    // reads below fixtures/ (asset-relative paths as the compile asks for them)
    FileAccess fixtureAccess(){
        FileAccess access;
        access.read = [](const std::string& path, std::string& content){ return readFixture(path, content); };
        access.exists = [](const std::string& path){
            std::error_code error;
            return std::filesystem::is_regular_file(fixturePath(path), error);
        };
        return access;
    }

    CompatOptions goldenOptions(){
        CompatOptions options;
        options.viewport = CompatSize{1280, 720};
        options.dpRatio = 1.0f;
        options.touchDevice = false;
        return options;
    }

    // the per-compile virtual directory is a counter: not part of the golden
    std::string normalized(const std::string& text){
        static const std::regex key("@htmlui/doc/[0-9a-f]{16}/");
        return std::regex_replace(text, key, "@htmlui/doc/KEY/");
    }

    std::string diagnosticsText(const std::vector<Diagnostic>& diagnostics){
        std::vector<std::string> lines;
        for (const Diagnostic& diagnostic : diagnostics)
            lines.push_back(toText(diagnostic));
        std::sort(lines.begin(), lines.end());
        std::string text;
        for (const std::string& line : lines)
            text += line + "\n";
        return text;
    }

    void compareOrWrite(const std::string& relative, const std::string& actual){
        if (updateGoldens()){
            std::ofstream file(fixturePath(relative), std::ios::binary);
            file << actual;
            return;
        }
        std::string expected;
        ASSERT_TRUE(readFixture(relative, expected)) << relative << " is missing (JASPER_HTMLUI_UPDATE_GOLDEN=1 writes it)";
        EXPECT_EQ(expected, actual) << relative << " differs";
    }

    std::vector<std::string> caseNames(){
        std::vector<std::string> names;
        for (const auto& entry : std::filesystem::directory_iterator(fixturePath("compat/html"))){
            if (entry.path().extension() == ".html")
                names.push_back(entry.path().stem().string());
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    const HtmlNode* findElement(const HtmlNode& node, const std::string& tag){
        for (const std::unique_ptr<HtmlNode>& child : node.children){
            if (child->kind != HtmlNode::Kind::Element)
                continue;
            if (child->tag == tag)
                return child.get();
            if (const HtmlNode* found = findElement(*child, tag))
                return found;
        }
        return nullptr;
    }

}

TEST(CompatHtml, GoldenCases){
    const std::vector<std::string> names = caseNames();
    ASSERT_GE(names.size(), 25u);
    for (const std::string& name : names){
        SCOPED_TRACE(name);
        const std::string path = "compat/html/" + name + ".html";
        std::shared_ptr<const CompatDocument> compiled = DocumentLoader::compile(path, goldenOptions(), fixtureAccess());
        ASSERT_TRUE(compiled->compiled);

        std::string rml = normalized(compiled->rml);
        for (size_t i = 0; i < compiled->templates.size(); i++){
            rml += "<!-- template " + std::to_string(i) + " id=\"" + compiled->templates[i].id + "\" -->\n" + normalized(compiled->templates[i].rml) + "\n";
        }
        for (const StylesheetOutput& sheet : compiled->stylesheets){
            if (sheet.kind == StylesheetInput::Kind::File)
                rml += "<!-- " + normalized(sheet.virtualPath) + " -->\n" + sheet.rcss;
        }
        for (const DocumentScript& script : compiled->scripts)
            rml += "<!-- lua " + script.chunkName + " (" + script.sourceFile + ":" + std::to_string(script.firstLine) + ") -->\n" + script.code + "\n";
        compareOrWrite("compat/html/" + name + ".rml", rml);
        compareOrWrite("compat/html/" + name + ".diag.txt", normalized(diagnosticsText(compiled->diagnostics)));
    }
}

TEST(CompatHtml, ElementLocationsCoverEveryElement){
    std::shared_ptr<const CompatDocument> compiled = DocumentLoader::compile("compat/html/forms.html", goldenOptions(), fixtureAccess());
    ASSERT_TRUE(compiled->compiled);
    // every data-htmlui-loc in the RML indexes elementRefs, in order
    const std::regex loc("data-htmlui-loc=\"([0-9]+)\"");
    size_t expected = 0;
    for (auto it = std::sregex_iterator(compiled->rml.begin(), compiled->rml.end(), loc); it != std::sregex_iterator(); ++it){
        EXPECT_EQ(std::stoul((*it)[1].str()), expected);
        expected++;
    }
    EXPECT_EQ(expected, compiled->sourceMap.elementRefs.size());
    // the root and the body are located at their tags (both implied here: the file has none)
    ASSERT_GE(compiled->sourceMap.elementRefs.size(), 3u);
    EXPECT_TRUE(compiled->sourceMap.elementRefs[2].valid());
}

TEST(CompatHtml, RootMappingAndShape){
    std::shared_ptr<const CompatDocument> compiled = DocumentLoader::compileString(
        "<!DOCTYPE html><html id=\"app\" class=\"theme\"><head><title>T</title></head><body class=\"b\"><p>x</p></body></html>",
        "ui/shape.html", goldenOptions());
    ASSERT_TRUE(compiled->compiled);
    const std::string& rml = compiled->rml;
    EXPECT_EQ(rml.rfind("<rml>\n<head>\n<title>T</title>\n<link type=\"text/rcss\" href=\"/@htmlui/", 0), 0u) << rml;
    EXPECT_NE(rml.find("origin=\"user-agent\""), std::string::npos);
    // (a line break between two tags goes inside the first one's '>': white space between tags
    // would be laid out as a space)
    EXPECT_NE(rml.find("<body data-html-id=\"app\" class=\"theme\" id=\"htmlui-root\" data-htmlui-loc=\"0\"\n>"), std::string::npos) << rml;
    EXPECT_NE(rml.find("<htmlbody class=\"b\" data-htmlui-loc=\"1\"\n>"), std::string::npos) << rml;
    EXPECT_NE(rml.find("</htmlbody></body>\n</rml>"), std::string::npos) << rml;
    EXPECT_EQ(compiled->title, "T");
}

TEST(CompatHtml, DepthIsLimited){
    std::string html = "<!DOCTYPE html><body>";
    for (int i = 0; i < 2000; i++)
        html += "<div>";
    html += "deep";
    SourceFiles files;
    const uint32_t file = files.add("ui/deep.html", html);
    DiagnosticSink sink(files);
    HtmlDocument document = HtmlParser::parseDocument(file, files.text(file), &sink);
    int depth = 0;
    const HtmlNode* node = document.root.get();
    while (node){
        const HtmlNode* next = nullptr;
        for (const std::unique_ptr<HtmlNode>& child : node->children){
            if (child->kind == HtmlNode::Kind::Element)
                next = child.get();
        }
        node = next;
        depth++;
    }
    EXPECT_LE(depth, HtmlParser::kMaximumDepth + 1);
    bool reported = false;
    for (const Diagnostic& diagnostic : sink.diagnostics()){
        if (diagnostic.code == codes::HtmlParseError && diagnostic.message.find("nested deeper") != std::string::npos)
            reported = true;
    }
    EXPECT_TRUE(reported);
}

TEST(CompatHtml, OffsetsOfStartTagsAndAttributes){
    const std::string html = "<!DOCTYPE html>\n<p class=\"a\" hidden>x</p>\n<div\n  id='d'>y</div>";
    SourceFiles files;
    const uint32_t file = files.add("ui/o.html", html);
    HtmlDocument document = HtmlParser::parseDocument(file, files.text(file), nullptr);
    const HtmlNode* p = findElement(*document.root, "p");
    ASSERT_TRUE(p);
    EXPECT_EQ(files.slice(p->ref), "<p class=\"a\" hidden>");
    EXPECT_EQ(files.slice(p->findAttr("class")->nameRef), "class");
    EXPECT_EQ(files.slice(p->findAttr("class")->valueRef), "a");
    EXPECT_EQ(files.slice(p->findAttr("hidden")->nameRef), "hidden");
    EXPECT_FALSE(p->findAttr("hidden")->valueRef.valid());
    const HtmlNode* div = findElement(*document.root, "div");
    ASSERT_TRUE(div);
    EXPECT_EQ(files.position(file, div->ref.offset).line, 3u);
    EXPECT_EQ(files.slice(div->findAttr("id")->valueRef), "d");
    EXPECT_EQ(files.position(file, div->findAttr("id")->nameRef.offset).line, 4u);
    // implied
    const HtmlNode* body = findElement(*document.root, "body");
    ASSERT_TRUE(body);
    EXPECT_TRUE(body->implied);
}

TEST(CompatHtml, FragmentsAreParsedInBodyContext){
    std::vector<Diagnostic> diagnostics;
    FragmentOptions options;
    options.documentPath = "ui/menu.html";
    const std::string rml = DocumentLoader::fragmentToRml("<td>cell</td><li>a&nbsp;b<br><img src=\"img/x.png\">", "ui/menu.html (innerHTML)", diagnostics, options);
    // a td outside a table is dropped by the HTML parser in body context (text kept)
    EXPECT_EQ(rml.find("<td"), std::string::npos) << rml;
    // (a start tag that follows another tag starts a new RML line, the break inside the first tag)
    EXPECT_NE(rml.find("<li>a\xC2\xA0" "b<br\n/><img src=\"asset://ui/img/x.png\"/></li>"), std::string::npos) << rml;
    // no data-htmlui-loc on runtime HTML
    EXPECT_EQ(rml.find("data-htmlui-loc"), std::string::npos);
    // the missing image is reported at the fragment
    bool missing = false;
    for (const Diagnostic& diagnostic : diagnostics){
        if (diagnostic.code == codes::FileNotFound && diagnostic.file == "ui/menu.html (innerHTML)")
            missing = true;
    }
    EXPECT_TRUE(missing);
}

TEST(CompatHtml, FragmentScriptsAndStylesAreInert){
    std::vector<Diagnostic> diagnostics;
    const std::string rml = DocumentLoader::fragmentToRml("<script>alert(1)</script><style>p{}</style><p onclick=\"x()\" align=\"center\">p</p>",
                                                          "fragment", diagnostics);
    EXPECT_EQ(rml.find("script"), std::string::npos);
    EXPECT_EQ(rml.find("<style"), std::string::npos);
    EXPECT_EQ(rml.find("onclick"), std::string::npos);
    // hints become inline declarations in fragments
    EXPECT_NE(rml.find("style=\"text-align: center;\""), std::string::npos) << rml;
    std::set<std::string> codesFound;
    for (const Diagnostic& diagnostic : diagnostics)
        codesFound.insert(diagnostic.code.id());
    EXPECT_TRUE(codesFound.count("HTMLUI-HTML-0006"));
    EXPECT_TRUE(codesFound.count("HTMLUI-HTML-0010"));
    EXPECT_TRUE(codesFound.count("HTMLUI-HTML-0005"));
}

TEST(CompatHtml, LegacyColors){
    EXPECT_EQ(html::legacyColor("red"), "#ff0000ff");
    EXPECT_EQ(html::legacyColor("#f00"), "#ff0000ff");
    EXPECT_EQ(html::legacyColor("#123456"), "#123456ff");
    EXPECT_EQ(html::legacyColor("chucknorris"), "#c00000ff");
    EXPECT_EQ(html::legacyColor("transparent"), "");
    EXPECT_EQ(html::legacyColor(""), "");
    EXPECT_EQ(html::legacyColor("  ReD "), "#ff0000ff");
}

TEST(CompatHtml, KebabCaseOfIconNames){
    EXPECT_EQ(html::kebabCase("ArrowRight"), "arrow-right");
    EXPECT_EQ(html::kebabCase("arrowRight"), "arrow-right");
    EXPECT_EQ(html::kebabCase("Volume2"), "volume-2");
    EXPECT_EQ(html::kebabCase("arrow-right"), "arrow-right");
}

TEST(CompatHtml, LargeGeneratedPageCompiles){
    // a 3 500-node settings page (§5.2): no crash, every element located
    std::string html = "<!DOCTYPE html><html><head><title>Settings</title><style>.row{display:flex}</style></head><body><main>";
    for (int i = 0; i < 500; i++){
        html += "<div class=\"row\" id=\"row" + std::to_string(i) + "\"><label for=\"in" + std::to_string(i) + "\">Option " +
                std::to_string(i) + "</label><input id=\"in" + std::to_string(i) + "\" type=\"range\" min=\"0\" max=\"100\">" +
                "<span class=\"value\">50</span><button type=\"button\">Reset</button></div>";
    }
    html += "</main></body></html>";
    std::shared_ptr<const CompatDocument> compiled = DocumentLoader::compileString(html, "ui/generated.html", goldenOptions());
    ASSERT_TRUE(compiled->compiled);
    EXPECT_GT(compiled->sourceMap.elementRefs.size(), 2500u);
    for (const SourceRef& ref : compiled->sourceMap.elementRefs)
        EXPECT_TRUE(ref.valid());
}
