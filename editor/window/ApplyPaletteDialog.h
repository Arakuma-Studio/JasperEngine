// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Apply Palette / Reduce Colours (docs/specs/S43-palettes.md R23, R24): one texture mapped to a
// palette, or reduced to N colours, previewed before anything is written. A window of its own
// (not a modal: the editor's keys still reach it, Space flicker-compares), opened from the
// Project window (a texture's Apply Palette... / Reduce Colours...) and the Palette window's Usage.
// The preview is a before / after split at integer zoom with Nearest filtering, the statistics,
// and the lost colours (hovering one outlines it). Apply rewrites the texture's PNG (or Save As a
// new file) as one undo step; the texture's .meta, and so its GUID, is untouched. The mapping
// runs on a worker: a 1024 x 1024 image never holds the frame.

#include "jobs/JobSystem.h"
#include "pixelart/ApplyPalette.h"
#include "pixelart/PaletteAssets.h"
#include "pixelart/RgbaImage.h"
#include "window/PaletteWindow.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class CommandCatalog;
    class Project;

    class ApplyPaletteDialog {
    public:
        static constexpr const char* WINDOW_NAME = "Apply Palette";

        explicit ApplyPaletteDialog(Project* project);

        // `texture`: absolute or project-relative; `palette`: a palette path or builtin:<id> (empty:
        // the Default Palette, else the project's first); `reduce`: open on Reduce Colours
        void open(const std::filesystem::path& texture, const std::string& palette, bool reduce);
        void show();
        bool isOpen() const;
        bool isFocused() const;
        // the catalog's Palette context: the window has the keyboard and no text field does
        bool takesKeys() const;
        // Space: the After half shows Before, and back (the Palette context)
        void bindCommands(CommandCatalog& catalog);
        void resetForProjectSwitch();

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        bool focused = false;

        std::filesystem::path texture;       // absolute
        std::string paletteName;             // as resolvePaletteName takes it; empty: none chosen
        pixelart::ResolvedPalette resolved;
        std::vector<pixelart::ProjectPalette> palettes;
        bool palettesListed = false;

        // the settings (R23)
        bool reduce = false;
        int reduceColors = 16;
        int metric = 0;                      // OKLab, CIEDE2000, RGB
        int dither = 0;                      // None, Bayer 2x2, 4x4, 8x8, Floyd-Steinberg
        float ditherStrength = 1.0f;
        bool binaryAlpha = false;
        int alphaThreshold = 128;

        // the view
        int zoom = 0;                        // 0: the largest integer zoom that fits
        bool flicker = false;
        int hoveredLost = -1;
        std::string notice;
        bool noticeIsError = false;

        // the texture's pixels, read on a worker
        struct Source {
            pixelart::RgbaImage image;
            std::string error;
        };
        jobs::Task<std::shared_ptr<Source>> sourceTask;
        std::shared_ptr<Source> source;
        std::string sourceKey;               // the texture, its time and size
        std::string sourceRunningKey;
        std::string sourceShownKey;

        // the mapped picture, on a worker
        jobs::Task<std::shared_ptr<pixelart::ApplyResult>> applyTask;
        std::shared_ptr<pixelart::ApplyResult> result;
        std::string runningKey;
        std::string shownKey;

        PaletteGpuImage before;
        PaletteGpuImage after;
        std::vector<Texture> retired;

        void resolvePalette(const std::string& name);
        pixelart::ApplyRequest request() const;
        std::string requestKey() const;
        void update();
        bool resultCurrent() const;

        void drawSettings();
        void drawPreview();
        void drawStatistics();
        void drawLostColors();
        void drawButtons();
        // the mapped PNG at `target` (and the built-in's copy when it has none yet), one undo step
        bool writeResult(const std::filesystem::path& target, const std::string& what);
        void savePaletteAs();
        void setNotice(const std::string& text, bool error);
        // the pictures and their workers' results go (a close, another texture)
        void clearPictures();
    };

}
