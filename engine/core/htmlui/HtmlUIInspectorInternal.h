// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUIINSPECTORINTERNAL_H
#define HTMLUIINSPECTORINTERNAL_H

// What HtmlUIInspector and HtmlUIAssertions share: the document of an entity, selectors, refs,
// boxes, clip rects and hit tests over RmlUi elements. Engine-internal (RmlUi types).

#include "htmlui/HtmlUIInspector.h"

#include <RmlUi/Core/Box.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rml {
    class Element;
    class ElementDocument;
}

namespace jasperengine {
    class HtmlUIContext;
    class UIDocumentSystem;
}

namespace jasperengine::htmlui::compat {
    struct CompatDocument;
}

namespace jasperengine::htmlui::inspect {

    // The live document of an entity, laid out
    struct Doc {
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        UIDocumentSystem* system = nullptr;
        HtmlUIContext* context = nullptr;          // its texture context, else the scene's
        Rml::ElementDocument* document = nullptr;  // the RmlUi document element (an HTML document's <html>)
        Rml::Element* body = nullptr;              // <body> (RML htmlbody); the document itself for .rml
        bool compat = false;                       // loaded through the compat layer (.html)
        std::shared_ptr<const compat::CompatDocument> compiled;
        std::string path;                          // asset-relative document path
        float width = 0, height = 0;               // viewport
    };

    // Resolves the entity's document and brings its layout up to date; false + error when the
    // entity has no UI Document or it is not loaded
    bool open(Scene* scene, Entity entity, Doc& doc, std::string& error);

    // Elements matching a CSS selector (an .rml document: an RCSS selector) in document order,
    // the <html> root included; "" = <body>. SVG internals are never matched. An invalid
    // selector is an error.
    bool select(const Doc& doc, const std::string& selector, std::vector<Rml::Element*>& out, std::string& error);
    // the selector in the document's own terms (compat: CSS names rewritten to RML); false when invalid
    bool rmlSelector(const Doc& doc, const std::string& selector, std::string& out, std::string& error);

    // element nodes (not text), and their element children (none below an <svg>)
    bool isElement(const Rml::Element* element);
    bool isLeaf(const Rml::Element* element);
    void elementChildren(Rml::Element* element, std::vector<Rml::Element*>& out);
    // ancestor is element or one of its ancestors
    bool isInclusiveAncestor(const Rml::Element* ancestor, const Rml::Element* element);

    // DOM tag name: "html", "body", "template" for the RML stand-ins
    std::string tagOf(const Doc& doc, const Rml::Element* element);

    // Unique selectors of a document's elements (InspectElement::ref); caches what the refs of
    // one query share (id counts, tags that select themselves)
    class Refs {
    public:
        explicit Refs(const Doc& doc);
        std::string of(Rml::Element* element);

    private:
        bool uniqueId(Rml::Element* element);
        bool tagSelects(Rml::Element* element);
        const Doc& doc;
        std::unordered_map<std::string, int> idCounts;
        std::unordered_map<std::string, bool> tagChecks;
        std::unordered_map<Rml::Element*, std::string> cache;
    };

    // a box area of the main box, through the element's CSS transforms (bounding rect)
    InspectRect boxRect(Rml::Element* element, Rml::BoxArea area);
    // a layout-space rect of the element (its text lines) through its CSS transforms (bounding rect): the
    // space boxRect and the clip regions are in
    InspectRect transformedRect(Rml::Element* element, const InspectRect& rect);
    // the border box: every box of the element (an inline split over lines) united
    InspectRect borderRect(Rml::Element* element);

    InspectRect intersect(const InspectRect& a, const InspectRect& b);
    InspectRect unite(const InspectRect& a, const InspectRect& b);
    bool isEmpty(const InspectRect& rect);   // width or height <= 0
    bool contains(const InspectRect& outer, const InspectRect& inner, float tolerance);

    // The region the ancestors' overflow clipping leaves visible for an element, as RmlUi's
    // ElementUtilities::GetClippingRegion finds it (containing-block chain, `clip`, scrollbars,
    // `overflow: clip` per axis), in floats. bounded = false: nothing clips it. `skip`
    // ignores some clipping ancestors (TextFits allow_scroll).
    struct ClipRegion {
        bool bounded = false;
        InspectRect rect;
        std::vector<std::pair<Rml::Element*, InspectRect>> clippers;   // innermost first
    };
    ClipRegion clipOf(Rml::Element* element, const std::function<bool(Rml::Element*)>& skip = {});
    // the innermost clipper whose region does not hold `rect` (null when every one does)
    Rml::Element* clipperCutting(const ClipRegion& clip, const InspectRect& rect, float tolerance);

    // the element itself or the nearest ancestor with display: none (null when none)
    Rml::Element* displayNoneAt(Rml::Element* element);
    // product of the opacities of the element and its ancestors
    float effectiveOpacity(Rml::Element* element);

    InspectFlags flagsOf(const Doc& doc, Rml::Element* element);
    // index/parent/depth are left to the caller
    InspectElement describe(const Doc& doc, Refs& refs, Rml::Element* element);
    // the direct text children's text, white space collapsed (untruncated)
    std::string ownText(Rml::Element* element);

    // The DOM element that receives the pointer at (x, y): among this document's elements only
    // (wholeContext = false), or the context's topmost, which may be in another document. Null
    // when the point falls through to the game. A page that paints no background passes the
    // pointer on to the documents below (HtmlUIContext::passesPointerThrough).
    Rml::Element* hitAt(const Doc& doc, float x, float y, bool wholeContext);
    // the hit and its ancestors up to <body> (or <html> when it is the hit), topmost first
    void hitStack(const Doc& doc, Rml::Element* hit, std::vector<Rml::Element*>& out);

    // "12", "12.5", "0.33": at most `decimals` decimals, no trailing zeros
    std::string number(double value, int decimals = 2);

}

#endif //HTMLUIINSPECTORINTERNAL_H
