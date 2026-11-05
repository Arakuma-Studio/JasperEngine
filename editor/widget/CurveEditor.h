// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The curve editor widget (docs/specs/S27-editor-tools.md R8, R10): several curves on one graph,
// keys with tangent handles, wrap ghosting outside the keys, a min-max band between a curve and
// its partner, the range rect (clamped when the field asks), double-click to add a key, Delete,
// Ctrl+C / Ctrl+V, Unity's key menu, F to frame, and a bottom bar with the selected key's Time
// and Value and the wrap modes. Those keys are the catalog's curve.* commands (S61): the host runs
// them through request(), the widget reads no key itself. Each gesture (a drag, a menu action, a typed number) is reported
// as began / changed / ended, so its host makes it one undo step. The edits are curveops'.

#include "widget/CurveEditorOps.h"

#include "imgui.h"

#include <string>
#include <vector>

namespace jasperengine::editor::ui {

    struct CurveBinding {
        Curve* curve = nullptr;
        ImU32 color = 0;
        const char* label = nullptr;
        int partner = -1;        // the other curve of a min-max pair: the band between them is shaded
        bool editable = true;
        bool visible = true;
    };

    // what a frame of the editor did with the curves
    struct CurveEditorResult {
        bool began = false;      // a gesture started (the curves are as before it)
        bool changed = false;    // the curves changed
        bool ended = false;      // the gesture is done
        bool cancelled = false;  // Esc: the curves are back as the gesture found them
    };

    // Unity's names over the wrap modes (R8)
    const char* wrapModeLabel(CurveWrapMode mode);

    class CurveEditor {
    public:
        // what the catalog's curve.* commands ask of it
        enum class Command : uint8_t { Frame, Delete, Copy, Paste };

        // Unity's `ranges` (null: none)
        void setRange(const curveops::CurveRange* range);
        // the graph (size: its pixels; 0 for the rest of the region) and the bottom bar
        CurveEditorResult draw(const char* id, std::vector<CurveBinding>& curves, ImVec2 size);
        // F: the selection, or every key
        void frame(const std::vector<CurveBinding>& curves);
        void clearSelection() { selection.clear(); }
        const std::vector<curveops::KeyRef>& getSelection() const { return selection; }
        // the curve presets and keys act on: the first selected key's, else the first editable one
        int activeCurve(const std::vector<CurveBinding>& curves) const;
        // whether the command has something to act on now, and not in the middle of a drag
        bool canRun(Command command, const std::vector<CurveBinding>& curves) const;
        // runs in the next draw: an edit is that frame's gesture, a paste lands at its pointer
        void request(Command command) { requested.push_back(command); }
        size_t clipboardSize() const { return clipboard.size(); }
        const curveops::View& getView() const { return view; }

    private:
        enum class Drag : uint8_t { None, Keys, Handle, Box, Pan };

        curveops::CurveRange range;
        bool hasRange = false;
        curveops::View view;
        bool framed = false;
        std::vector<curveops::KeyRef> selection;

        Drag drag = Drag::None;
        curveops::KeyRef dragKey;
        curveops::Side dragSide = curveops::Side::Out;
        ImVec2 dragStart;
        float dragStartTime = 0.0f, dragStartValue = 0.0f;
        std::vector<Curve> originals;
        std::vector<curveops::KeyRef> originalSelection;
        bool numberEditing = false;
        // a drag's gesture was reported begun (it ends on release, or cancels on Esc)
        bool gestureOpen = false;

        std::vector<Command> requested;
        std::vector<CurveKey> clipboard;
        std::vector<curveops::KeyRef> menuSelection;

        void beginGesture(const std::vector<CurveBinding>& curves, CurveEditorResult& result);
        void restoreGesture(std::vector<CurveBinding>& curves);
        void runCommand(Command command, std::vector<CurveBinding>& curves, CurveEditorResult& result, bool hovered, float mouseX);
        void drawGrid(ImDrawList* drawList);
        void drawCurves(ImDrawList* drawList, const std::vector<CurveBinding>& curves);
        void drawKeyMenu(std::vector<CurveBinding>& curves, CurveEditorResult& result);
        void drawBottomBar(std::vector<CurveBinding>& curves, CurveEditorResult& result);
    };

}
