// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Unity-style colour field (docs/specs/S27-editor-tools.md R1, §6.1): a swatch (alpha as a
// checker half, an "HDR" tag on HDR fields) that opens the one Color window
// (ColorPickerWindow) on click. It edits a copy and reports the window's phases (FieldEdit.h).
// Every colour the editor edits goes through it (no ImGui::ColorEdit outside editor/widget/).

#include "widget/FieldEdit.h"

#include "imgui.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

namespace jasperengine {
    class Framebuffer;
}

namespace jasperengine::editor::ui {

    struct ColorFieldOptions {
        bool alpha = true;
        bool hdr = false;
        bool linearStorage = true;   // false: the value is sRGB as shown (ColorSRGB fields, UI tints)
        bool eyedropper = true;
        const char* label = nullptr; // the window's title and the undo step's "Change <label>"
        bool mixed = false;          // multi-selection with different values: a dash over the swatch
        float width = 0.0f;          // 0: the rest of the line
    };

    // Returns Begin on the click that opened the window on this field, Changing with `rgba` set
    // when the window changed the value, Committed / Cancelled once when its session ended
    // (Cancelled with `rgba` the original value), None otherwise.
    EditPhase colorField(const char* id, Vector4& rgba, const ColorFieldOptions& options = {});
    EditPhase colorField(const char* id, Vector3& rgb, ColorFieldOptions options = {});

    // the swatch alone (sRGB shown colour), as the field draws it
    void drawColorSwatch(ImDrawList* drawList, ImVec2 min, ImVec2 max, const Vector4& shownSrgb, bool alphaChecker, bool hdrTag, bool mixed);

    // A Scene or Game view's image this frame (min / max in ImGui coordinates of `viewport`): the
    // eyedropper reads its framebuffer at the texel under the cursor, exactly
    void registerViewImage(ImGuiID viewport, ImVec2 min, ImVec2 max, Framebuffer* framebuffer);

}
