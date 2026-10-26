// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_TREERECONCILE_H
#define COMPAT_TREERECONCILE_H

// The live tree patch of HTML documents (S17 R27-R29): the RML of the compile the live document
// came from (the common ancestor) and the RML of the new compile, read back as trees, diffed into
// a plan that UIDocumentSystemPatch.cpp applies to the live document. Pure (no RmlUi), so it is
// tested on its own.
//
// Matching (OpenEngine's UIHotReload, MIT; see NOTICE.md): per parent, a new child takes the old
// child with its id, else the first unmatched old child of its tag whose classes are the same, then
// one whose classes hold the new ones, then one of its tag (looking after the previous match
// first, so the order is kept); text nodes match by their position among the text nodes. Merge
// (3-way): an attribute, a class token, an inline-style property or a text takes the new value only
// where the old and the new compile differ: what a script changed live stays unless the author
// changed it too. An element whose tag or data binding (data-model, data-for, data-if, ..., a
// {{ }} text) changed is replaced as a subtree, as are an inline <svg> and a data-for template
// whose markup changed. Old nodes without a match are removed, new ones inserted.

#include "Export.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace jasperengine::htmlui::compat {

    // A node of compiled RML as RmlUi's parser reads it: elements with their attributes (entities
    // decoded) and one text node per run of character data between two tags, white space included.
    struct JASPERENGINE_API RmlNode {
        enum class Kind : uint8_t { Element, Text };

        Kind kind = Kind::Element;
        std::string tag;
        std::vector<std::pair<std::string, std::string>> attributes;   // in source order
        std::string text;                  // Text: the decoded text
        std::string rml;                   // Element: its RML (start tag to end tag), as written
        int loc = -1;                      // data-htmlui-loc
        std::vector<RmlNode> children;

        bool isElement() const { return kind == Kind::Element; }
        // nullptr when absent
        const std::string* attribute(std::string_view name) const;
        // the RML of the children
        std::string innerRml() const;
    };

    // Reads the element that starts at the first `<tag` of `rml` (the document's <body>, a
    // template's fragment: wrap it first). False with `error` set on markup it cannot read.
    JASPERENGINE_API bool parseRmlElement(std::string_view rml, std::string_view tag, RmlNode& out, std::string* error = nullptr);
    // Every top-level node of a fragment (a template's content)
    JASPERENGINE_API bool parseRmlFragment(std::string_view rml, std::vector<RmlNode>& out, std::string* error = nullptr);

    // "a: b; c: d" -> (a, b), (c, d), in order (the inline style of an element)
    JASPERENGINE_API std::vector<std::pair<std::string, std::string>> parseDeclarations(std::string_view style);

    struct JASPERENGINE_API ReconcilePlan {
        // one new child of a matched element, in the new order
        struct Child;

        // an old child without a match
        struct Removed {
            bool text = false;
            int loc = -1;                       // an element: its data-htmlui-loc
            int textIndex = -1;                 // a text: its index among the parent's text nodes
        };

        // a matched pair: the old element (its live counterpart) becomes the new one. The plan
        // points into the new tree, which must outlive it
        struct Element {
            int oldLoc = -1;
            int newLoc = -1;
            std::string tag;
            const RmlNode* node = nullptr;      // the new element
            // replaced as a subtree by the new element's RML, with the cause
            bool replace = false;
            std::string reason;
            // the authored changes (3-way)
            std::vector<std::pair<std::string, std::string>> setAttributes;
            std::vector<std::string> removeAttributes;
            std::vector<std::string> addClasses;
            std::vector<std::string> removeClasses;
            std::vector<std::pair<std::string, std::string>> setStyles;   // value "" removes
            // its content is the engine's (a <markdown>): replaced by contentRml when it changed
            bool leaf = false;
            bool content = false;
            std::string contentRml;
            // the children (not for a leaf or a replace)
            std::vector<Child> children;
            std::vector<Removed> removedChildren;

            bool changed() const;
        };

        struct Child {
            enum class Kind : uint8_t { Element, Text, Insert };
            Kind kind = Kind::Element;
            const RmlNode* node = nullptr;      // the new node (Insert: its RML goes in)
            int oldIndex = -1;                  // Element, Text: the old child's index in its parent
            int textIndex = -1;                 // Text: the old child's index among the parent's text nodes
            bool textChanged = false;
            std::string text;                   // Text: the new text (decoded)
            std::vector<Element> element;      // Element: the plan of the pair (one)
        };

        Element root;
        // old loc -> new loc of every matched element, template contents and data-for content included
        std::unordered_map<int, int> locs;
        // new template index -> its content's markup changed (beyond renumbering)
        std::unordered_set<int> changedTemplates;
        // old template index -> new one
        std::unordered_map<int, int> templates;

        int kept = 0;          // matched elements without an authored change
        int updated = 0;       // matched elements with one
        int inserted = 0;
        int removed = 0;
        int replaced = 0;
        std::vector<std::string> causes;   // why each replace happened ("<tag#id>: data-for changed")
    };

    // Old and new: the document's <body> (the RML body, data-htmlui-loc 0) and each template's content.
    JASPERENGINE_API ReconcilePlan reconcileTrees(const RmlNode& oldRoot, const RmlNode& newRoot,
                                                  const std::vector<std::vector<RmlNode>>& oldTemplates,
                                                  const std::vector<std::vector<RmlNode>>& newTemplates);

}

#endif //COMPAT_TREERECONCILE_H
