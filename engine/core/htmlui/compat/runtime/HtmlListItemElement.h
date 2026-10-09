// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_HTMLLISTITEMELEMENT_H
#define COMPAT_RUNTIME_HTMLLISTITEMELEMENT_H

#include "Export.h"

#include <RmlUi/Core/Element.h>

#include <string>

namespace Rml {
    class ElementText;
}

namespace jasperengine::htmlui::compat {

    // <li> with its list marker (phase-2 §3.11.2, §3.11.6). The marker is a non-DOM child
    // `marker` element (author `li::marker` rules are rewritten to `li > marker`): not in
    // `children`, not counted by :first-child or :nth-child, not in the layout flow. After each
    // layout of the item it is filled (ListMarkers text from list-style-type, or an <img> for
    // list-style-image), formatted with the item's font and placed:
    // - outside (the default): its right edge at the item's content box, its top at the first
    //   line, as Chromium places it;
    // - inside: the same, with the item's padding-left grown by the marker width (Approximated:
    //   the marker is not an inline box of the first line; the padding is a local property).
    // The counter: the parent list's `start` (default 1, or the item count for `reversed`),
    // each <li value> resetting it, +1 per item (-1 reversed); nested lists count apart.
    class JASPERENGINE_INTERNAL HtmlListItemElement : public Rml::Element {
    public:
        RMLUI_RTTI_DeclareWithParent(HtmlListItemElement, Rml::Element)

        explicit HtmlListItemElement(const Rml::String& tag);
        ~HtmlListItemElement() override;

        Rml::Element* getMarker() const { return marker; }
        const std::string& getMarkerText() const { return markerText; }
        // the counter value of this item in its list
        int ordinal();

    protected:
        void OnLayout() override;
        void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;

        // the marker's text (the summary element overrides it with the disclosure triangle)
        virtual std::string markerTextFor(const std::string& listStyleType);

    private:
        void layoutMarker();

        Rml::Element* marker = nullptr;
        Rml::ElementText* markerTextElement = nullptr;
        Rml::Element* markerImage = nullptr;
        std::string markerText;
        std::string markerImageSource;
        float insideShift = 0.0f;   // px added to padding-left for list-style-position: inside
        bool markerVisible = false;
    };

}

#endif //COMPAT_RUNTIME_HTMLLISTITEMELEMENT_H
