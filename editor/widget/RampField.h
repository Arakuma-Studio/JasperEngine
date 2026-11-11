// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R25, AC39)
// - Purpose: the inline gradient row of the Title Designer, editing S80's Gradient
//   (titleart::TitleGradient: 2..32 stops sorted by position, each with a Photoshop midpoint
//   `mid` to the next stop, and the `vivid` OKLCH blend). Photoshop's Gradient Editor in one row:
//   click the bar to add a stop with the colour there, drag stops, drag one 40 px down (or press
//   Delete) to remove it (never below 2), diamond midpoints (double-click resets to 0.5), the
//   selected stop's colour (the shared colorField), alpha and location, Reverse, Vivid, presets
//   from the swatch libraries' gradient presets. Mixed targets draw a striped bar; an edit
//   reports the whole gradient, which the glue sets on every target.
// - Design: the pure parts (sampling as S80's renderer bakes its gradient LUT, insert, remove,
//   move, midpoints, reverse, presets, hit tests) are free functions in `ramp`; RampEditor is the
//   gesture logic without ImGui (pointer and key input in bar pixels in, FieldEdit phases out);
//   rampField() only reads ImGui input, feeds RampEditor and draws. RampEditor works on its own
//   copy of the value and queues each phase with the value it carries, so the field reports one
//   phase per frame and a Begin never carries a change (FieldEdit.h).
// - Phases: a stop or midpoint drag is Begin (at the first move) .. Changing .. Committed (or
//   Cancelled on Esc / a release without a change): one undo step. A click that adds a stop, a
//   removal by Delete, a midpoint reset, Reverse, Vivid and a preset are one Applied each. The
//   colour, alpha and location fields report their own Begin .. Committed.
// - Thread-safety: main thread (ImGui); the `ramp` functions are pure.

#pragma once

#include "widget/FieldEdit.h"
#include "widget/SwatchLibrary.h"

#include "titleart/TitleArtDoc.h"

