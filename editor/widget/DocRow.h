// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Document rows (docs/specs/S82-title-designer.md R20-R23): one labelled row of a document
// field that is not an ECS component (the Title Designer's Properties, its search results; S86's
// 3D rows and later the particles reuse it). A row edits the span of targets its caller holds
// and reports what the gesture did, never writing anything itself:
// - Set: give every target `value` (typing, picking, a slider click, Paste, Reset);
// - Shift: move every target by `delta` from its own value at the gesture's start (scrubbing
//   the label or the value, arrow keys, the wheel, the - / + steppers), so rims of 2, 3 and 5
//   scrubbed by +2 become 4, 5 and 7.
// The phases are FieldEdit.h's: Begin when a drag or a wheel burst starts (the caller stages
// from then on), Changing while it runs, Committed on release or 600 ms after the last wheel
// notch, Cancelled on Esc (the caller drops what it staged), Applied for a one-shot edit.
// Scrubbing: 3 px of slop (below it a click focuses the field with its text selected), Shift
// x10, Alt x0.1 (Unity's number fields), also for the arrow keys and the wheel; the wheel edits
// only a focused field, so scrolling the panel never changes a value. Typed values may go past
// the UI range up to the hard range; past that they are clamped and the field flashes amber.
// The row menu has Reset, Copy Value, Paste Value, Copy Property Path and Expose as Parameter.
// The left gutter holds the source dot of inheriting fields (filled = set here, hollow = from
// the source) and the reset arrow sits right of the value while it differs from its default.

#include "widget/FieldEdit.h"

#include "imgui.h"

#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace jasperengine::editor::ui {

    enum class SliderMap : uint8_t { None, Linear, Log };

    // the gutter dot of an inheriting field (R23)
    enum class SourceDot : uint8_t { None, SetHere, Inherited };

    struct DocRowOptions {
        const char* label = "";
        std::string tooltip;                 // the description (range, default and path are added)
        std::string fieldPath;               // "style.rims[1].width": tooltip and Copy Property Path
        bool mixed = false;                  // the targets differ: "—", striped swatch, blank combo
        bool isDefault = true;               // equal to its default: dim label, no reset arrow
        bool canReset = true;                // has a default and is not inheriting
        SourceDot source = SourceDot::None;
        std::string sourceMenu = "Use Global Light";   // the row menu's item that returns to the source
        bool readOnly = false;
        std::string readOnlyReason;          // the disabled tooltip (R6, R71)
        bool canExpose = true;               // the menu's Expose as Parameter...
        // numbers
        double uiMin = -std::numeric_limits<double>::infinity();
        double uiMax = std::numeric_limits<double>::infinity();
        double hardMin = -std::numeric_limits<double>::infinity();
        double hardMax = std::numeric_limits<double>::infinity();
        double step = 0.0;                   // 0: from the range (or 1 for integers)
        double defaultValue = 0.0;
        bool integer = false;
        SliderMap slider = SliderMap::None;  // drawn when the UI range is finite
        const char* unit = "";               // "px", "%", "°"
        int decimals = -1;                   // -1: from the step
        float labelWidth = 0.0f;             // 0: 40 % of the row
        bool flash = false;                  // a search landing: the row flashes (R26)
    };

    struct DocRowResult {
        EditPhase phase = EditPhase::None;
        enum class Kind : uint8_t { None, Set, Shift } kind = Kind::None;
        double value = 0.0;                  // Set (numbers, bools as 0/1, enum indexes)
        double delta = 0.0;                  // Shift
        std::string text;                    // Set of a text / enum value / pasted value
        bool reset = false;                  // the reset arrow, the label's double-click or the menu
        bool toggleSource = false;           // the source dot (R23)
        bool useSource = false;              // the menu's "Use Global Light"
        bool copyPath = false;
        bool expose = false;
        bool pasted = false;                 // Paste Value (text holds the clipboard)
        bool clamped = false;                // a typed value hit the hard range
        bool hovered = false;                // the row (search hover, source legend)
        explicit operator bool() const { return phase != EditPhase::None || reset || toggleSource || useSource || expose || pasted; }
    };

    // the label column and the row's frame; returns the value column's width. Draws the source
    // dot, the label (dim at default), the tooltip and the context menu. `scrubbing` gets the
    // label's horizontal drag in pixels when the label is a scrub handle (numbers)
    struct DocRowFrame {
        float valueWidth = 0.0f;
        bool labelDoubleClicked = false;
        bool menuReset = false, menuCopyValue = false, menuPaste = false, menuCopyPath = false, menuExpose = false, menuUseSource = false;
        bool dotClicked = false;
        bool labelActive = false;            // the label is being dragged
        float labelDragX = 0.0f;             // since the press
        bool labelReleased = false;
    };

    // a number (float or integer): scrub, type, slider, steppers, wheel, arrows
    DocRowResult docRowNumber(const char* id, double value, const DocRowOptions& options);
    // a checkbox (mixed: indeterminate)
    DocRowResult docRowBool(const char* id, bool value, const DocRowOptions& options);
    // a combo of `labels` (mixed or index < 0: blank)
    DocRowResult docRowEnum(const char* id, int index, const std::vector<std::string>& labels, const DocRowOptions& options);
    // a single-line text (Enter or focus loss applies); multiLine: Enter = new line, Ctrl+Enter = done
    DocRowResult docRowText(const char* id, const std::string& value, const DocRowOptions& options, bool multiLine = false,
                            float height = 0.0f);
    // a value drawn by `drawValue` in the value column (colours, ramps, curves, assets); the
    // callback returns its own result, the frame adds reset, menu and the source dot
    DocRowResult docRowCustom(const char* id, const DocRowOptions& options, const std::function<DocRowResult(float width)>& drawValue);
    // a link row of the search (works only on its page): the label and a button
    bool docRowLink(const char* id, const DocRowOptions& options, const char* buttonText);
    // the one-line legend above the first inheriting row ("● set here ○ from Global Light")
    void docRowSourceLegend(const char* sourceName);

    // formatting shared with the search results and readouts
    std::string formatDocNumber(double value, const DocRowOptions& options);
    // the value a slider position 0..1 maps to and back (linear or log), within the UI range
    double sliderToValue(double t, const DocRowOptions& options);
    double valueToSlider(double value, const DocRowOptions& options);
    // the step of a number row (options.step, else integers 1, else 1 % of the UI range)
    double docRowStep(const DocRowOptions& options);
    // the scrub delta of a mouse drag of `pixels` with modifiers (Shift x10, Alt x0.1)
    double scrubDelta(float pixels, const DocRowOptions& options, bool shift, bool alt);
    // clamps a typed value to the hard range; `clamped` set when it moved
    double clampTyped(double value, const DocRowOptions& options, bool& clamped);
    // "Clamped to 2,000 px (the maximum)"
    std::string clampMessage(double value, const DocRowOptions& options);

    // the shift-each gesture's result for one target (R22)
    inline double shiftEach(double startValue, double delta, const DocRowOptions& options) {
        bool clamped = false;
        return clampTyped(startValue + delta, options, clamped);
    }

    // the time a wheel burst stays one step, and the scrub slop
    constexpr double DOC_ROW_WHEEL_BURST_SECONDS = 0.6;
    constexpr float DOC_ROW_SCRUB_SLOP = 3.0f;
    constexpr double DOC_ROW_CLAMP_FLASH_SECONDS = 0.6;

    // tests: the clock the wheel bursts and flashes use (null: ImGui's time)
    void setDocRowClockForTesting(std::function<double()> clock);

}
