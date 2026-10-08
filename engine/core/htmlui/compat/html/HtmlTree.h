// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_HTML_HTMLTREE_H
#define COMPAT_HTML_HTMLTREE_H

// The compat layer's own HTML tree (phase-2 §3.7.2): lexbor's DOM is converted into it at
// once, so nothing but HtmlParser.cpp sees a lexbor type (the swappable-core principle
// applies to the parser too). Every node keeps the source range it came from.

#include "htmlui/compat/CompatTypes.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    enum class Namespace : uint8_t { Html, Svg, MathMl };

    struct HtmlAttr {
        std::string name;        // HTML: lowercase; SVG/MathML: qualified name, case kept (viewBox, xlink:href)
        std::string value;       // decoded
        SourceRef nameRef;       // the name (invalid for attributes compat adds)
        SourceRef valueRef;      // the raw value between the quotes (invalid without a value)
        bool hasValue = false;   // false for boolean attributes written without '='
    };

    struct HtmlNode {
        enum class Kind : uint8_t {
            Element,
            Text,
            Comment,
            Doctype,
            Raw,        // pre-serialized RML/XML markup written as is (Lucide icon shapes)
        };

        Kind kind = Kind::Element;
        std::string tag;                   // HTML: lowercase local name; SVG/MathML: qualified name, case kept
        Namespace ns = Namespace::Html;
        std::vector<HtmlAttr> attrs;       // source order
        std::string text;                  // decoded text (Text/Comment), markup (Raw)
        SourceRef ref;                     // start tag `<` .. `>` (or the text); invalid when implied
        bool implied = false;              // created by the parser, not by a token of the source
        bool selfClosingSyntax = false;    // written `<tag/>` (meaningless on HTML elements that are not void)
        HtmlNode* parent = nullptr;
        std::vector<std::unique_ptr<HtmlNode>> children;
        std::unique_ptr<HtmlNode> templateContent;   // <template>: its content fragment (tag "#content")

        // set by the compile before serialization
        uint32_t loc = 0;                  // data-htmlui-loc (index into RmlSourceMap::elementRefs)
        bool hasLoc = false;
        bool hasRewrittenStyle = false;    // style="" rewritten to RCSS in rewrittenStyle
        std::string rewrittenStyle;

        static std::unique_ptr<HtmlNode> makeElement(std::string tag, Namespace ns = Namespace::Html);
        static std::unique_ptr<HtmlNode> makeText(std::string text);
        static std::unique_ptr<HtmlNode> makeRaw(std::string markup);

        bool isElement() const { return kind == Kind::Element; }
        // an HTML element with this (lowercase) tag
        bool is(std::string_view htmlTag) const { return kind == Kind::Element && ns == Namespace::Html && tag == htmlTag; }

        const HtmlAttr* findAttr(std::string_view name) const;
        HtmlAttr* findAttr(std::string_view name);
        bool hasAttr(std::string_view name) const { return findAttr(name) != nullptr; }
        // the value, "" when absent
        const std::string& attr(std::string_view name) const;
        // sets (or adds at the end) an attribute
        HtmlAttr& setAttr(std::string_view name, std::string value);
        bool removeAttr(std::string_view name);

        HtmlNode* appendChild(std::unique_ptr<HtmlNode> child);
        HtmlNode* insertChild(size_t index, std::unique_ptr<HtmlNode> child);
        std::unique_ptr<HtmlNode> removeChild(const HtmlNode* child);
        size_t indexInParent() const;      // the position in the parent's children (0 without a parent)
        // replaces this node in its parent by its children (unwrap)
        void unwrap();

        // the node's own range, or the nearest ancestor's (implied elements, generated nodes)
        SourceRef location() const;
        // concatenated text of the descendants
        std::string textContent() const;
    };

    struct HtmlDocument {
        std::unique_ptr<HtmlNode> root;    // the html element (fragments: a "#fragment" element holding the nodes)
        uint32_t file = 0;
        bool quirks = false;
        bool hasDoctype = false;
    };

}

#endif //COMPAT_HTML_HTMLTREE_H