#include "imgui.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace jasperengine::editor::ui {

    namespace ramp {

        using jasperengine::titleart::TitleGradient;
        using jasperengine::titleart::TitleGradientStop;

        constexpr size_t MIN_STOPS = 2;
        constexpr size_t MAX_STOPS = 32;                    // S80 §6.1
        constexpr double MID_MIN = 0.05;                    // S80's gradient_stop.mid range
        constexpr double MID_MAX = 0.95;
        constexpr double MIN_MIDPOINT_GAP = 0.02;           // narrower gaps show no diamond
        constexpr float REMOVE_DRAG_PIXELS = 40.0f;         // drag a stop this far down to remove it
        constexpr const char* TOO_MANY_STOPS = "At most 32 stops";
        constexpr const char* TOO_FEW_STOPS = "A gradient keeps at least 2 stops";

        struct Rgba {
            double r = 1.0, g = 1.0, b = 1.0, a = 1.0;     // sRGB-encoded colour, straight alpha
        };

        // The colour and alpha at `pos` exactly as S80's renderer bakes its 256-texel gradient LUT
        // (engine/core/titleart/render/TitleLuts.cpp): stops stable-sorted by position, Photoshop
        // midpoints, straight RGB or (vivid) OKLCH on the shorter hue arc, chroma reduced to fit
        // the sRGB gamut; outside the stops the end colours
        Rgba sample(const TitleGradient& gradient, double pos);

        bool sameGradient(const TitleGradient& a, const TitleGradient& b);
        // the stops sorted by position (stable: equal positions keep their order)
        void sortStops(TitleGradient& gradient);

        // A stop at `pos` (clamped to 0..1) with the colour and alpha sampled there (midpoint
        // 0.5), after the stops at that position: its index; -1 with `error` = TOO_MANY_STOPS
        // when the gradient has MAX_STOPS
        int insertStop(TitleGradient& gradient, double pos, std::string* error = nullptr);
        // false (nothing removed) for a bad index or when only MIN_STOPS are left
        bool removeStop(TitleGradient& gradient, size_t index);
        // stop `index` moved to `pos` (clamped), placed after the stops at that position: its
        // new index; -1 for a bad index
        int moveStop(TitleGradient& gradient, size_t index, double pos);
        // the midpoint between stop `left` (in position order) and the next, clamped to
        // MID_MIN..MID_MAX and rounded to 0.001
        void setMidpoint(TitleGradient& gradient, size_t left, double mid);
        // positions p -> 1 - p and the order reversed; each midpoint moves to the mirrored gap
        // as 1 - mid (rounded to 0.001), so the gradient looks mirrored (twice is the original)
        void reverse(TitleGradient& gradient);
        // a swatch library's gradient preset (S20's Gradient: colour and alpha keys) as stops at
        // every key time, with the preset's colour (sRGB) and alpha there; `rgb` blend
        TitleGradient fromPreset(const Gradient& preset);
        // S80's JSON form ({"stops": [{"pos", "color", "alpha", "mid"}], "interp"}), for a `set` op
        jasperengine::titleart::Json toJson(const TitleGradient& gradient);

        // bar geometry: a bar from pixel x0, `width` wide
        float xAt(float x0, float width, double pos);
        double posAt(float x0, float width, float x);
        // the stop whose marker is nearest x within `radius` pixels (a tie: the later, drawn on
        // top); -1 for none
        int hitStop(const TitleGradient& gradient, float x0, float width, float x, float radius);
        // the position of the diamond between stop `left` and the next (sorted gradient)
        double midpointPos(const TitleGradient& gradient, size_t left);
        // the left stop of the diamond nearest x within `radius` (gaps of at least
        // MIN_MIDPOINT_GAP); -1 for none
        int hitMidpoint(const TitleGradient& gradient, float x0, float width, float x, float radius);

    }

    // The gesture logic of rampField, without ImGui: input in bar pixels, phases out (queued
    // with the value each carries)
    class RampEditor {
    public:
        struct Geometry {
            float x0 = 0.0f;                                // the bar's left edge
            float width = 1.0f;                             // its width
            float removeDistance = ramp::REMOVE_DRAG_PIXELS;
            float dragThreshold = 2.0f;                     // a press becomes a drag past this many pixels
        };
        struct Step {
            EditPhase phase = EditPhase::None;
            ramp::TitleGradient value;                      // the value to show once this phase is reported
        };

        // takes the caller's value while idle (no drag, nothing queued): undo and the other
        // targets' edits show up; during a gesture the editor's copy is the truth
        void sync(const ramp::TitleGradient& value);
        const ramp::TitleGradient& value() const { return working; }

        int selected() const { return selection; }
        void select(int index);
        bool dragging() const { return drag != Drag::None; }
        bool removing() const { return removed; }           // the dragged stop is past the removal distance
        // the last refusal ("At most 32 stops"), cleared by the next edit
        const std::string& warning() const { return message; }

        // pointer input (x, y in pixels)
        void pressBar(const Geometry& geometry, float x);                        // Applied: a stop added and selected
        void pressStop(const Geometry& geometry, int index, float x, float y);   // selects; the next moves drag it
        void pressMidpoint(int left, float x);                                   // the next moves drag it
        void doubleClickMidpoint(int left);                                      // Applied (to 0.5) when it was not 0.5
        void moveTo(const Geometry& geometry, float x, float y);                 // Begin past the threshold, then Changing
        void release();                                                          // Committed, or Cancelled without a change
        void cancel();                                                           // Esc: the value before the drag, Cancelled

        // commands (each one Applied when it changes the value)
        void deleteSelected();                                                   // refused at MIN_STOPS (warning)
        void reverse();
        void setVivid(bool vivid);
        void applyPreset(const ramp::TitleGradient& preset);

        // the selected stop's fields: the phases of their widgets with the new value
        void editSelectedColor(EditPhase phase, const jasperengine::titleart::TitleColor& color);
        void editSelectedAlpha(EditPhase phase, double alpha);
        void editSelectedPosition(EditPhase phase, double pos);

        // the next queued phase and its value (false when none)
        bool next(Step& out);
        bool pending() const { return !queue.empty(); }

    private:
        enum class Drag : uint8_t { None, Stop, Midpoint };

        ramp::TitleGradient working;
        ramp::TitleGradient before;                         // at the press of a drag
        std::deque<Step> queue;
        std::string message;
        int selection = 0;
        Drag drag = Drag::None;
        int dragIndex = -1;                                 // the stop (Stop) or left stop (Midpoint) in `before`
        float startX = 0.0f, startY = 0.0f;
        float grabOffset = 0.0f;                            // pointer x minus the stop's x at the press
        bool begun = false;                                 // past the threshold: Begin reported
        bool removed = false;

        void push(EditPhase phase);
        void apply(const ramp::TitleGradient& value);       // one Applied
        void clampSelection();
    };

    struct RampFieldOptions {
        const char* label = nullptr;                        // the Color window's title for a stop ("<label> Stop")
        bool mixed = false;                                 // several targets differ: a striped bar
        float height = 0.0f;                                // the bar's height; 0: the frame height
        // the presets' libraries; null: the Color window's (ColorPickerWindow::libraries())
        const std::vector<SwatchLibrary>* libraries = nullptr;
        bool presets = true;
    };

    // One phase per frame (FieldEdit.h) with `gradient` set to the value it carries
    EditPhase rampField(const char* id, jasperengine::titleart::TitleGradient& gradient, const RampFieldOptions& options = {});

    // the bar alone: the gradient over a checker, striped when mixed
    void drawRampBar(ImDrawList* drawList, ImVec2 min, ImVec2 max, const jasperengine::titleart::TitleGradient& gradient, bool mixed);

}
