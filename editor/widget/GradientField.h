// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Gradient field and the Gradient Editor (docs/specs/S27-editor-tools.md R12, §6.1, D2). The
// field is the gradient's bar (colours over a checker for alpha); a click opens the one Gradient
// Editor: Unity's layout, alpha keys above the bar and colour keys below it; a click adds a key,
// a drag moves it, dragging it off the bar deletes it; the selected key's Color (the Color
// window) or Alpha and its Location; Mode Blend or Fixed; presets from the swatch libraries'
// gradient section. At most 8 colour and 8 alpha keys. Keys are sRGB (S20's Gradient).
// Undo as the Curve window: each gesture one step (a colour key's Color window session one).

#include "widget/FieldEdit.h"
#include "widget/SwatchLibrary.h"

#include "imgui.h"
#include "math/Curve.h"
#include "particles/ParticleValues.h"

#include <string>

namespace jasperengine::editor::ui {

    struct GradientFieldOptions {
        const char* label = nullptr;
        bool mixed = false;
        float height = 0.0f;
    };

    EditPhase gradientField(const char* id, Gradient& gradient, const GradientFieldOptions& options = {});
    EditPhase minMaxGradientField(const char* id, particles::MinMaxGradient& value, const GradientFieldOptions& options = {});

    // the bar alone (sRGB keys as shown, alpha over a checker)
    void drawGradientBar(ImDrawList* drawList, ImVec2 min, ImVec2 max, const Gradient& gradient);

    class GradientWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Gradient Editor";

        static GradientWindow& get();
        static void beginEdit(CommandHistory* history, const char* label);

        void show();

        bool isOpen() const { return target != 0; }
        bool targets(ImGuiID id) const { return target != 0 && target == id; }
        void open(ImGuiID id, const Gradient& gradient, const GradientFieldOptions& options);
        EditPhase consume(ImGuiID id, Gradient& gradient);
        void close();

        // the window has the keyboard focus (App: its claim on the edit commands: it has no keys)
        bool isFocused() const { return target != 0 && windowFocused; }

    private:
        GradientWindow() = default;

        enum class Drag : uint8_t { None, Color, Alpha };

        ImGuiID target = 0;
        GradientFieldOptions options;
        std::string title;
        Gradient gradient;
        bool focusOnOpen = false;
        bool windowFocused = false;
        int lastSeenFrame = -1;
        CommandHistory* history = nullptr;
        std::string stepName;
        bool changed = false;
        bool gestureOpen = false;
        bool commitAfterDelivery = false;
        bool closeAfterDelivery = false;
        ImGuiID endedTarget = 0;

        // the selected key: a colour key (selectedColor) or an alpha key
        int selectedColor = 0;
        int selectedAlpha = -1;
        Drag drag = Drag::None;
        Gradient beforeDrag;
        int dragIndex = -1;   // the dragged key's index in beforeDrag
        float dragOffset = 0.0f;
        std::string warning;

        void beginGesture();
        void deliver(bool ends);
        void drawKeys(ImVec2 barMin, ImVec2 barMax);
        void drawSelected();
        void drawPresets();
    };

}
