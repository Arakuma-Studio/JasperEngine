// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Curve field and the Curve window (docs/specs/S27-editor-tools.md R9-R11, §6.1). The field
// is a thumbnail of the curve in the field's colour over its range, its polyline cached and
// rebuilt only when the curve changes; a click opens the one Curve window (retargeted like the
// Color window), right-click offers Copy, Paste, Reset and Presets. The min-max field adds
// Unity's mode dropdown and the inline constants; its curve modes open the window with both
// curves and the band between them. The field opens on click as OpenEngine's CurveField does
// (Apps/Editor/Source/UI/Controls/CurveField.cpp, MIT, NOTICE.md).
//
// Undo: every gesture in the window (a drag, a menu action, a typed number, a preset) is one
// step. On Begin the glue tells the window where the steps go (CurveWindow::beginEdit); the
// window opens a session per gesture and the glue writes the Changing values into it.

#include "widget/CurveEditor.h"
#include "widget/CurvePresets.h"
#include "widget/FieldEdit.h"

#include "imgui.h"
#include "particles/ParticleValues.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::ui {

    struct CurveFieldOptions {
        curveops::CurveRange range;   // the thumbnail's frame and the window's range (clamped when range.clamp)
        ImU32 color = 0;              // 0: green
        float height = 0.0f;          // 0: a frame's height
        const char* label = nullptr;  // the window title and the undo steps' "Change <label>"
        bool mixed = false;           // multi-selection with different curves
        const Curve* resetValue = nullptr;   // the menu's Reset (null: a line across the range)
    };

    EditPhase curveField(const char* id, Curve& curve, const CurveFieldOptions& options = {});
    // `random`/`multiplier` are the particle value's; the field edits all of it
    EditPhase minMaxCurveField(const char* id, particles::MinMaxCurve& value, const CurveFieldOptions& options = {});

    // the thumbnail alone: a polyline cached by `cacheId` until the curves change
    void drawCurveThumbnail(ImGuiID cacheId, ImDrawList* drawList, ImVec2 min, ImVec2 max, const Curve& curve, const Curve* second,
                            const curveops::CurveRange& range, ImU32 color);

    class CurveWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Curve";

        static CurveWindow& get();
        // the glue of a field with undo, on Begin: each gesture is one step in `history`
        static void beginEdit(CommandHistory* history, const char* label);

        void show();
        void setProjectPath(const std::filesystem::path& projectPath);

        bool isOpen() const { return target != 0; }
        bool targets(ImGuiID id) const { return target != 0 && target == id; }
        // opens on a field's curves (one, or a min-max pair)
        void open(ImGuiID id, const std::vector<Curve>& curves, const CurveFieldOptions& options);
        // what the field gets this frame (`curves` in: the field's value, out: the window's)
        EditPhase consume(ImGuiID id, std::vector<Curve>& curves);
        void close();

        // the window the opened field is drawn in (its top-level window's name)
        bool openedFrom(const char* windowName) const { return target != 0 && opener == windowName; }
        // a playhead on the graph for this frame (the opener's clock on the curve's time axis, S91
        // R23): a line, and Paste at Playhead in the menus; not set in a frame, it goes
        void setPlayhead(float time);
        // the window has the keyboard focus (App: the Curve context and its claim on the edit commands)
        bool isFocused() const { return target != 0 && windowFocused; }
        // the catalog's curve.* commands
        bool canRun(CurveEditor::Command command);
        void run(CurveEditor::Command command);
        // what editor_status shows of it
        struct UiState {
            bool open = false;
            bool focused = false;
            size_t selectedKeys = 0;
            size_t clipboardKeys = 0;
            curveops::View view;
        };
        UiState uiState() const;

        // built-in first, then user and project libraries
        const std::vector<CurvePresetLibrary>& libraries();
        void reloadLibraries();
        std::filesystem::path userLibraryDirectory() const;
        std::filesystem::path projectLibraryDirectory() const;

    private:
        CurveWindow() = default;

        // the opened field
        ImGuiID target = 0;
        std::string opener;
        int playheadFrame = -1;
        CurveFieldOptions options;
        bool twoCurves = false;
        std::string title;
        std::vector<Curve> curves;
        bool focusOnOpen = false;
        bool windowFocused = false;
        int lastSeenFrame = -1;
        CurveEditor editor;
        CommandHistory* history = nullptr;
        std::string stepName;
        // a gesture's value waiting for the field, and the session to commit after it
        bool changed = false;
        bool gestureOpen = false;
        bool commitAfterDelivery = false;
        bool closeAfterDelivery = false;
        // the field whose session (or window) just ended: Committed once
        ImGuiID endedTarget = 0;

        std::filesystem::path projectPath;
        std::vector<CurvePresetLibrary> loaded;
        bool librariesLoaded = false;
        size_t activeLibrary = 0;
        std::string warning;

        std::vector<CurveBinding> bindings();
        void drawPresets();
        void beginGesture();
        void endGesture(bool cancelled);
    };

}
