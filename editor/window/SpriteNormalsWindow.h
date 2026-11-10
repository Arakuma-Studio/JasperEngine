// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > Sprite Normal Map: a normal map for a pixel-art sprite, seen lit before anything is
// written. Live: every change of the settings, the frames or the source regenerates the preview
// on a worker 150 ms after the last change (spritenormals::SpriteNormalsRun); Save writes the
// shown result with its recipe as one Project undo step, Save & Assign also sets it as the
// Sprite's Normal Texture as one Scene undo step (spritenormals::applyToSprite). Batch mode: one
// map per image, the same settings, Save All as one undo step.
//
// Opened from the Window menu, the Project window ("Generate Normal Map...", "Edit Normal
// Map...", "Regenerate Normal Map"), the Inspector (the Normal Texture row's wand, the Sprite's
// "Normal Map..." button) and MCP focus_window "sprite_normals". Every entry, button and
// shortcut calls the scripted API below; the non-UI logic is spritenormals/SpriteNormalsSession.
//
// Paint (Phase 5): brushes (Angle, Flatten, Smooth, Erase; a colour wand) correct the generated
// normals on a paint layer the window keeps (single mode). Strokes are window-local undo steps
// until Save writes the layer into the normal map (its jasper.spritenormals.paint chunk); every
// later generation composites it again. Opening a map (or changing the output) loads its layer.

#include "spritenormals/SpriteNormalsApply.h"
#include "spritenormals/SpriteNormalsSession.h"
#include "window/SpriteNormalsWindowSettings.h"

#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "texture/Texture.h"

