// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_HTML_HTMLPARSER_H
#define COMPAT_HTML_HTMLPARSER_H

#include "htmlui/compat/html/HtmlTree.h"

#include <string_view>

namespace jasperengine::htmlui::compat {

    class DiagnosticSink;

    // HTML5 parsing with lexbor (phase-2 §3.7.1), browser-forgiving: doctype and quirks,
    // implied tags, all named entities, foreign SVG/MathML content, templates. The whole input
    // is parsed in one call, so the patched lexbor gives every node made from a token its
    // source range (engine/libs/lexbor/PATCHES.md). Parse errors become HTMLUI-HTML-0001
    // (Info: the browser recovers the same way).
    class HtmlParser {
    public:
        // Elements nested deeper are attached to their ancestor at this depth (Chromium's
        // kMaximumHTMLParserDOMTreeDepth), which keeps every later pass's recursion bounded.
        static constexpr int kMaximumDepth = 512;

        // A document. `text` is the registered text of `file` (offsets count from its start).
        // root is the html element.
        static HtmlDocument parseDocument(uint32_t file, std::string_view text, DiagnosticSink* sink);

        // A fragment parsed in the context of a `contextTag` element (innerHTML,
        // insertAdjacentHTML: "body"), `text` starting at byte `baseOffset` of `file` (0 = no
        // file). root is an implied "#fragment" element holding the nodes.
        static HtmlDocument parseFragment(uint32_t file, std::string_view text, uint32_t baseOffset,
                                          DiagnosticSink* sink, std::string_view contextTag = "body");

        // HTML void elements (no end tag, no content)
        static bool isVoidElement(std::string_view tag);
    };

}

#endif //COMPAT_HTML_HTMLPARSER_H
