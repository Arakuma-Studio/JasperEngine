// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The one Color window (docs/specs/S27-editor-tools.md R2-R5, §6.3): SV square or hue wheel, RGB
// 0-255 / RGB 0-1.0 / HSV sliders, Hexadecimal, alpha, HDR Intensity, the eyedropper and the
// swatch libraries. A floating ImGui window (an OS window with Detachable Windows). It edits the
// field it was opened on through phases (ColorField.h): opening starts the field's session,
// closing it or Enter commits one undo step, Esc cancels it, and opening it on another field
// commits the previous one first. The callback triad of OpenEngine's ColorPickerCallbacks
// (onValueChanging / onApply / onCancel) is the phase set, on linear float RGBA and stops.
// Below the swatch libraries the project's palettes are listed under "Palettes" (S43 R34): a
// click picks a colour, Add to Palette is an undoable palette write, and Snap to Palette moves
// every picked or dragged colour to the nearest colour (OKLab) of a chosen palette.

#include "widget/ColorField.h"
#include "widget/ColorMath.h"
#include "widget/SwatchLibrary.h"
#include "pixelart/Palette.h"

#include "imgui.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::ui {

    class ColorPickerWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Color";

        static ColorPickerWindow& get();

        // the glue of a field with undo, on Begin: the window's one step, "Change <label>"
        static void beginEdit(CommandHistory* history, const char* label);

        // App: every frame, after every window that draws a colour field
        void show();
        // the project whose EditorSettings/Swatches are listed (empty: none)
        void setProjectPath(const std::filesystem::path& projectPath);
        // the project whose palettes are listed under "Palettes" and snapped to (null: none)
        void setProject(Project* project);

        bool isOpen() const { return target != 0; }
        // the field drawing now is the window's (colorField)
        bool targets(ImGuiID id) const { return target != 0 && target == id; }
        // opens the window on a field (committing the previous field's session)
        void open(ImGuiID id, const Vector4& stored, const ColorFieldOptions& options);
        // what the field gets this frame (colorField); also marks the field as drawn
        EditPhase consume(ImGuiID id, Vector4& value);
        // Window > General > Color Picker: the window on its own (no field: the eyedropper, the
        // swatches and the hexadecimal to copy)
        void openStandalone();
        bool isStandalone() const { return standalone; }
        // closes as Enter does (commits) or as Esc does (cancels)
        void commit();
        void cancel();

        // the window has the keyboard focus (App: the Color Picker context and its claim on the
        // edit commands)
        bool isFocused() const { return target != 0 && windowFocused; }
        // the catalog's colorPicker.eyedropper: not with a control in use or while one is going on
        bool canEyedropper() const;
        void eyedropper();
        bool isSampling() const { return sampling || macPicking; }

        // the libraries, user ones first (reread from the files)
        const std::vector<SwatchLibrary>& libraries();
        void reloadLibraries();
        std::filesystem::path userLibraryDirectory() const;
        std::filesystem::path projectLibraryDirectory() const;

        const colormath::ColorPickerModel& model() const { return current; }

    private:
        ColorPickerWindow() = default;

        // the field the window edits (0: closed), and what it was opened with
        ImGuiID target = 0;
        ColorFieldOptions options;
        std::string title;
        colormath::ColorPickerModel current;
        // phases for the field: a change to deliver, then the end
        bool changed = false;
        EditPhase ending = EditPhase::None;
        // the field whose session just ended, and its value when the window opened
        ImGuiID endedTarget = 0;
        Vector4 endedOriginal;
        // a close waiting for the field to take the last change
        EditPhase pendingClose = EditPhase::None;
        bool cancelRequested = false;
        // the target field was drawn this frame (else its window is gone: the session commits)
        int lastSeenFrame = -1;
        bool focusOnOpen = false;
        bool windowFocused = false;
        bool standalone = false;

        colormath::SliderMode sliderMode = colormath::SliderMode::Rgb255;
        bool hueWheel = false;
        float hsvHue = 0.0f;   // kept while saturation or value is 0 (the hue of a grey)
        std::array<char, 16> hexText{};
        bool hexEditing = false;
        std::string warning;

        // the eyedropper
        bool sampling = false;
        bool macPicking = false;
        colormath::ColorPickerModel beforeSampling;
        int savedConfigFlags = 0;
        std::array<uint8_t, 81 * 3> loupe{};
        bool loupeValid = false;
        bool loupeFromView = false;
        std::string loupeMessage;

        // the swatch libraries
        std::filesystem::path projectPath;
        std::vector<SwatchLibrary> loaded;
        bool librariesLoaded = false;
        size_t activeLibrary = 0;
        int renameSwatch = -1;
        std::array<char, 64> nameText{};
        bool newLibraryPopup = false;
        bool renameLibraryPopup = false;
        bool newLibraryProject = false;

        void close(EditPhase how);
        void requestClose(EditPhase how);
        void markChanged();
        void drawPicker();
        void drawSliders();
        void drawHexAlphaIntensity();
        void drawSwatches();
        void drawLibraryMenu();
        void applySwatch(const Swatch& swatch);
        void saveActive();

        // ---- the project's palettes (R34): read-only libraries through the SwatchLibrary adapter
        struct PaletteEntry {
            std::filesystem::path file;          // absolute
            std::string path;                    // project-relative
            jasperengine::pixelart::Palette palette;
            SwatchLibrary library;
            std::filesystem::file_time_type time{};
        };
        Project* paletteProject = nullptr;
        std::vector<PaletteEntry> palettes;
        bool palettesLoaded = false;
        double palettesCheckedAt = -1.0;
        size_t activePalette = 0;
        // Snap to Palette: off by default; the palette is the project's Default Palette unless one is chosen
        bool snapToPalette = false;
        std::string snapChoice;                  // project-relative; empty: the Default Palette
        int snapIndex = -1;
        void reloadPalettes();
        void refreshPalette(PaletteEntry& entry);
        // the palette snapped to (null when there is none)
        const PaletteEntry* snapTarget();
        void snapCurrent();
        void drawSnap();
        void drawPalettes();
        void addToPalette(PaletteEntry& entry);

        void startSampling();
        void updateSampling();
        void stopSampling(bool keep);
        void drawLoupe(ImVec2 at, bool inWindow);
    };

}