#include "imgui.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    class Command;

    class SpriteNormalsWindow {
        friend struct SpriteNormalsWindowTestAccess;
        friend struct SpriteNormalsPaintWindowTestAccess;

    public:
        static constexpr const char* WINDOW_NAME = "Sprite Normal Map";
        enum class View { Color, Normal, Lit, Split };
        // A row of the Warnings section: Phase 2's sceneWarnings (kind and message verbatim) or
        // the window's own (output_exists, source_changed, the session's problems); `fixes` are
        // the labels of its one-click buttons (applyFix)
        struct WarningRow {
            std::string kind;
            std::string text;
            bool error = false;
            std::vector<std::string> fixes;
        };

        explicit SpriteNormalsWindow(Project* project);
        ~SpriteNormalsWindow();                       // cancels a live run

        // Once a frame, open or not: draws the window when open and ticks the model (debounce,
        // the worker, Play / Orbit, Save All, Shift-click generations, the assign target)
        void show();
        void open();
        void setOpen(bool open);
        bool isOpen() const;
        // the window or one of its children has the keyboard focus (App: undo routing, keys)
        bool isFocused() const;
        // Edit > Frame Selected (F) while it has focus: the image fits the view (App's claim)
        void requestFit() { fitRequested = true; }
        // Another project is being opened: cancel, forget sources, results, targets, GPU images
        void resetForProjectSwitch();

        // ---- entry points (absolute or project-relative paths); false + error when nothing usable

        // 1 image = single mode, more = batch mode (non-images and .svg are skipped)
        bool openSources(const std::vector<std::filesystem::path>& images, std::string& error);
        // "Edit Normal Map...": a generated PNG's recipe restored (settings, frames, outputs)
        bool openRecipe(const std::filesystem::path& generatedPng, std::string& error);
        // The Inspector: the GameObject's submesh texture (and a Sprite's frames) as the source,
        // Assign on; the current normal texture's recipe restored when it has one. now: generate
        // with the last settings (or that recipe's), save and assign without opening the window
        // (an existing non-generated file at the default path opens the window instead)
        bool openForEntity(uint32_t sceneId, Entity entity, unsigned submesh, bool now, std::string& error);
        // "Regenerate Normal Map": from the output's recipe, written over it (one Project undo
        // step), no window
        // (blocking: tests and tools; the editor's entries use startRegenerate)
        static bool regenerateFromRecipe(Project* project, const std::filesystem::path& generatedPng,
                                         std::vector<std::string>& written, std::string& error);
        // The same on the worker, written by a later tick (the Project window's and the Inspector's
        // Regenerate): false + error when the recipe can't be used; the outcome goes to the
        // Console (Out::info) or an alert
        bool startRegenerate(const std::filesystem::path& generatedPng, std::string& error);
        // a Shift-click generation or a Regenerate still running
        bool hasPendingJobs() const { return !quickJobs.empty(); }
        // How the last Regenerate of a generated PNG ended (the Inspector's asset view says it)
        struct RegenerateOutcome {
            bool known = false;              // one was started in this window's life
            bool finished = false;
            bool ok = false;
            std::string message;             // "Regenerated hero_normal.png.", the error, or why it waits
        };
        RegenerateOutcome regenerateOutcome(const std::filesystem::path& generatedPng) const;
        // Editor shutdown: the Shift-click / Regenerate workers are cancelled and waited for (bounded)
        void shutdownJobs(double timeoutSeconds = 2.0);

        // ---- settings (the shared set) and the current item's spec

        void setSettings(const spritenormals::Settings& s);
        const spritenormals::Settings& getSettings() const { return settings; }
        // all six preset values (the four sliders, Softness 1.15, Radius Auto), overrides cleared
        void setPreset(spritenormals::Preset p);
        void setFrames(const spritenormals::FramesSpec& f);
        void setOutput(const std::string& projectRelative);          // "" = default
        void setHeightMap(bool on);
        // the current item's spec with the shared settings (what a run of it is made of)
        const spritenormals::Spec& currentSpec() const;
        void setAssignTarget(uint32_t sceneId, Entity entity, unsigned submesh);
        void clearAssignTarget();
        void setAssign(bool on);
        bool hasAssignTarget() const { return target.entity != NULL_ENTITY; }
        bool getAssign() const { return assign && hasAssignTarget(); }

        // ---- batch

        size_t itemCount() const { return items.size(); }
        size_t currentItem() const { return current; }
        bool isBatch() const { return items.size() > 1; }
        void selectItem(size_t i);
        void setItemChecked(size_t i, bool checked);
        bool isItemChecked(size_t i) const;

        // ---- running

        // a run now (no debounce); false + error when the spec has problems
        bool regenerate(std::string& error);
        void cancel();
        bool isRunning() const;
        // a finished result of the current item is shown
        bool hasResult() const;
        // the settings changed since the shown result
        bool isStale() const;
        // Save / Save & Assign (assign = the checkbox's value); batch: Save All (it generates the
        // checked items without a result first, then writes everything in one step: `written`
        // stays empty until that write)
        bool save(bool overwrite, std::vector<std::string>& written, std::string& error);

        // ---- preview

        void setView(View v);
        View getView() const { return view; }
        void setLight(float x, float y, float height);
        void setLightColour(const Vector3& c);
        void setAmbient(float a);
        // the lit preview's Light Steps: each light's term in N bands as the runtime's. kSceneLightSteps
        // (the default) follows the scene's Light Steps (the target's scene, else the open one); 0 = off, 1..16
        static constexpr int kSceneLightSteps = -1;
        void setLightSteps(int steps);
        int getLightSteps() const { return lightSteps; }
        int effectiveLightSteps() const;              // what the lit preview draws with (0..16)
        void setFrame(int index);                     // -1 = All
        int getFrame() const { return frame; }
        void setPlaying(bool on);
        bool isPlaying() const { return playing; }
        void setOrbit(bool on);
        bool isOrbiting() const { return orbit; }
        // the eyedropper's click: the source pixel's colour added to the region
        bool pickRegionColour(int region, int px, int py, std::string& error);

        // ---- warnings

        const std::vector<WarningRow>& getWarnings() const { return warnings; }
        bool applyFix(const std::string& kind, const std::string& fix, std::string& error);

        // ---- what the window shows (status line, recipe notices, the source summary)

        const std::string& getStatus() const { return statusMessage; }
        bool statusIsError() const { return statusError; }
        const std::vector<std::string>& getNotices() const { return notices; }
        std::string sourceSummary() const;
        std::string framesSummary() const;
        // the files Save writes for the current item (session plannedOutputs)
        std::vector<spritenormals::PlannedOutput> plannedOutputs() const;

        // ---- App: Ctrl+Z / Ctrl+Y while the window is the last focused (spec 3.10): the Scene
        // history of the last Save & Assign while that assign is its newest step (redo: the next
        // redoable one), else the Project history
        bool undoGoesToScene(uint32_t& sceneId) const;
        bool redoGoesToScene(uint32_t& sceneId) const;
        // the window's keys (views, frames, play, orbit, grid, fit, Esc, Ctrl+Enter; the paint
        // tools V B N M E, W X H P, - =); the App keeps the main toolbar's letters away while the
        // window is focused
        void handleShortcuts();

        // ---- paint (Phase 5 spec 5.5; scripted for tests: no confirm alerts). Pixels are of the
        // output image (the atlas for animations). Painting needs a result in single mode.

        void setPaintTool(spritenormals::PaintTool tool);
        spritenormals::PaintTool getPaintTool() const { return paintTool; }
        // shape, size, strength, smoothRadius, fill, wandTolerance, wandContiguous, wandAllFrames,
        // mirrorX, direction (clamped to their ranges)
        void setPaintBrush(const spritenormals::PaintBrush& brush);
        const spritenormals::PaintBrush& getPaintBrush() const { return brush; }
        // one stroke of the current tool along `path` (mouse samples joined by lines, the brush
        // stamped at every pixel, each pixel changed once); one undo step. False when painting is
        // off (Navigate, no result, batch, a paint size mismatch)
        bool paintStroke(const std::vector<std::array<int, 2>>& path);
        // the wand at (x, y): the current tool on the matching pixels, one undo step
        bool wandFill(int x, int y);
        // MCP-shaped operations on the window's overlay, one undo step; false on invalid_paint
        bool applyPaintOps(const std::vector<spritenormals::PaintOp>& ops, std::vector<spritenormals::Problem>* problems);
        bool undoPaint();
        bool redoPaint();
        bool canUndoPaint() const { return !paintUndo.empty(); }
        bool canRedoPaint() const { return !paintRedo.empty(); }
        void clearPaint();                            // one undo step, no confirm alert
        const spritenormals::PaintOverlay& paintOverlay() const { return paint; }
        size_t paintedPixelCount() const { return paint.paintedCount(); }
        // the overlay differs from the one last loaded or saved
        bool hasUnsavedPaint() const;
        // the strokes since the last load or save dropped (project switch, closing: Discard)
        void discardPaint();
        // the stroke redos dropped (the App: a project or scene step undone while the window is
        // focused, so Ctrl+Y redoes that step first)
        void dropPaintRedo() { paintRedo.clear(); }
        // Opening another source or map (openSources, openRecipe, openForEntity, setSourcePath,
        // setSourceEntity, the Use Base Texture fix, an output with its own paint) asks "Discard
        // Paint Changes?" first while strokes are unsaved, and goes on after Discard. Off (the
        // scripted path): unsaved strokes are dropped without asking.
        void setConfirmPaintDiscard(bool on) { confirmPaintDiscard = on; }
        // Alt+click: the composited normal at (x, y) becomes the direction, the tool Angle
        // (false on a transparent pixel)
        bool samplePaintDirection(int x, int y);
        void setShowPaint(bool on);
        bool getShowPaint() const { return showPaint; }
        void setHighlightPainted(bool on) { highlightPainted = on; }
        bool getHighlightPainted() const { return highlightPainted; }
        // why the brushes are off ("" when painting is possible)
        std::string paintDisabledReason() const;
        // the loaded paint has another size than the result (Save waits for Clear Paint)
        bool paintSizeMismatch() const;

    private:
        // A GPU copy of an RGBA8 image for ImGui (the Texture Synthesis pattern)
        struct GpuImage {
            Texture texture;
            int width = 0;
            int height = 0;
            uint64_t serial = 0;
            bool valid() const { return width > 0 && !texture.empty(); }
        };

        // One source (single mode: the only one; batch: one per image)
        struct Item {
            spritenormals::Spec spec;          // source / entity / submesh / frames / outputs (settings: the window's)
            bool checked = true;
            std::string label;                 // the file name, or the GameObject's
            // what the source is (probed without decoding pixels, re-probed when it changes on disk)
            bool probed = false;
            bool probeOk = false;
            spritenormals::SourceInfo info;
            std::string texturePath;           // the image (a GameObject's texture), project-relative
            std::vector<spritenormals::Cell> spriteCells;
            std::string probeError;
            std::filesystem::file_time_type stampTime{};
            uintmax_t stampSize = 0;
            bool changedOnDisk = false;
            // the result shown for it (a finished run) and the fingerprint it was made from
            std::shared_ptr<spritenormals::SpriteNormalsRun> result;
            uint64_t resultId = 0;             // a fresh id per result (caches key on it, never on the run's address)
            std::string resultKey;
            std::string savedKey;              // the resultKey last written (Save / Save All)
            std::vector<spritenormals::Problem> problems;
            std::string problemsKey;           // what problems were checked for (source, frames, outputs, stamps)
        };

        struct AssignTarget {
            uint32_t sceneId = 0;
            Entity entity = NULL_ENTITY;
            unsigned submesh = 0;
            std::string name;
        };

        struct LightState {
            float x = 0.0f;                    // image pixels, relative to the shown cell's top-left
            float y = 0.0f;
            float height = 32.0f;
            float range = 96.0f;
            Vector3 colour = Vector3(1.0f, 1.0f, 1.0f);
            float ambient = 0.25f;
            bool placed = false;               // set for the current cell size (else the default)
            bool userHeight = false;           // the height was set by hand (else 0.6 x the larger cell side)
            int cellWidth = 0;
            int cellHeight = 0;
        };

        // A Save All in progress: the checked items without a fresh result run one after another
        struct SaveAllJob {
            bool active = false;
            bool overwrite = false;
            std::vector<size_t> queue;
            size_t done = 0;
        };

        // A paint undo step: the pixels a stroke (wand fill, Clear) changed, before and after
        struct PaintPatch {
            int width = 0, height = 0;           // the overlay's size (a cleared mismatched one comes back)
            std::vector<int> pixels;
            std::vector<std::array<uint8_t, 4>> before;
            std::vector<std::array<uint8_t, 4>> after;
            size_t bytes() const { return pixels.size() * (sizeof(int) + 8); }
        };

        // An Inspector Shift-click: generate, save and assign with the window closed
        // ... and a Regenerate (no target: written only). Decoding, generating and encoding run on
        // the job's worker (QuickWork); the write (an undoable command) and the assign on the main thread
        struct QuickWork;
        struct QuickJob {
            std::shared_ptr<QuickWork> work;
            AssignTarget target;
            bool overwrite = false;
            std::string label;                   // the normal map's file name
            std::filesystem::path png;           // Regenerate: the map regenerated (normalized)
        };
        std::map<std::string, RegenerateOutcome> regenerateOutcomes;
        // starts `work` on a detached thread
        static void startWork(const std::shared_ptr<QuickWork>& work);
        // every job's worker to its end, without writing (tests)
        void waitJobWorkers();

        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        bool focused = false;

        std::vector<Item> items;
        size_t current = 0;
        spritenormals::Settings settings;
        mutable spritenormals::Spec specCache;   // currentSpec()'s storage
        AssignTarget target;
        bool assign = false;
        bool storedLoaded = false;              // the project's stored settings were taken
        std::filesystem::path storedProject;    // ... from this project

        // the run of the current item (live or forced) and what it was started from
        std::shared_ptr<spritenormals::SpriteNormalsRun> run;
        size_t runItem = 0;
        std::string runKey;
        double runStartedAt = 0.0;
        std::vector<spritenormals::Problem> runErrors;
        double changedAt = -1.0;                // debounce: the last change, -1 = none pending
        double clockOffset = 0.0;               // the test access advances the clock
        double lastTick = -1.0;
        SaveAllJob saveAll;
        std::vector<QuickJob> quickJobs;

        // feedback
        std::string statusMessage;
        bool statusError = false;
        std::vector<std::string> notices;
        std::vector<std::string> lastWritten;
        std::vector<WarningRow> warnings;
        double warningsAt = -1.0;
        bool warningsDirty = true;
        std::function<void()> lastConfirm;      // the last Replace Files? callback (test access)
        bool holdConfirms = false;              // tests: keep it instead of asking

        // undo routing (spec 3.10)
        uint32_t assignSceneId = 0;
        size_t assignPosition = 0;               // the scene history's position right after the assign
        unsigned long long assignSerial = 0;     // ... and its step (Command::serial, not only the position)
        bool assignRecorded = false;
        size_t writePosition = 0;                // the project history's position right after the write
        unsigned long long writeSerial = 0;
        bool writeRecorded = false;

        // preview state
        View view = View::Lit;
        int frame = -1;                          // -1 = All
        bool playing = false;
        double playElapsed = 0.0;
        bool orbit = false;
        double orbitTime = 0.0;
        bool grid = true;
        bool advancedOpen = false;
        LightState light;
        int lightSteps = kSceneLightSteps;       // the lit preview's Light Steps (kSceneLightSteps, 0 off, 1..16)
        int litSteps = 0;                        // the steps the lit preview was last drawn with
        float zoom = 0.0f;                       // 0 = fit (computed on draw)
        bool fitRequested = true;
        ImVec2 pan = ImVec2(0, 0);
        float splitPosition = 0.5f;
        bool draggingLight = false;
        bool draggingSplit = false;
        int pickingRegion = -1;                  // the eyedropper's region, -1 = off
        int lastEditedRegion = -1;
        float footerHeight = 0.0f;
        float lastFitZoom = 1.0f;
        ImVec2 canvasSize = ImVec2(0, 0);
        int visibleX0 = 0, visibleY0 = 0, visibleX1 = 0, visibleY1 = 0;   // All view of a large image
        double sourceCheckedAt = -1.0;
        char pickerFilter[128] = "";
        std::vector<std::string> projectImages;
        double projectImagesScannedAt = -1.0;
        std::string sourceText;                  // the Source field as typed
        std::string outputText;
        uint32_t addColourValue = 0xffffff;

        // CPU images of the preview (the shown cell) and their GPU copies
        spritenormals::Image colorRgba;          // the source (or the atlas), whole
        std::string colorLoadedKey;
        spritenormals::Image litRgba;            // the lit preview of the shown region
        int litOriginX = 0, litOriginY = 0;      // where litRgba sits in the shown cell
        uint64_t litSerial = 0;
        bool litDirty = true;
        double litRenderedAt = -1.0;
        uint64_t resultSerial = 0;
        GpuImage colorGpu;
        GpuImage normalGpu;
        GpuImage litGpu;
        std::vector<Texture> retired;            // destroyed next frame (ImGui draws after show())
        // the metrics lines of the shown result (measure + assess, once per result)
        uint64_t metricsSerial = 0;
        uint64_t metricsResult = 0;            // the result id of the metrics line
        spritenormals::Assessment metricsAssessment;
        std::string metricsLine;
        uint64_t metricsPaintSerial = 0;

        // paint (single mode): the overlay, what was last loaded / saved, the stroke stack
        spritenormals::PaintOverlay paint;
        spritenormals::PaintOverlay paintSaved;
        std::string paintTarget;                 // the normal map the overlay was loaded for (absolute)
        std::string paintProblem;                // its paint chunk could not be read (dropped on Save)
        std::vector<PaintPatch> paintUndo;
        std::vector<PaintPatch> paintRedo;
        size_t paintUndoBytes = 0;
        size_t paintUndoByteLimit = 64u * 1024u * 1024u;  // the undo stack's byte cap (tests lower it)
        // strokes made in the window since the target was loaded and not undone (a Save keeps
        // them; the undo cap dropping old patches doesn't lower it): with none, a change of the
        // target on disk is taken as the overlay
        size_t paintStrokes = 0;
        spritenormals::PaintTool paintTool = spritenormals::PaintTool::Navigate;
        spritenormals::PaintBrush brush;
        bool showPaint = true;
        bool highlightPainted = false;
        bool paintOpen = false;                  // the Paint foldout
        bool paintOpenSet = false;               // ... opened once for a non-empty overlay
        uint64_t paintSerial = 1;                // the overlay changed
        bool confirmPaintDiscard = true;
        // the target file as it was last loaded or written (a change on disk: a Save undone, a
        // regeneration): its paint is read again as the saved state
        bool paintStampExists = false;
        std::filesystem::file_time_type paintStampTime{};
        uintmax_t paintStampSize = 0;
        double paintStampCheckedAt = -1.0;
        // what changed since the composite shown and the lit preview were made (a stroke: its pixels)
        bool displayedValid = false;
        bool dirtyAll = true;
        int dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = 0, dirtyY1 = 0;
        int litDirtyX0 = 0, litDirtyY0 = 0, litDirtyX1 = 0, litDirtyY1 = 0;
        double normalUploadedAt = -1.0;
        // Smooth's cells and islands of the shown result (built once per result)
        spritenormals::PaintTopology topology;
        uint64_t topologyResult = 0;            // the result id it was built for
        mutable uint64_t unsavedFor = 0;         // hasUnsavedPaint() for this paintSerial ...
        mutable bool unsavedCached = false;      // ... (an overlay compare per change, not per frame)
        // a stroke in progress (mouse or paintStroke)
        bool stroking = false;
        std::vector<uint8_t> strokeMask;
        PaintPatch strokePatch;
        std::array<int, 2> strokeLast{0, 0};
        bool strokeStarted = false;
        // the composited normal shown (the generation + the overlay) and the generation in OpenGL green
        spritenormals::Image displayed;
        uint64_t displayedResult = 0;           // the result id the composite shown was made from
        spritenormals::Image generatedGLImage;
        uint64_t generatedGLResult = 0;
        uint64_t resultIds = 0;                 // the last result id given
        // what the Normal view's GPU copy (normalGpu) shows: the overlay state it was uploaded at
        // (paintSerial * 2 + Show Paint; 0 = upload again) and the result id it is the composite of
        uint64_t normalGpuFor = 0;
        uint64_t normalGpuResult = 0;
        GpuImage hemisphereGpu;                  // the direction disk's colour wheel
        // the wand's hover outline (cached for the hovered pixel)
        int wandHoverX = -1, wandHoverY = -1;
        uint64_t wandHoverSerial = 0;
        std::vector<int> wandHover;

        // ---- model
        double now() const;
        void loadStored();
        void stopRun();
        bool setSourcePath(const std::string& text, std::string& error);
        bool setSourceEntity(uint32_t sceneId, Entity entity, std::string& error);
        std::string primaryDisabledReason() const;
        void primaryAction();
        void tick();
        void markChanged();
        void resetPreviewFor(bool keepLight);
        Item makeItem(const spritenormals::Spec& spec) const;
        void probe(Item& item, bool force);
        void refreshProblems(Item& item);
        spritenormals::Spec specOf(const Item& item) const;
        std::string keyOf(const Item& item) const;
        bool liveAllowed(const Item& item) const;
        const Scene* sceneFor(const spritenormals::Spec& spec) const;
        bool startRun(size_t itemIndex, std::string& error);
        void finishRun();
        void tickSaveAll();
        void tickQuickJobs();
        void checkTarget();
        void checkSourceOnDisk();
        bool writeResults(const std::vector<size_t>& itemIndices, bool overwrite, std::vector<std::string>& written, std::string& error);
        void afterWrite(const std::vector<spritenormals::OutputFile>& files);
        std::vector<std::string> existingOutputs(const std::vector<size_t>& itemIndices) const;
        void requestSave();
        void confirm(const std::string& title, const std::string& message, std::function<void()> onYes);
        void setStatus(const std::string& message, bool error);
        void refreshWarnings(bool force);
        void storeSettings();
        void autoTargetFor(const std::string& imagePath);
        // the assign target still draws `item`'s image (or its atlas); else it is cleared
        bool targetDraws(const Item& item) const;
        void keepTargetFor(const Item& item);
        void recordWrite();
        void recordAssign(uint32_t sceneId);
        std::shared_ptr<spritenormals::SpriteNormalsRun> shown() const;
        std::vector<spritenormals::Cell> shownCells() const;
        void cellRect(int index, int& x, int& y, int& w, int& h) const;
        void defaultLight();
        void ensureLight();
        void renderLit();
        spritenormals::LightSetup lightSetup() const;
        static bool writeAndNotify(Project* project, const std::vector<spritenormals::OutputFile>& files, std::string& error);

        // ---- paint
        void loadPaintForTarget();
        void paintChanged();
        // a change of the pixels [x0, x1) x [y0, y1): only they are composited and lit again
        void paintChangedRect(int x0, int y0, int x1, int y1);
        void paintChangedPixels(const std::vector<int>& pixels);
        bool paintDiscardOk(const std::function<void()>& retry);
        void recordPaintStamp();
        void checkPaintTarget();
        // the normal map `output` ("" = the default name) would write for `item`, absolute
        std::filesystem::path plannedNormalFor(const Item& item, const std::string& output) const;
        const spritenormals::PaintTopology* topologyFor(bool needed);
        // the shown result's id (0: none)
        uint64_t shownId() const;
        void renderLitRect();
        bool painting() const { return paintTool != spritenormals::PaintTool::Navigate && paintDisabledReason().empty(); }
        // the shown result's generation in OpenGL green, the colour (atlas) and cells
        const spritenormals::Image& generatedGL();
        const spritenormals::Image& displayNormal();
        // the composite Save writes (whatever Show paint says)
        spritenormals::Image composedNormal();
        void beginStroke();
        void strokeTo(int x, int y);
        void endStroke();
        void pushPaintPatch(PaintPatch patch);
        // a patch of what `change` does to the overlay (the pixels it changed)
        void recordPaint(const std::function<void()>& change);
        void drawPaint();
        bool directionDisk(const char* id, float size);
        void drawPaintOverlays(ImDrawList* draw, const ImVec2& imgMin, float zoom, int cx, int cy, int cw, int ch, const ImVec2& pixel, bool hovered);

        // ---- GPU images
        void updateGpuImage(GpuImage& gpu, const spritenormals::Image& rgba, uint64_t serial);
        void retire(GpuImage& gpu);
        void releaseRetired();
        void updatePreviewImages();

        // ---- UI
        void drawSettings();
        void drawNotices();
        void drawSource();
        void drawBatch();
        void drawFrames();
        void drawPresetAndSliders();
        void drawToonSteps(const spritenormals::Settings& base);              // after Volume: Off / 4 / 8 / 16
        void drawToonAdvanced(const spritenormals::Settings& base, bool& changed);   // directions, tilt rings, flat, rotation
        void drawGaps(const spritenormals::Settings& base);                   // Dark Gaps Are Edges, Gap Amount
        void drawAdvanced();
        void drawRegions();
        void drawOutput();
        void drawWarnings();
        void drawFooter();
        void drawPreview();
        void drawToolbar();
        void drawCanvas(const ImVec2& size);
        void drawMetrics();
        bool imagePickerPopup(const char* popupId, std::string& path);
        void scanProjectImages(bool force);
    };

}
