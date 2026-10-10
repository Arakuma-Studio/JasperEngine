// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_HTMLFIELDSETELEMENT_H
#define COMPAT_RUNTIME_HTMLFIELDSETELEMENT_H

#include "Export.h"

#include <RmlUi/Core/Element.h>

namespace jasperengine::htmlui::compat {

    // <fieldset> with a rendered legend (HTML Rendering §15.3.13; phase-2 §3.10): the legend
    // sits on the fieldset's top border. The HTML stage marks such a fieldset with the class
    // `htmlui-legend` (an .rml document's fieldset is a plain element); its first element
    // child, when that is a <legend> in normal flow, is the rendered legend:
    // - layout: the legend starts at the top of the fieldset's border box and the content
    //   follows it after the fieldset's padding-top (the legend gets the margins for that as
    //   local properties, so an author's `border` and `padding` on the fieldset work);
    // - paint: the fieldset's box is painted from the legend's middle down (the top border
    //   is centred on the legend) and the top border leaves a gap where the legend is (the
    //   fork's Element::GetBorderTopCut, FORK.md P2-14).
    // The legend is as wide as the fieldset (RmlUi has no fit-content width until phase 3);
    // the gap is as wide as its content.
    class JASPERENGINE_INTERNAL HtmlFieldsetElement : public Rml::Element {
    public:
        RMLUI_RTTI_DeclareWithParent(HtmlFieldsetElement, Rml::Element)

        explicit HtmlFieldsetElement(const Rml::String& tag);
        ~HtmlFieldsetElement() override;

        // the legend on the top border, or nullptr
        Rml::Element* getRenderedLegend();

    protected:
        void OnPropertyChange(const Rml::PropertyIdSet& changed_properties) override;
        void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;
        void OnChildAdd(Rml::Element* child) override;
        void OnChildRemove(Rml::Element* child) override;
        void OnLayout() override;
        bool GetBorderTopCut(BorderTopCut& cut) override;

    private:
        void placeLegend();

        Rml::Element* placed = nullptr;     // the legend that has our margins
        float placedTop = 0.0f;
        float placedBottom = 0.0f;
        BorderTopCut lastCut;               // what the fieldset was painted with
        bool hadCut = false;
    };

}

#endif //COMPAT_RUNTIME_HTMLFIELDSETELEMENT_H
