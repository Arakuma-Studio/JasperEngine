// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_HTML_RMLSERIALIZER_H
#define COMPAT_HTML_RMLSERIALIZER_H

#include "htmlui/compat/CompatTypes.h"
#include "htmlui/compat/html/HtmlTree.h"

#include <string>
#include <vector>

namespace jasperengine::htmlui::compat {

    // Writes the transformed tree as RML (phase-2 §3.7.6): XML that RmlUi's parser reads,
    // element and attribute names as HTML has them (SVG case kept), values in double quotes,
    // text escaped (`& < >`), no comments, doctype or processing instructions. An element's
    // start tag begins a new line wherever that adds no text (after another tag), so RmlUi's
    // XML line numbers map back through RmlSourceMap::lineRefs.
    class RmlSerializer {
    public:
        struct HeadItem {
            enum class Kind : uint8_t { Link, Style };
            Kind kind = Kind::Link;
            std::string href;         // Link: "/@htmlui/doc/<key>/ui/menu.css.rcss"
            std::string attributes;   // Link: extra attributes (` origin="user-agent"`)
            std::string rcss;         // Style: the sheet
            SourceRef ref;            // the <link>/<style> of the HTML (or nothing for generated sheets)
            uint32_t contentLine = 0; // out: Style: the RmlUi line number of the first RCSS line
        };

        struct Document {
            std::string title;
            std::vector<HeadItem> head;
            const HtmlNode* root = nullptr;   // written as <body id="htmlui-root" ...>
            const HtmlNode* body = nullptr;   // written as <htmlbody ...>
        };

        struct Output {
            std::string rml;
            std::vector<SourceRef> lineRefs;  // RML line (1-based) - 1 -> source
        };

        // the whole RML document; fills HeadItem::contentLine
        static Output writeDocument(Document& document);
        // the children of `container` (template content, innerHTML fragments)
        static Output writeFragment(const HtmlNode& container);
        // one element with its content (inline SVG for the SVG preprocessor)
        static std::string writeElement(const HtmlNode& element);

        static std::string escapeText(const std::string& text);
        static std::string escapeAttribute(const std::string& value);
    };

}

#endif //COMPAT_HTML_RMLSERIALIZER_H
