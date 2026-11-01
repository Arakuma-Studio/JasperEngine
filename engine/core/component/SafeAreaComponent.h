// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SAFE_AREA_COMPONENT_H
#define SAFE_AREA_COMPONENT_H

#include "Export.h"

#include <cstdint>

namespace jasperengine{

    // Drives the entity's RectTransform to the screen's safe area (Screen.safeArea), so the
    // children anchored inside it keep clear of notches, rounded corners and the home indicator
    // (the community SafeArea script of Unity projects). Put it on a full-stretch panel under
    // the canvas: backgrounds outside it still reach the screen edges. UISystem sets the four
    // anchor points each update (edges turned off stay at the reference's edge) and zeroes the
    // offsets; the RectTransform's anchors, offsets and size are driven while the component is on.
    struct JASPERENGINE_API SafeAreaComponent{
        // off (or the GameObject inactive): the RectTransform keeps the anchors last applied
        bool enabled = true;
        bool left = true;
        bool top = true;
        bool right = true;
        bool bottom = true;
        // Screen.setExtraInsets (a banner ad) counts too
        bool includeExtraInsets = true;

        // runtime, not serialized: Screen::getSafeAreaRevision() of the last applied anchors
        uint64_t appliedRevision = 0;
    };

}

#endif //SAFE_AREA_COMPONENT_H
