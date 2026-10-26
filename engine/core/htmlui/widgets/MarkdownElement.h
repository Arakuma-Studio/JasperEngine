// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_WIDGETS_MARKDOWNELEMENT_H
#define HTMLUI_WIDGETS_MARKDOWNELEMENT_H

// <markdown> at runtime (S17 R24-R26). An HTML document's <markdown> is rendered by the compile
// (compat ElementTransform: data-htmlui-markdown marks it); this element renders what changes
// afterwards: a new src="" (setAttribute, or an .rml document's src at its first update), and
// el:setMarkdown(). The data-markdown="expr" data view renders the expression's text into any
// element. Images resolve as the document's <img src> do; HTML in the Markdown is shown as text.

#include "Export.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>

#include <string>
#include <string_view>

namespace Rml {
    class DataViewInstancer;
}

namespace jasperengine::htmlui {

    class JASPERENGINE_INTERNAL MarkdownElement : public Rml::Element {
    public:
        RMLUI_RTTI_DeclareWithParent(MarkdownElement, Rml::Element)

        explicit MarkdownElement(const Rml::String& tag);

        // the RML of `markdown` for an element of `document` (images resolved against it);
        // rawHtml: the Markdown held HTML markup (shown as text)
        static std::string render(Rml::Element* element, std::string_view markdown, bool* rawHtml = nullptr);
        // `element`'s content becomes the rendered Markdown (el:setMarkdown, any element)
        static void setMarkdown(Rml::Element* element, std::string_view markdown);

        // the element and the data-markdown view (WidgetsRuntime::registerAll)
        static void registerInstancers();

    protected:
        void OnAttributeChange(const Rml::ElementAttributes& changed) override;
        void OnUpdate() override;

    private:
        // reads src (against the document) and renders it
        void loadSource();

        bool loadPending = false;
    };

    class JASPERENGINE_INTERNAL MarkdownInstancer : public Rml::ElementInstancer {
    public:
        Rml::ElementPtr InstanceElement(Rml::Element* parent, const Rml::String& tag, const Rml::XMLAttributes& attributes) override;
        void ReleaseElement(Rml::Element* element) override;
    };

}

#endif //HTMLUI_WIDGETS_MARKDOWNELEMENT_H
