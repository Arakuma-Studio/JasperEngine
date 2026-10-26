// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/widgets/PopupPlacement.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine::htmlui;

namespace {

    bool vertical(PopupSide side){
        return side == PopupSide::Top || side == PopupSide::Bottom;
    }

    PopupSide opposite(PopupSide side){
        switch (side){
            case PopupSide::Top: return PopupSide::Bottom;
            case PopupSide::Bottom: return PopupSide::Top;
            case PopupSide::Left: return PopupSide::Right;
            case PopupSide::Right: return PopupSide::Left;
        }
        return PopupSide::Bottom;
    }

    // the room between the anchor and the viewport's edge (pad and gap taken off) on a side
    float roomOn(const PlaceRequest& request, PopupSide side){
        const PopupRect& a = request.anchor;
        const PopupRect& v = request.viewport;
        float room = 0.0f;
        switch (side){
            case PopupSide::Top: room = a.y - v.y; break;
            case PopupSide::Bottom: room = v.bottom() - a.bottom(); break;
            case PopupSide::Left: room = a.x - v.x; break;
            case PopupSide::Right: room = v.right() - a.right(); break;
        }
        return std::max(0.0f, room - request.gap - request.pad);
    }

    float needOn(const PlaceRequest& request, PopupSide side){
        return vertical(side) ? request.popup.height : request.popup.width;
    }

    // the popup's start along an axis, kept within [low, high - size] (the start edge wins when
    // it does not fit at all)
    float clampSpan(float start, float size, float low, float high, bool& clamped){
        const float maxStart = high - size;
        float result = start;
        if (result > maxStart)
            result = maxStart;
        if (result < low)
            result = low;
        if (result != start)
            clamped = true;
        return result;
    }

}

PlaceResult jasperengine::htmlui::placePopup(const PlaceRequest& request){
    PlaceResult result;

    PopupSide side = request.preferred;
    if (request.automatic){
        // the side with the most room for the popup's extent along it, the preferred side first
        const PopupSide order[] = {request.preferred, opposite(request.preferred),
                                   vertical(request.preferred) ? PopupSide::Right : PopupSide::Bottom,
                                   vertical(request.preferred) ? PopupSide::Left : PopupSide::Top};
        bool bestFits = false;
        float bestRoom = -1.0f;
        for (PopupSide candidate : order){
            // a side where it fits beats one where it does not, then more room wins
            const float room = roomOn(request, candidate);
            const bool fits = room >= needOn(request, candidate);
            if ((fits && !bestFits) || (fits == bestFits && room > bestRoom)){
                bestFits = fits;
                bestRoom = room;
                side = candidate;
            }
        }
    }else{
        const float room = roomOn(request, side);
        const float other = roomOn(request, opposite(side));
        if (room < needOn(request, side) && other > room){
            side = opposite(side);
            result.flipped = true;
        }
    }
    result.side = side;

    const PopupRect& a = request.anchor;
    const PopupRect& v = request.viewport;
    const float w = request.popup.width;
    const float h = request.popup.height;
    PopupRect rect;
    rect.width = w;
    rect.height = h;
    switch (side){
        case PopupSide::Top:
            rect.y = a.y - request.gap - h;
            rect.x = a.x + (a.width - w) * 0.5f;
            break;
        case PopupSide::Bottom:
            rect.y = a.bottom() + request.gap;
            rect.x = a.x + (a.width - w) * 0.5f;
            break;
        case PopupSide::Left:
            rect.x = a.x - request.gap - w;
            rect.y = a.y + (a.height - h) * 0.5f;
            break;
        case PopupSide::Right:
            rect.x = a.right() + request.gap;
            rect.y = a.y + (a.height - h) * 0.5f;
            break;
    }

    // into the viewport, on both axes (a popup that fits on neither side overlaps the anchor
    // rather than leave the screen)
    const float pad = request.pad;
    rect.x = clampSpan(rect.x, w, v.x + pad, v.right() - pad, result.clamped);
    rect.y = clampSpan(rect.y, h, v.y + pad, v.bottom() - pad, result.clamped);
    result.rect = rect;

    // the arrow points at the anchor's centre, kept off the popup's rounded corners
    if (vertical(side)){
        const float centre = a.x + a.width * 0.5f - rect.x;
        result.arrowOffset = std::clamp(centre, std::min(request.arrow, w * 0.5f), std::max(w - request.arrow, w * 0.5f));
    }else{
        const float centre = a.y + a.height * 0.5f - rect.y;
        result.arrowOffset = std::clamp(centre, std::min(request.arrow, h * 0.5f), std::max(h - request.arrow, h * 0.5f));
    }
    return result;
}

bool jasperengine::htmlui::parsePopupSide(const char* text, PopupSide& side){
    if (!text)
        return false;
    if (std::strcmp(text, "top") == 0)
        side = PopupSide::Top;
    else if (std::strcmp(text, "bottom") == 0)
        side = PopupSide::Bottom;
    else if (std::strcmp(text, "left") == 0)
        side = PopupSide::Left;
    else if (std::strcmp(text, "right") == 0)
        side = PopupSide::Right;
    else
        return false;
    return true;
}

const char* jasperengine::htmlui::popupSideName(PopupSide side){
    switch (side){
        case PopupSide::Top: return "top";
        case PopupSide::Bottom: return "bottom";
        case PopupSide::Left: return "left";
        case PopupSide::Right: return "right";
    }
    return "top";
}
