// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_WIDGETS_POPUPPLACEMENT_H
#define HTMLUI_WIDGETS_POPUPPLACEMENT_H

// Where a popup (a tooltip) goes next to its anchor (S17 §6.3): on the preferred side, flipped
// to the opposite one when the preferred side has less room than the popup needs and the
// opposite side has more, then clamped into the viewport with an edge pad. A pure function of
// rectangles in one coordinate space (document pixels), so it is tested without RmlUi. The
// rules are OpenEngine's dropdown placement (DropdownPopupPlacement.cpp, MIT; see NOTICE.md),
// generalised to four sides with an arrow offset.

#include "Export.h"

#include <cstdint>

namespace jasperengine::htmlui {

    enum class PopupSide : uint8_t { Top, Bottom, Left, Right };

    struct PopupRect {
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;

        float right() const { return x + width; }
        float bottom() const { return y + height; }
    };

    struct PopupSize {
        float width = 0.0f, height = 0.0f;
    };

    struct PlaceRequest {
        PopupRect anchor;                    // the source's border box (a pointer: a 1x1 rectangle)
        PopupSize popup;                     // the popup's border box
        PopupRect viewport;                  // where the popup may be (the document's viewport)
        PopupSide preferred = PopupSide::Top;
        // "auto": the side with the most room, the preferred side breaking ties
        bool automatic = false;
        float gap = 6.0f;                    // between anchor and popup (the arrow sits in it)
        float pad = 4.0f;                    // the clearance kept from the viewport's edges
        float arrow = 0.0f;                  // the arrow's size: its offset stays this far from the popup's corners
    };

    struct PlaceResult {
        PopupRect rect;                      // the popup's border box
        PopupSide side = PopupSide::Top;     // where it went
        // the arrow's centre along the popup's edge facing the anchor (x for Top/Bottom, y for
        // Left/Right), from the popup's left/top: it points at the anchor's centre while the
        // popup is clamped away from it
        float arrowOffset = 0.0f;
        bool flipped = false;                // the opposite side of the preferred one
        bool clamped = false;                // moved to stay inside the viewport
    };

    JASPERENGINE_API PlaceResult placePopup(const PlaceRequest& request);

    // "top" / "bottom" / "left" / "right"; false for anything else ("auto" is handled by the caller)
    JASPERENGINE_API bool parsePopupSide(const char* text, PopupSide& side);
    JASPERENGINE_API const char* popupSideName(PopupSide side);

}

#endif //HTMLUI_WIDGETS_POPUPPLACEMENT_H
