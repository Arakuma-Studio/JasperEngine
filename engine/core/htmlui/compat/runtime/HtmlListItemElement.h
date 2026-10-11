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
    // list-style-image; the bullets and disclosure triangles as an inline SVG shape sized by the
    // font, since the built-in fonts lack their glyphs), formatted with the item's font and placed:
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

        // Phase 2's marker emulation (the `marker` child): on while the fork lacks ListMarkers
        // (phase 3), whose ::marker draws the markers then. Elements made while it is off have no
        // marker child. Tests switch it on to check the emulation; releaseRuntime resets it.
        static bool markerEmulation();
        static void setMarkerEmulation(bool on);
        static void resetMarkerEmulation();

        Rml::Element* getMarker() const { return marker; }
        const std::string& getMarkerText() const { return markerText; }
        // the counter value of this item in its list
        int ordinal();

    protected:
        void OnLayout() override;
        void OnRender() override;
        void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;

        // the marker's text (the summary element overrides it with the disclosure triangle)
        virtual std::string markerTextFor(const std::string& listStyleType);

    private:
        void layoutMarker();

        Rml::Element* marker = nullptr;
        Rml::ElementText* markerTextElement = nullptr;
        Rml::Element* markerImage = nullptr;
        Rml::Element* markerShape = nullptr;   // bullets and triangles: an inline <svg>
        float markerShapeSize = 0.0f;          // em
        float markerShapeSide = -1.0f;         // px last set
        std::string markerShapeMarkup;
        bool markerShapeShown = true;          // shapes follow the font engine's text switch (tests)
        std::string markerText;                // the ListMarkers text
        std::string shownText;                 // what the marker's text node shows
        std::string markerImageSource;
        float insideShift = 0.0f;   // px added to padding-left for list-style-position: inside
        bool markerVisible = false;
    };

}

#endif //COMPAT_RUNTIME_HTMLLISTITEMELEMENT_H
