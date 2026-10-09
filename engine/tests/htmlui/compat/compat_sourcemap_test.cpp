// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Source maps (phase-2 §3.7.6): a document loaded into a real RmlUi context; every element's
// data-htmlui-loc resolves (locateElement) to the line:column of its start tag in the HTML;
// RML lines map back to the source; a problem RmlUi or the rewriter finds in a style block is
// reported at the HTML line it came from.

#include "CompatTestCommon.h"

#include "harness/RecordingRenderInterface.h"

#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/VirtualFiles.h"

#include "HtmlUI.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <cctype>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::htmlui::compat;

namespace {

    struct Session {
        bool ok;
        RecordingRenderInterface recording;
        Rml::Context* context = nullptr;
        Session(): ok(HtmlUI::acquire()){
            if (ok)
                context = Rml::CreateContext("compat-sourcemap", Rml::Vector2i(800, 600), &recording);
        }
        ~Session(){
            if (context)
                Rml::RemoveContext("compat-sourcemap");
            Rml::ReleaseTextures(&recording);
            Rml::ReleaseRenderManagers();
            if (ok)
                HtmlUI::release();
        }
    };

    // the text at a 1-based line and code-point column of `text`
    std::string_view at(const std::string& text, uint32_t line, uint32_t column){
        size_t offset = 0;
        for (uint32_t l = 1; l < line && offset < text.size(); offset++){
            if (text[offset] == '\n')
                l++;
        }
        for (uint32_t c = 1; c < column && offset < text.size(); c++){
            offset++;
            while (offset < text.size() && ((unsigned char)text[offset] & 0xC0) == 0x80)
                offset++;
        }
        return std::string_view(text).substr(offset);
    }

    void collect(Rml::Element* element, std::vector<Rml::Element*>& out){
        out.push_back(element);
        for (int i = 0; i < element->GetNumChildren(); i++)
            collect(element->GetChild(i), out);
    }

    bool startsWithNoCase(std::string_view text, const std::string& prefix){
        if (text.size() < prefix.size())
            return false;
        for (size_t i = 0; i < prefix.size(); i++){
            if (std::tolower((unsigned char)text[i]) != std::tolower((unsigned char)prefix[i]))
                return false;
        }
        return true;
    }

}

TEST(CompatSourceMap, ElementsResolveToTheirStartTags){
    Session session;
    ASSERT_TRUE(session.context);
    const std::string html =
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head><title>Map</title></head>\n"
        "<body>\n"
        "  <main id=\"main\">\n"
        "    <h1>Título ünïcode</h1><p class=\"x\">a <b>b</b> <i>c</i></p>\n"
        "    <ul><li>one<li>two</ul>\n"
        "    <svg viewBox=\"0 0 2 2\"><rect width=\"1\" height=\"1\"/></svg>\n"
        "    <table><tr><td>cell</td></tr></table>\n"
        "  </main>\n"
        "</body>\n"
        "</html>\n";
    VirtualFiles::add("@htmlui/test/map/page.html", html);
    std::shared_ptr<const CompatDocument> compiled;
    CompatOptions options;
    Rml::ElementDocument* document = DocumentLoader::load(session.context, "@htmlui/test/map/page.html", options, &compiled);
    ASSERT_NE(document, nullptr);

    std::vector<Rml::Element*> elements;
    collect(document, elements);
    size_t located = 0;
    for (Rml::Element* element : elements){
        if (!element->HasAttribute(DocumentLoader::kLocationAttribute))
            continue;
        std::optional<Diagnostic> location = locateElement(*compiled, element);
        ASSERT_TRUE(location.has_value()) << element->GetAddress();
        EXPECT_EQ(location->file, "@htmlui/test/map/page.html");
        std::string tag = element->GetTagName();
        if (tag == "htmlbody")
            tag = "body";
        else if (element == document)
            tag = "html";
        const std::string_view source = at(html, location->line, location->column);
        // implied elements (tbody) point at the nearest written ancestor
        if (tag != "tbody"){
            EXPECT_TRUE(startsWithNoCase(source, "<" + tag)) << tag << " at " << location->line << ":" << location->column << ": " << source.substr(0, 20);
        }
        located++;
    }
    EXPECT_GE(located, 14u);

    // RmlUi's XML lines: the line of each element's start tag maps to that element
    Rml::Element* paragraph = document->QuerySelector("p.x");
    ASSERT_NE(paragraph, nullptr);
    const std::string loc = paragraph->GetAttribute<Rml::String>(DocumentLoader::kLocationAttribute, "");
    const size_t pos = compiled->rml.find("data-htmlui-loc=\"" + loc + "\"");
    ASSERT_NE(pos, std::string::npos);
    const uint32_t rmlLine = 1 + (uint32_t)std::count(compiled->rml.begin(), compiled->rml.begin() + (std::ptrdiff_t)pos, '\n');
    const SourceRef ref = compiled->sourceMap.line(rmlLine);
    ASSERT_TRUE(ref.valid());
    EXPECT_EQ(compiled->files.position(ref.file, ref.offset).line, 6u);

    session.context->UnloadDocument(document);
    session.context->Update();
    VirtualFiles::remove("@htmlui/test/map/page.html");
}

TEST(CompatSourceMap, StyleProblemsAreReportedAtTheHtmlLine){
    Session session;
    ASSERT_TRUE(session.context);
    const std::string html =
        "<!DOCTYPE html>\n"
        "<html><head>\n"
        "<style>\n"
        ".a { color: #ff0000; }\n"
        ".b { width: bogus; }\n"
        "</style>\n"
        "</head><body><p class=\"a b\">x</p></body></html>\n";
    VirtualFiles::add("@htmlui/test/map/style.html", html);
    std::shared_ptr<const CompatDocument> compiled;
    CompatOptions options;
    Rml::ElementDocument* document = DocumentLoader::load(session.context, "@htmlui/test/map/style.html", options, &compiled);
    ASSERT_NE(document, nullptr);
    // the rewriter drops it (p2-css) or RmlUi rejects what the seam passes through: either way
    // a diagnostic of the document sits at line 5 of the HTML
    bool found = false;
    for (const Diagnostic& diagnostic : DocumentLoader::diagnosticsOf(document)){
        if (diagnostic.file == "@htmlui/test/map/style.html" && diagnostic.line == 5 && diagnostic.severity != Severity::Info)
            found = true;
    }
    EXPECT_TRUE(found);
    session.context->UnloadDocument(document);
    session.context->Update();
    VirtualFiles::remove("@htmlui/test/map/style.html");
}
