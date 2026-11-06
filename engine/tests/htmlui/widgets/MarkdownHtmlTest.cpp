// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Markdown emitter (S17 AC10, R24-R26): every fixtures/markdown/<NN-name>.md renders to the
// committed <NN-name>.rml (regenerate with JASPER_HTMLUI_UPDATE_GOLDEN=1 and review every
// diff), and each output is well-formed with only the elements the dialect makes. Plus raw
// HTML as text, autolinks, resolveUrl, dedent, and the 20 KB re-render target (Release only).

#include "compat/CompatTestCommon.h"
#include "compat/PerfFixtures.h"

#include "htmlui/widgets/Markdown/MarkdownHtml.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <utility>
#include <vector>

using namespace jasperengine::htmlui::markdown;
using namespace jasperengine::test::compat;

namespace {

    bool updateGoldens(){
        const char* value = std::getenv("JASPER_HTMLUI_UPDATE_GOLDEN");
        return value && value[0] && std::string(value) != "0";
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
        for (const auto& entry : std::filesystem::directory_iterator(fixturePath("markdown"))){
            if (entry.path().extension() == ".md")
                names.push_back(entry.path().stem().string());
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    std::string bodyOf(const std::string& inner){
        return "<div class=\"markdown-body\">" + inner + "</div>";
    }

    // The output is well-formed XML made of the dialect's elements: every element closed (the
    // void ones as <x/>), attributes double-quoted, no '<', '>' or '&' outside markup and the
    // emitter's references, no "{{". Returns what is wrong, "" when nothing is.
    std::string checkWellFormed(const std::string& rml){
        static const std::set<std::string> elements = {"div", "h1", "h2", "h3", "h4", "h5", "h6", "p", "ul", "ol", "li", "pre", "code",
                                                       "blockquote", "table", "thead", "tbody", "tr", "th", "td", "a", "strong", "em"};
        static const std::set<std::string> voidElements = {"hr", "img", "br"};
        static const char* const references[] = {"&amp;", "&lt;", "&gt;", "&quot;", "&#123;"};
        if (rml.find("{{") != std::string::npos)
            return "holds {{";
        std::vector<std::string> open;
        size_t at = 0;
        while (at < rml.size()){
            const char c = rml[at];
            if (c == '&'){
                bool known = false;
                for (const char* reference : references)
                    known = known || rml.compare(at, std::strlen(reference), reference) == 0;
                if (!known)
                    return "a bare '&' at " + std::to_string(at);
                at++;
                continue;
            }
            if (c == '>')
                return "a bare '>' at " + std::to_string(at);
            if (c != '<'){
                at++;
                continue;
            }
            const bool closing = at + 1 < rml.size() && rml[at + 1] == '/';
            size_t end = at + (closing ? 2 : 1);
            const size_t nameStart = end;
            while (end < rml.size() && std::isalnum((unsigned char)rml[end]))
                end++;
            const std::string name = rml.substr(nameStart, end - nameStart);
            if (closing){
                if (end >= rml.size() || rml[end] != '>')
                    return "a malformed end tag at " + std::to_string(at);
                if (open.empty() || open.back() != name)
                    return "</" + name + "> closes " + (open.empty() ? std::string("nothing") : "<" + open.back() + ">");
                open.pop_back();
                at = end + 1;
                continue;
            }
            if (!elements.count(name) && !voidElements.count(name))
                return "an element <" + name + "> at " + std::to_string(at);
            // attributes: name="value", the value free of '<' and '"'
            while (end < rml.size() && rml[end] == ' '){
                end++;
                while (end < rml.size() && (std::isalnum((unsigned char)rml[end]) || rml[end] == '-'))
                    end++;
                if (rml.compare(end, 2, "=\"") != 0)
                    return "a malformed attribute in <" + name + ">";
                const size_t close = rml.find_first_of("\"<", end + 2);
                if (close == std::string::npos || rml[close] != '"')
                    return "an unterminated attribute in <" + name + ">";
                end = close + 1;
            }
            const bool selfClosing = rml.compare(end, 2, "/>") == 0;
            if (!selfClosing && (end >= rml.size() || rml[end] != '>'))
                return "a malformed tag <" + name + ">";
            if (selfClosing != (voidElements.count(name) > 0))
                return "<" + name + "> " + (selfClosing ? "is self-closed" : "is not self-closed");
            if (!selfClosing)
                open.push_back(name);
            at = end + (selfClosing ? 2 : 1);
        }
        if (!open.empty())
            return "<" + open.back() + "> is not closed";
        return "";
    }

}

TEST(MarkdownHtml, Corpus){
    const std::vector<std::string> names = caseNames();
    ASSERT_GE(names.size(), 30u);
    for (const std::string& name : names){
        SCOPED_TRACE(name);
        std::string markdown;
        ASSERT_TRUE(readFixture("markdown/" + name + ".md", markdown));
        const RenderResult result = renderToRml(markdown);
        EXPECT_EQ(checkWellFormed(result.rml), "");
        EXPECT_EQ(result.rawHtml, name.find("raw-html") != std::string::npos);
        compareOrWrite("markdown/" + name + ".rml", result.rml);
    }
}

// The checker itself: it rejects what the emitter must never write.
TEST(MarkdownHtml, WellFormedCheckRejectsBadMarkup){
    EXPECT_EQ(checkWellFormed(bodyOf("<p>a<br/>b</p>")), "");
    EXPECT_NE(checkWellFormed(bodyOf("<p>a</div>")), "");
    EXPECT_NE(checkWellFormed(bodyOf("<script>x</script>")), "");
    EXPECT_NE(checkWellFormed(bodyOf("<p>a & b</p>")), "");
    EXPECT_NE(checkWellFormed(bodyOf("<p>{{x}}</p>")), "");
    EXPECT_NE(checkWellFormed(bodyOf("<hr>")), "");
    EXPECT_NE(checkWellFormed(bodyOf("<img src=x/>")), "");
    EXPECT_NE(checkWellFormed("<div class=\"markdown-body\"><p>"), "");
}

// R25: HTML in the Markdown, from whoever, is shown as text and noted.
TEST(MarkdownHtml, ScriptAndOnerrorAreText){
    RenderResult result = renderToRml("<script>alert(1)</script>");
    EXPECT_EQ(result.rml, bodyOf("<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>"));
    EXPECT_TRUE(result.rawHtml);

    result = renderToRml("<img src=x onerror=alert(1)>");
    EXPECT_EQ(result.rml, bodyOf("<p>&lt;img src=x onerror=alert(1)&gt;</p>"));
    EXPECT_TRUE(result.rawHtml);

    result = renderToRml("[x](https://a.example/\"onclick=\"alert(1)) ![\"/><script>](a.png)");
    EXPECT_EQ(result.rml, bodyOf("<p><a href=\"https://a.example/&quot;onclick=&quot;alert(1)\">x</a> "
                                 "<img src=\"a.png\" alt=\"&quot;/&gt;&lt;script&gt;\"/></p>"));
    EXPECT_EQ(checkWellFormed(result.rml), "");

    result = renderToRml("plain `<script>` code");
    EXPECT_EQ(result.rml, bodyOf("<p>plain <code>&lt;script&gt;</code> code</p>"));
    EXPECT_FALSE(result.rawHtml) << "markup inside code is code";
}

// AC10: an autolink is a link, also from an inline <markdown> source (dedented first).
TEST(MarkdownHtml, AutolinkIsALink){
    const RenderResult result = renderToRml("<https://a.b>");
    EXPECT_EQ(result.rml, bodyOf("<p><a href=\"https://a.b\">https://a.b</a></p>"));
    EXPECT_FALSE(result.rawHtml);

    const std::string inlineSource = "\n        See <https://a.b> and *this*.\n\n        - one\n          - two\n    ";
    EXPECT_EQ(renderToRml(dedent(inlineSource)).rml,
              bodyOf("<p>See <a href=\"https://a.b\">https://a.b</a> and <em>this</em>.</p><ul><li>one<ul><li>two</li></ul></li></ul>"));
}

// resolveUrl gets every link href and image src as written and decides what the output gets;
// an empty answer leaves the text.
TEST(MarkdownHtml, ResolveUrlMapsLinksAndImages){
    std::vector<std::pair<std::string, bool>> asked;
    RenderOptions options;
    options.resolveUrl = [&](const std::string& url, bool image){
        asked.emplace_back(url, image);
        return image ? "asset://ui/" + url : url + "#r";
    };
    const std::string markdown = "[docs **here**](https://x.example/d) ![i](icons/a.png) [![b](b.png)](https://y.example)";
    EXPECT_EQ(renderToRml(markdown, options).rml,
              bodyOf("<p><a href=\"https://x.example/d#r\">docs <strong>here</strong></a> <img src=\"asset://ui/icons/a.png\" alt=\"i\"/> "
                     "<a href=\"https://y.example#r\"><img src=\"asset://ui/b.png\" alt=\"b\"/></a></p>"));
    EXPECT_EQ(asked, (std::vector<std::pair<std::string, bool>>{{"https://x.example/d", false}, {"icons/a.png", true},
                                                                {"https://y.example", false}, {"b.png", true}}))
        << "a link of several spans is resolved once";

    options.resolveUrl = [](const std::string&, bool){ return std::string(); };
    EXPECT_EQ(renderToRml(markdown, options).rml, bodyOf("<p>docs <strong>here</strong> i b</p>"));

    EXPECT_EQ(renderToRml("![i](icons/a.png)").rml, bodyOf("<p><img src=\"icons/a.png\" alt=\"i\"/></p>")) << "identity without a resolver";
}

// What XML does not allow becomes U+FFFD; CRLF line ends read as LF.
TEST(MarkdownHtml, ControlCharactersAndLineEnds){
    EXPECT_EQ(renderToRml(std::string("a\x01" "b\x1F" "c", 5)).rml, bodyOf("<p>a\xEF\xBF\xBD" "b\xEF\xBF\xBD" "c</p>"));
    EXPECT_EQ(renderToRml(std::string("a\0b", 3)).rml, bodyOf("<p>a\xEF\xBF\xBD" "b</p>"));
    EXPECT_EQ(renderToRml("# T\r\n\r\ntext\r\nmore\r\n").rml, bodyOf("<h1>T</h1><p>text more</p>"));
    EXPECT_EQ(renderToRml("").rml, bodyOf(""));
    EXPECT_EQ(renderToRml("\n\n  \n").rml, bodyOf(""));
}

TEST(MarkdownHtml, Dedent){
    EXPECT_EQ(dedent("\n    # Title\n\n    text\n      indented\n  "), "# Title\n\ntext\n  indented");
    EXPECT_EQ(dedent("\t# T\n\t\tx"), "# T\n\tx") << "a tab is four columns";
    EXPECT_EQ(dedent("  a\n\tb"), "a\n  b") << "the columns of a tab past the cut are spaces";
    EXPECT_EQ(dedent("  a\r\n  b\r\n"), "a\nb");
    EXPECT_EQ(dedent("  a\n     \n  b"), "a\n\nb") << "a line of spaces is blank and does not count";
    EXPECT_EQ(dedent("a\n b"), "a\n b");
    EXPECT_EQ(dedent(" \n\t\n"), "");
    EXPECT_EQ(dedent(""), "");
    EXPECT_EQ(dedent("    x  "), "x  ") << "trailing spaces (a hard break) stay";
}

#ifdef NDEBUG

namespace {

    // Typical Markdown of about 20 KB: headings, paragraphs with emphasis, links and code,
    // nested lists, a code block and a table, repeated with numbered text.
    std::string typicalMarkdown(){
        std::string text;
        for (int i = 0; text.size() < 20 * 1024; i++){
            const std::string n = std::to_string(i);
            text += "## Section " + n + "\n\n"
                    "The **lantern " + n + "** lights the *old mill* road; see [the guide](https://example.com/guide/" + n + ") "
                    "or call `light(" + n + ")` from a script. A_snake_case name and 2 * 3 stay as written.\n"
                    "A second line of the paragraph, with ***both*** looks and an ![icon](icons/i" + n + ".png).\n\n"
                    "- first item with `code`\n"
                    "- second item\n"
                    "  - nested **strong** item\n"
                    "    - deeper item with a [link](https://example.com/" + n + ")\n"
                    "1. numbered\n"
                    "2. numbered again\n\n"
                    "```lua\n"
                    "local light = scene:find(\"lantern" + n + "\")\n"
                    "light.intensity = 2.5 -- <brighter> & warmer\n"
                    "```\n\n"
                    "| Light | Intensity |\n"
                    "|-------|----------:|\n"
                    "| Lantern " + n + " | 2.5 |\n"
                    "| Torch | 4 |\n\n"
                    "> A quote about light " + n + ".\n\n";
        }
        return text;
    }

}

// R26: re-rendering 20 KB of Markdown takes <= 2 ms on desktop (PerfFixtures.h: the least of up
// to 15 medians of 20 samples of 10 runs, CPU time). Enforced by the benchmark run
// (benchmarkPerfSlack: JASPER_HTMLUI_PERF_SLACK set, docs/testing.md); elsewhere printed.
TEST(MarkdownHtml, Perf20KB){
    const std::string markdown = typicalMarkdown();
    RenderResult result;
    const double slack = benchmarkPerfSlack();
    const double target = 2.0 * (slack > 0.0 ? slack : 1.0);
    const double ms = gatedCpuMs([&]{ result = renderToRml(markdown); }, 10, target);
    EXPECT_EQ(checkWellFormed(result.rml), "");
    std::printf("[perf] Markdown to RML: %zu bytes in, %zu bytes out, %.3f ms (target 2 ms%s)\n", markdown.size(), result.rml.size(), ms,
                slack > 0.0 ? "" : ", not enforced: JASPER_HTMLUI_PERF_SLACK unset");
    if (slack > 0.0){
        EXPECT_LE(ms, target);
    }
}

#endif
