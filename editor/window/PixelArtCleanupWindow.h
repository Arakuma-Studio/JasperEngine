// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > 2D > Pixel Art Cleanup (docs/specs/S46-pixel-art-cleanup.md R42, R43): the cleanup for
// people. The window keeps a recipe the toolbar, the canvas (grid line drags, the frame boxes, the
// hand tools) and the right panel edit, and shows the session's run of it: after every edit the
// last result stays until the new one is in (R39: a run predicted over 33 ms goes to the job
// system). Recipe edits are a local undo stack until Write ("Ctrl+Z" reaches the window first, as
// Sprite Normal Map's paint does); Write and Save as Import Step are steps of the project history.
//
// Everything runs through the session layer (pixelart/cleanup/CleanupSession), so a preview is
// the pixels a write would write. Opened from the Window menu, the Project window, the Inspector
// and MCP focus_window "pixel_art_cleanup"; every entry and button calls the scripted API below.

#include "pixelart/cleanup/CleanupSession.h"
#include "pixelart/cleanup/CleanupSheetOutput.h"

#include "jobs/JobSystem.h"
#include "texture/Texture.h"

#include "imgui.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    class CommandCatalog;

    class PixelArtCleanupWindow {
        friend struct PixelArtCleanupWindowTestAccess;

    public:
        static constexpr const char* WINDOW_NAME = "Pixel Art Cleanup";
        enum class View { Source, Result, Compare };
        enum class HandTool { None, Erase, Restore, Paint, Pick };
        enum class Tab { Palette, LostColors, Cleanup, Lints, Frames };

        // What MCP (editor_status.ui.pixel_art_cleanup) and tests read of the window as it is shown
        struct UiState {
            bool open = false;
            bool focused = false;
            std::string source;              // project-relative ("" none, "(demo)" the demo)
            std::string view;                // "source", "result", "compare"
            std::string verdict;             // "clean", "fixable", "needs_person", "" while none is in
            double confidence = 0.0;
            bool running = false;
            std::string tab;                 // "palette", "lost_colors", "cleanup", "lints", "frames"
            int recipeMoved = 0;             // lines dragged by hand in the recipe
            size_t undoDepth = 0;
            float zoom = 0.0f;
            float centerX = 0.0f, centerY = 0.0f;  // source pixels at the canvas centre
            float imageMinX = 0.0f, imageMinY = 0.0f;   // screen position of source (0,0) in the view
            std::string copiedRecipe;        // the last Copy Recipe's JSON
            std::string highlight;           // what the canvas outlines now (`lost:<i>`, `spot:<lint>:<i>`), "" none
            std::vector<double> xs, ys;      // the shown grid's cuts, source pixels
            struct Spot {
                int lint = 0, spot = 0;
                double x = 0.0, y = 0.0;     // view-space centre
            };
            std::vector<Spot> spots;         // the first lint spots (at most 32), in list order
        };

        explicit PixelArtCleanupWindow(Project* project);
        ~PixelArtCleanupWindow();                      // cancels a live run

        // Once a frame, open or not: draws the window when open, adopts finished runs
        void show();
        void open();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        bool isFocused() const { return focused; }
        void requestFit() { fitRequested = true; }
        void bindCommands(CommandCatalog& catalog);
        bool takesKeys() const { return windowOpen && focused && !ImGui::IsAnyItemActive(); }
        // Another project is being opened: cancel, forget the source, the result and the undo stack
        void resetForProjectSwitch();
        // Editor shutdown: the worker is cancelled and waited for (bounded)
        void shutdownJobs(double timeoutSeconds = 2.0);

        // ---- entry points (absolute or project-relative paths); false + error when not usable

        // An image file; when it is a cleanup output, its recipe and source (R43)
        bool open(const std::filesystem::path& image, std::string& error);
        // The source file with default settings (never reads a recipe from it)
        bool openSource(const std::filesystem::path& image, std::string& error);
        // A cleanup output: re-opens its source and recipe (R43); its output path is the Output
        bool openOutput(const std::filesystem::path& output, std::string& error);
        // "Try a Demo": CleanupSynthetic's fake AI sprite; it cannot be written
        bool openDemo();
        // The project-relative source, "" none, "(demo)" the demo
        const std::string& sourcePath() const { return sourceName; }
        bool hasSource() const { return prepared != nullptr; }
        // Re-runs and writes over a cleanup output from its stored recipe, on the worker (the
        // Project window's Re-run Pixel Art Cleanup): the outcome goes to the Console or an alert
        bool startRerun(const std::filesystem::path& output, std::string& error);

        // ---- the recipe (what the UI edits)

        const pixelart::cleanup::Recipe& getRecipe() const { return recipe; }
        // One undo step: `edit` changes the recipe; a no-change edit adds no step
        void applyEdit(const std::function<void(pixelart::cleanup::Recipe&)>& edit, bool gridAffecting = false);
        // A slider drag: everything between begin and commit is one undo step (R43)
        void beginEdit();
        void commitEdit();
        // Sets the recipe as a whole (an undo step, the last argument false: when opening a source)
        void setRecipe(const pixelart::cleanup::Recipe& value, bool undoStep = true);
        bool canUndo() const { return !undoStack.empty(); }
        bool canRedo() const { return !redoStack.empty(); }
        bool undoEdit();
        bool redoEdit();
        // another history's step undone from the window: the edits' redos go (Ctrl+Y redoes that step first)
        void dropRedo() { redoStack.clear(); }
        size_t undoDepth() const { return undoStack.size(); }

        // "x:3": a grid line moved by hand, source pixels; a bent line moves as a whole (recipe.moved)
        void moveGridLine(char axis, int index, double position);
        // the same inside an open edit (a line drag)
        void setGridLine(char axis, int index, double position);

        // ---- running

        // a run of the current recipe now (no debounce): blocking tests and the footer's Write use it
        void runNow();
        bool isRunning() const { return job != nullptr; }
        // blocks until the running job has finished, without adopting its result (the next show does)
        void waitForRunningJob();
        bool hasResult() const { return shown != nullptr; }
        // the recipe changed since the shown result
        bool isStale() const;
        const pixelart::cleanup::Result* result() const { return shown ? &shown->result : nullptr; }
        const pixelart::cleanup::Prepared* getPrepared() const { return prepared.get(); }

        // ---- writing (R43): one project-history step; needs_person asks first (confirmed)

        void setOutputPath(const std::string& projectRelative) { outputPath = projectRelative; }
        const std::string& getOutputPath() const { return outputPath; }
        void setOutputFormat(pixelart::cleanup::OutputFormat format) { outputFormat = format; }
        pixelart::cleanup::OutputFormat getOutputFormat() const { return outputFormat; }
        // writes the shown result (asks through the Confirm dialog on needs_person unless `confirmed`)
        bool write(std::string& error, bool confirmed = false);
        // the confirmed replace of an existing or edited output: one write with overwrite, which never outlives it
        bool replaceOutput(std::string& error);
        // import.pixelCleanup on the source's .meta, one undo step (R33)
        bool saveAsImportStep(std::string& error, bool confirmed = false);
        const std::vector<std::string>& lastWritten() const { return written; }
        // the JSON Copy Recipe gives (the shown result's resolved recipe)
        std::string recipeJson() const;

        // ---- the canvas

        void setView(View value);
        View getView() const { return view; }
        void setTab(Tab value) { tab = value; tabRequested = true; }
        Tab getTab() const { return tab; }
        void setTool(HandTool value) { tool = value; }
        HandTool getTool() const { return tool; }
        void setFrame(int index);                      // the player's frame (and the shown source frame)
        int getFrame() const { return frame; }
        float getZoom() const { return zoom; }
        // pans and zooms so a rect of the view (a cell, in source or result pixels) is centred and
        // readable; `flash`: the 1 s flash of a lint spot, outlining that rect
        void focusOn(double x0, double y0, double x1, double y1, bool flash);
        // Lints tab: the spot's row was clicked / the arrow keys
        void selectLintSpot(int lintIndex, int spotIndex);
        // Frames tab: Alt+Arrows
        void nudgeCurrentFrame(int dx, int dy);

        UiState uiState() const;

        // Lints tab: the next (1) or previous (-1) spot, across the lints (the arrow keys' commands)
        void stepLintSpot(int step);

    private:
        struct Gpu {
            Texture texture;
            int width = 0, height = 0;
            bool valid() const { return width > 0 && !texture.empty(); }
        };

        // A finished run and what the canvas shows of it
        struct Bundle {
            pixelart::cleanup::Result result;
            pixelart::PixelImage image;     // the whole output (a sheet: its frames on one picture)
            pixelart::PixelImage compare;   // the result upscaled onto the source's cells (source size)
            std::string recipeText;         // the recipe it ran with (canonical), for isStale
            int cropX = 0, cropY = 0;       // what the crop took off the top-left, in art pixels
            std::vector<pixelart::cleanup::AtlasRect> atlasRects;  // a sheet: each exported frame on `image`
            std::map<uint32_t, int> colorCounts;   // packed RGB -> pixels of the output with it
            int opaqueCount = 0;
            double ms = 0.0;
        };
        struct Job;

        // ---- the model
        Project* project;
        std::unique_ptr<pixelart::cleanup::Prepared> prepared;
        std::shared_ptr<pixelart::cleanup::RunCache> cache;
        std::string sourceName;                     // project-relative, "(demo)"
        bool isDemo = false;
        pixelart::cleanup::Recipe recipe;           // the recipe the UI edits
        std::vector<pixelart::cleanup::Recipe> undoStack, redoStack;
        std::optional<pixelart::cleanup::Recipe> editBefore;   // an open drag's starting point
        std::shared_ptr<Bundle> shown;
        std::shared_ptr<Job> job;
        bool dirty = false;
        double lastRunMs = 40.0, lastFullRunMs = 200.0;   // prediction for R39
        std::string lastFullKey;                    // what a change of forces a full run
        std::string outputPath;
        pixelart::cleanup::OutputFormat outputFormat = pixelart::cleanup::OutputFormat::Png;
        bool outputFormatPicked = false;
        std::vector<std::string> written;
        std::string statusMessage;
        bool statusError = false;
        std::string copiedRecipe;
        // Re-run (Project window): workers writing over outputs, one per request
        struct RerunJob;
        std::vector<std::shared_ptr<RerunJob>> reruns;
        void tickReruns();

        // ---- view state
        bool windowOpen = false;
        bool focused = false;
        bool focusRequested = false;
        bool fitRequested = true;
        View view = View::Compare;
        Tab tab = Tab::Palette;
        bool tabRequested = false;
        HandTool tool = HandTool::None;
        bool showGrid = true;
        bool showLines = false;
        float zoom = 0.0f;
        ImVec2 pan = ImVec2(0, 0);
        float splitPosition = 0.5f;
        ImVec2 canvasOrigin = ImVec2(0, 0), canvasSize = ImVec2(1, 1);
        ImVec2 imageMin = ImVec2(0, 0);

        // the canvas's hover and highlights: the row hovered during the last panel draw
        int hoverLost = -1, hoverLint = -1;         // this frame's rows, the next canvas draws them
        int hoverLostShown = -1, hoverLintShown = -1;
        std::string highlightText;
        double flashUntil = 0.0;
        double flashSourceX = 0.0, flashSourceY = 0.0;
        double flashCellW = 1.0, flashCellH = 1.0;

        // canvas interaction
        bool draggingSplit = false;
        char draggingAxis = 0;                      // 'x' / 'y': a grid line being dragged
        int draggingLine = -1;
        bool painting = false;                      // a hand stroke open (one undo step)
        int draggingBox = -1;                       // a frame box being moved or resized
        int boxHandle = 0;                          // 0 move, 1..8 resize
        ImVec2 dragStartMouse = ImVec2(0, 0);
        pixelart::cleanup::FrameBox dragStartBox;
        bool addingBox = false;
        ImVec2 addStart = ImVec2(0, 0);
        bool panning = false;
        char hoveredAxis = 0;                       // the grid line under the mouse, with Lines on
        int hoveredLine = -1;
        int hoveredBox = -1, hoveredHandle = 0;     // a frame box / handle under the mouse (Frames tab)

        Gpu sourceGpu, resultGpu, compareGpu, playerGpu;
        std::string playerKey;                      // what playerGpu shows
        std::vector<Texture> retired;
        int uploadedFrame = -1;
        std::shared_ptr<Bundle> uploadedResult;

        // the Palette, Lost Colors, Cleanup, Lints and Frames tabs
        int lintSelected = -1, spotSelected = -1;
        int frame = 0;                              // the source / player frame
        int animationSelected = 0;
        bool playing = false, onionSkin = true;
        double playClock = 0.0, lastTime = -1.0;
        int playDirection = 1;
        jasperengine::pixelart::Rgba8 paintColor{0, 0, 0, 255};

        // ---- model
        void adopt(pixelart::cleanup::Prepared&& value, const std::string& name, bool demo);
        void clearAll();
        void edited(bool gridAffecting = false);
        void tick();
        void startRun(bool allowSync);
        void collectJob();
        void cancelJob();
        void uploadImages();
        void updateGpu(Gpu& gpu, const pixelart::PixelImage& image);
        void retire(Gpu& gpu);
        void releaseRetired();
        void setStatus(const std::string& text, bool error);
        void confirm(const std::string& title, const std::string& message, std::function<void()> yes);
        std::shared_ptr<Bundle> makeBundle(const std::atomic<bool>* cancel, const std::string& text, double* outMs);
        void applyBundle(std::shared_ptr<Bundle> bundle, bool full);
        const pixelart::cleanup::Sheet* currentSheet() const;
        int sourceFrameIndex() const;
        pixelart::PixelImage compareForFrame(const Bundle& bundle, int frameIndex) const;

        // coordinate maps: "view space" is source pixels in Source and Compare, output pixels in Result
        ImVec2 viewImageSize() const;
        bool resultSpace() const { return view == View::Result; }
        ImVec2 viewToScreen(double x, double y) const;
        ImVec2 screenToView(const ImVec2& p) const;
        // the source rectangle of a cell of the output (frameIndex -1: a single picture)
        bool cellRect(int frameIndex, int ax, int ay, double& x0, double& y0, double& x1, double& y1) const;
        // a cell of the output in view space (frameIndex -1: a single picture; else a sheet's frame)
        bool cellRectInView(int frameIndex, int ax, int ay, double& x0, double& y0, double& x1, double& y1) const;
        // a lint spot in view space
        bool lintSpotRect(const pixelart::lint::LintSpot& spot, double& x0, double& y0, double& x1, double& y1) const;

        // ---- UI
        void drawEmptyState(const ImVec2& area);
        void drawToolbar();
        void drawFooter();
        void drawCanvas(const ImVec2& size);
        void drawCanvasGrid(ImDrawList* draw, const ImVec2& origin, const ImVec2& area);
        void drawHighlights(ImDrawList* draw, bool canvasHovered);
        void drawFrameBoxes(ImDrawList* draw);
        void handleFrameBoxInput(bool hovered);
        void handleHandTool(bool hovered);
        void drawRightPanel();
        void drawPaletteTab();
        void drawLostColorsTab();
        void drawCleanupTab();
        void drawLintsTab();
        void drawFramesTab();
        void drawPlayer();
        void uploadPlayer();
        void acceptDrop();
        void handleCanvasInput(bool hovered, bool active, const ImVec2& origin, const ImVec2& area);
        void outlineHighlight(ImDrawList* draw, double x0, double y0, double x1, double y1, ImU32 color);

        void beginStroke(int ax, int ay, bool area);
        void addStroke(int ax, int ay);
    };

}
