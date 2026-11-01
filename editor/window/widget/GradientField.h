// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's gradient field (lines and trails spec 6.1, "Editor widgets"; particles spec
// 6.8): an inline bar of a Gradient that opens Unity's popup gradient editor. The pure part
// (marker hit testing, key add / move / remove, presets) is GradientFieldLogic.h.
//
// Contract: call every frame the row is drawn, with the property's current value. `changed` is
// true on every frame `gradient` was modified (each frame of a drag); `finished` is true when
// one edit ends (the mouse released after a drag, and on every discrete action: add / remove
// key, preset, mode, a colour / alpha / location field deactivated): the caller commits one
// undo step then. Colours are shown and picked in sRGB and stored linear. The gradient is kept
// valid (normalize()). The popup's UI state (selection) is kept per ImGui ID. Main thread.

#pragma once

#include "math/Gradient.h"

namespace jasperengine::editor {

    struct GradientFieldResult{
        bool changed = false;    // `gradient` was modified this frame
        bool finished = false;   // one edit (a drag, a popup action) ended: commit an undo step
    };

    // Inline bar (a checkerboard behind for alpha; an em dash over it when `mixed`), the row's
    // width (-1: the available width). Clicking opens the popup: alpha markers above the bar,
    // colour markers below; click a marker strip to add a key, drag to move, Delete / Backspace
    // or the button removes the selected key (not the last one of its list); the colour (sRGB)
    // or alpha of the selected key, its Location (%), the Blend / Fixed mode, presets
    GradientFieldResult gradientField(const char* id, Gradient& gradient, bool mixed, float width = -1.0f);

}
