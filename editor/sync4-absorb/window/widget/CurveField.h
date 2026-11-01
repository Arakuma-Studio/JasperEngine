// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's curve field (lines and trails spec 6.1, "Editor widgets"; particles spec 6.8):
// an inline preview of an AnimationCurve that opens Unity's popup curve editor. The pure part
// (view mapping, hit testing, key edits, tangent modes, presets) is CurveFieldLogic.h.
//
// Contract: call every frame the row is drawn, with the property's current value. `changed` is
// true on every frame `curve` was modified (each frame of a drag); `finished` is true when one
// edit ends (the mouse released after a drag, and on every discrete action: add / delete key,
// preset, wrap combo, tangent mode, a numeric field deactivated): the caller commits one undo
// step then. The curve is kept valid (normalize()). The popup's UI state (selection, view,
// Free flags) is kept per ImGui ID. Main thread.

#pragma once

#include "math/AnimationCurve.h"

namespace jasperengine::editor {

    struct CurveFieldResult{
        bool changed = false;    // `curve` was modified this frame
        bool finished = false;   // one edit (a drag, a popup action) ended: commit an undo step
    };

    // Inline preview of `curve` (a framed box the row's width, -1: the available width): the
    // curve drawn over its key range, the value range shown on hover; an em dash when `mixed`.
    // Clicking it opens the popup editor: drag keys, double-click empty space adds a key,
    // Delete / Backspace or the context menu deletes the selected key, tangent handles on the
    // selected key, per-key Auto / Linear / Constant / Free, presets, Fit, pre / post wrap,
    // numeric Time / Value of the selected key
    CurveFieldResult curveField(const char* id, AnimationCurve& curve, bool mixed, float width = -1.0f);

    // The same with a second curve in one editor (Unity's "Random Between Two Curves": `curve`
    // the upper, `second` the lower one, the band between them shaded); both editable, the edit
    // goes to the curve of the selected key. `second` null: the one-curve field
    CurveFieldResult curveField(const char* id, AnimationCurve& curve, AnimationCurve* second, bool mixed, float width = -1.0f);

}
