// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R38, AC18)
// - Purpose: the small gizmo on each selected character while a text is edited on the canvas:
//   its em cell as an outline turned by the character's rotation, a move dot at its centre, a
//   rotate knob above it and a scale square at its lower right. A drag on any character's handle
//   changes every selected character by the same amount, in S80's per-character units (CharAdjust:
//   `offset` in % of the text size, y up; `rotation` degrees clockwise; `scale`).
// - Pure geometry: the window gives the glyphs (from the text layout), the layer's transform into
//   the document and the canvas transform each frame; values() is the `set` the drag writes
//   (`chars.<index>.<member>`), so a drag stages it and its release commits it as one step.
// - Thread-safety: main thread.

#pragma once

#include "TitleDesignerTypes.h"

#include "imgui.h"

#include <string>
#include <vector>

namespace jasperengine::editor::titledesigner {

    // one selected character, in the text layer's space
    struct LetterGlyph {
        int index = 0;                  // its code point index (CharAdjust key)
        TitleVec2 centre;               // its em cell's centre, with its offset and scale
        double width = 0.0, height = 0.0;
        double rotation = 0.0;          // its CharAdjust values now
        double scale = 1.0;
        TitleVec2 offset;               // % of the text size
    };

    class TitleLetterGizmo {
    public:
        enum class Part : uint8_t { None, Move, Rotate, Scale };
        struct Hit {
            Part part = Part::None;
            int glyph = -1;
        };

        // this frame's characters; `size` the text size (what offset percentages are of)
        void setGlyphs(std::vector<LetterGlyph> glyphs, const Affine2& layerToDoc, const CanvasXform& xform, double size);
        bool empty() const { return glyphs.empty(); }
        const std::vector<LetterGlyph>& selected() const { return glyphs; }

        Hit hitTest(ImVec2 screen) const;
        // a press on `hit`; values() then answers for the pointer's position
        void begin(const Hit& hit, ImVec2 screen);
        bool dragging() const { return active.part != Part::None; }
        void end() { active = Hit(); }
        // {"chars.<i>.offset": [x, y]} / rotation / scale for every selected character; `snap`
        // (Shift) turns in 15 degree steps and scales in 0.05 steps
        Json values(ImVec2 screen, bool snap) const;
        // "Move Letters", "Rotate Letters", "Scale Letters"
        std::string stepName() const;

        void draw(ImDrawList* dl, const Hit& hovered) const;

        // screen positions (tests and drawing): the cell's corners (top-left, top-right,
        // bottom-right, bottom-left), its centre, the rotate knob and the scale square
        void corners(size_t glyph, ImVec2 out[4]) const;
        ImVec2 centreOf(size_t glyph) const;
        ImVec2 knobOf(size_t glyph) const;
        ImVec2 scaleHandleOf(size_t glyph) const;

    private:
        std::vector<LetterGlyph> glyphs;
        Affine2 toDoc;
        CanvasXform xform;
        double textSize = 1.0;
        Hit active;
        ImVec2 pressed = ImVec2(0.0f, 0.0f);
        ImVec2 startCentre = ImVec2(0.0f, 0.0f);   // the grabbed cell's centre at the press
        std::vector<LetterGlyph> startGlyphs;
    };

}
