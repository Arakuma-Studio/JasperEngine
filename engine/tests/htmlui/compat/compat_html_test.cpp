// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// HTML parsing and fragment behavior (phase-2 §3.7): depth limits, source locations,
// inert fragment scripts/styles and legacy HTML color values.

#include "CompatTestCommon.h"

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/html/HtmlParser.h"
#include "htmlui/compat/html/HtmlTransform.h"

#include <gtest/gtest.h>

#include <set>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test::compat;

namespace {

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
    EXPECT_LE(depth, 513);   // documented maximum nesting depth: 512, plus the root
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
