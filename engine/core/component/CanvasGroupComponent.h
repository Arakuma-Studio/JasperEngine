// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CANVAS_GROUP_COMPONENT_H
#define CANVAS_GROUP_COMPONENT_H

#include "Export.h"
#include <cstdint>

namespace jasperengine{

    // How a group's alpha reaches its subtree (ui canvas group spec 6.2, 7.10)
    enum class CanvasGroupRenderMode{
        // Unity: each element's draw alpha is multiplied, so overlapping children show
        // through each other. No extra draw cost
        PER_ELEMENT,
        // CSS opacity / Godot CanvasGroup: the subtree is drawn into a layer that is
        // composited at the group's alpha, so overlaps do not stack. Two pass breaks and the
        // fill of its rect while the alpha is between 0 and 1
        FLATTEN
    };

    // Unity's CanvasGroup for the native UI: the GameObject it is on and all its descendants.
    // Over an element's chain (the enabled groups from the element up, stopping after the first
    // one with ignoreParentGroups) alpha multiplies and interactable / blocksRaycasts AND.
    // UISystem::syncCanvasGroups finds the changes by comparing the fields with `seen`, so
    // writers need no update flag (a raw write to the struct is seen too).
    struct JASPERENGINE_API CanvasGroupComponent{
        // component checkbox: a disabled group does not count
        bool enabled = true;
        // [0, 1], multiplied into every UI element's draw alpha (writes clamp it, NaN is rejected)
        float alpha = 1.0f;
        // false: Buttons, InputFields, Scrollbars, Panels and the on-screen controls below
        // stop reacting and show their disabled visual (Unity Selectable)
        bool interactable = true;
        // false: pointers go through the elements below to whatever is behind them
        bool blocksRaycasts = true;
        // the groups above this one stop counting
        bool ignoreParentGroups = false;
        CanvasGroupRenderMode renderMode = CanvasGroupRenderMode::PER_ELEMENT;

        // runtime, never serialized or cloned (UISystem::syncCanvasGroups)
        struct Seen{
            bool valid = false;
            bool enabled = true;
            bool interactable = true;
            bool blocks = true;
            bool ignore = false;
            float alpha = 1.0f;
            CanvasGroupRenderMode renderMode = CanvasGroupRenderMode::PER_ELEMENT;
        } seen;
        // FLATTEN: the group's effective alpha relative to the enclosing flatten layer (its
        // layer is composited at it)
        float layerAlpha = 1.0f;
        // UI elements (UIComponents) in the subtree, the group's own included (FLATTEN leaf
        // rule: one draw needs no layer)
        uint32_t subtreeDraws = 0;
        // FLATTEN: the subtree is drawn into a layer this frame (layerAlpha in (0, 1), two draws
        // or more, at most 4 nested layers)
        bool layered = false;
        // one warning per entity for a rejected NaN alpha
        bool warnedInvalidAlpha = false;
    };

}

#endif //CANVAS_GROUP_COMPONENT_H
