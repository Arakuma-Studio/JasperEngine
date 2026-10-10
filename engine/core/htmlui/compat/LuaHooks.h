// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_LUAHOOKS_H
#define COMPAT_LUAHOOKS_H

// The compat side of the element API (phase-2 §3.12.4, hook H5): what UIElement / UIDocument
// (C++ and Lua) do differently for documents the compat layer loaded, so scripts see the HTML
// they wrote and not its RML: selectors through the CSS rewriter, DOM tag names, the hidden
// data-htmlui-* attributes and htmlui-* classes, innerHTML/outerHTML as HTML, style setters
// through the declaration rewriter, Lucide icons set at runtime, template.content.
// Engine-internal (RmlUi types).

#include "htmlui/compat/Diagnostics.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Rml {
    class Element;
    class ElementDocument;
}

namespace jasperengine::htmlui::compat {

    class LuaHooks {
    public:
        // `document` is a document the compat layer loaded (its RML body is #htmlui-root)
        static bool isCompat(const Rml::ElementDocument* document);

        // querySelector(All), matches, closest: CSS names -> RML names (CssRewriter::rewriteSelector)
        static std::string selector(std::string_view css);

        // DOM names: htmlbody -> body, the RML body -> html, htmltemplate -> template
        static std::string localName(const Rml::Element* element);
        // tagName: the local name, upper case for HTML elements (svg stays as written)
        static std::string tagName(const Rml::Element* element);

        // what scripts do not see
        static bool isHiddenAttribute(std::string_view name);   // data-htmlui-*
        static bool isHiddenClass(std::string_view name);       // htmlui-*
        static std::string visibleClassNames(const std::string& classNames);
        // a class list a script sets, plus the hidden classes the element has now
        static std::string withHiddenClasses(const std::string& visible, const std::string& current);

        // innerHTML / outerHTML getters: HTML (the DOM names, without what is hidden)
        static std::string innerHTML(Rml::Element* element);
        static std::string outerHTML(Rml::Element* element);
        // HTML for innerHTML / outerHTML / insertAdjacentHTML setters -> RML; the diagnostics are
        // logged and recorded with the document (chunk: "<document> (innerHTML)")
        static std::string fragmentToRml(std::string_view html, Rml::ElementDocument* document, const std::string& what);

        // style[prop] = value, setProperty, cssText: the CSS rewritten into RCSS declarations
        static std::vector<std::pair<std::string, std::string>> rewriteDeclarations(const std::string& css);

        // setAttribute("data-lucide", name): the RML of the <svg> the element becomes (its
        // visible attributes merged as lucide.createIcons() does); "" when the icon is unknown
        static std::string lucideSvg(const Rml::Element* element, const std::string& name, std::string* shapes = nullptr,
                                     std::vector<std::pair<std::string, std::string>>* attributes = nullptr);

        // template.content: the RML of a template placeholder's content ("" when none)
        static std::string templateContent(const Rml::Element* placeholder);
        static constexpr const char* kTemplateAttribute = "data-htmlui-template";
        static constexpr const char* kFragmentTag = "htmlfragment";   // what template.content returns

        // document.body: the htmlbody of a compat document (null otherwise)
        static Rml::Element* body(Rml::ElementDocument* document);
    };

}

#endif //COMPAT_LUAHOOKS_H
