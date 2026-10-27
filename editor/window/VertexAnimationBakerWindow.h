// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > Animation > Vertex Animation Baker (vertex animation spec 5.1): bakes any animation the
// engine plays into a Vertex Animation (.vat + .vat.glb), or imports a Houdini Labs VAT 3 / OpenVAT
// export. One column, top to bottom:
//   Source   a field and drop zone: a model, prefab, .anim, .poseanim or .vat file (Project window),
//            a GameObject (Hierarchy), or a Houdini / OpenVAT export path; one line of what it found
//   Clips    the source's clips: checklist, frames, length, wrap and fps override per clip, All /
//            None; for .anim sources the "Play on" model, prefab or GameObject
//   Settings Mode (Auto shows its decision), Sample rate, Normals, Precision, Preset, Sockets
//   Estimate texture size, memory (total and per clip), fetches per vertex, a bar against the
//            preset's texels, the validation rows (spec 4.7) with their fixes as buttons
//   Preview  source vs VAT side by side (VatPreviewView: live offscreen views of the source posed by
//            its clip and of the bake's in-memory result on the GPU), play / pause, scrub, clip
//   Output   path (the baker's default shown), Place (Files only / Beside / Replace / Crowd...),
//            Bake (progress, "Stage x of y" and Cancel replace it while it runs)
// A Houdini or OpenVAT export switches it to Import: what the importer detected (mode, metadata,
// files, missing pieces), its options (fps when missing, axis when undetermined, upsampling,
// colours) and Import. A .vat opens its recipe: Rebake (or Re-import) writes over it keeping its
// GUID and hand edits; its staleness is shown. Nothing bakes while a scene plays.
//
// The model is reachable without the UI (the scripted API below): the buttons, the Inspector's
// "Bake ▾ › Vertex Animation…" entries (VatBakerHooks), the Project window and the tests run the
// same functions. The baking itself is VatBakeSession's; imports are importVat + writeVatImport.

#include "vat/VatBakeSession.h"
#include "vat/VatBakerHooks.h"
#include "vat/VatImportWrite.h"
#include "vat/import/VatImport.h"

#include "ecs/Entity.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine::editor {

    class Project;
    class VatPreviewView;
    struct VatBakerWindowTestAccess;

    class VertexAnimationBakerWindow {
        friend struct VatBakerWindowTestAccess;
    public:
        static constexpr const char* WINDOW_NAME = "Vertex Animation Baker";
        // posing time per editor frame of a running bake (the scene stays responsive)
        static constexpr double FRAME_BUDGET_MS = 12.0;

        explicit VertexAnimationBakerWindow(Project* project);
        ~VertexAnimationBakerWindow();
        VertexAnimationBakerWindow(const VertexAnimationBakerWindow&) = delete;
        VertexAnimationBakerWindow& operator=(const VertexAnimationBakerWindow&) = delete;

        // once a frame: draws the window when open, and advances a running bake or import (also
        // with the window closed: a Shift-click bake runs without it)
        void show();
        void open();
        void setOpen(bool open);
        bool isOpen() const;
        // Another project is being opened: a running bake or import is cancelled, everything forgotten
        void resetForProjectSwitch();
        // a bake or import result that was not written (or a run in progress): opening another
        // source would drop it
        bool holdsUnsavedResult() const;
        // `replace` now, or (holdsUnsavedResult) once the user confirms the editor host's
        // "Replace the Baker's Result?" (the Project window's double-click on a .vat, Rebake)
        void askBeforeReplacing(const std::string& what, std::function<void()> replace);

        enum class Mode { None, Bake, Import };

        // ---- sources (the Source field, drops, entry points)

        // a project file (model, prefab, .anim, .poseanim, .vat) or a Houdini / OpenVAT export
        // (folder or any of its files, absolute or project-relative). False with `error` when it
        // is none of these.
        bool setSourcePath(const std::string& path, std::string& error);
        // a GameObject of an open scene (Hierarchy drop)
        bool setSourceEntity(uint32_t sceneId, Entity entity, std::string& error);
        // a .vat: its recipe loaded, Rebake (baker recipes) or Re-import (importer recipes)
        bool openVat(const std::string& vatPath, std::string& error);
        // .anim sources: the model, prefab (file) or GameObject the clips play on
        bool setPlayOnPath(const std::string& path, std::string& error);
        bool setPlayOnEntity(uint32_t sceneId, Entity entity, std::string& error);
        // the Inspector's Bake ▾ › Vertex Animation… (now: Shift-click, bakes with the last
        // settings placed beside); also the window opens
        bool openEntry(const VatBakeEntry& entry, bool now, std::string& error);

        // ---- settings

        Mode getMode() const { return mode; }
        const vat::VatBakeSpec& getSpec() const { return spec; }
        // the settings (the source stays: setSource* changes it); the estimate follows
        void setSpec(const vat::VatBakeSpec& next);
        // the clip checklist: `names` checked, in source order (empty: none)
        void setCheckedClips(const std::vector<std::string>& names);
        // a validation row's fix button (VatFix setting/value); false with `error` for a fix it
        // can't apply
        bool applyFix(const vat::VatFix& fix, std::string& error);
        // sockets: the Hierarchy selection under the source (or a GameObject of its model)
        int addSelectedSockets(std::string& error);
        vat::VatImportSpec& importSettings() { return importSpec; }
        vat::VatImportWriteOptions& importOutput() { return importOut; }

        // ---- the estimate (live)

        // runs the check and the estimate now when the settings changed since (the window
        // refreshes it itself a moment after the last change)
        void refreshEstimate();
        const std::vector<vat::VatIssue>& getIssues() const { return issues; }
        const vat::VatEstimate& getEstimate() const { return estimate; }
        const std::string& getSummary() const { return summary; }
        // the clips the source offers (after refreshEstimate)
        struct ClipRow {
            std::string name;
            std::string origin;
            float length = 0.0f;
            ClipWrapMode wrap = ClipWrapMode::Loop;     // the source's
        };
        const std::vector<ClipRow>& getClips() const { return clips; }
        // the output the bake would write (the existing-file rule), "" when it can't be told
        std::string plannedOutput() const { return planned; }

        // ---- running

        // Bake (write: the files, one undo step, once done) or a preview run (write false: the
        // result stays in memory, a later Bake of the same settings writes it without baking
        // again). False with `error` when it can't start (scene playing, check errors...).
        bool bake(bool write, std::string& error);
        // Import of the export (on a worker thread; written once done)
        bool runImport(std::string& error);
        void cancel();
        bool isRunning() const;
        // advances a running bake by `budgetMs` (0: to the end); finishes (writes) it once done
        void tick(double budgetMs);
        // the last bake run (its result when done), null before one
        const vat::VatBakeSession* lastRun() const { return run.get(); }
        const vat::VatImportResult* lastImport() const { return importResult.get(); }
        const vat::VatWriteResult& lastWrite() const { return written; }
        const std::string& getStatus() const { return status; }
        bool statusIsError() const { return statusError; }
        // "Stage x of y: ..." of the running bake or import
        std::string stageLine() const;
        // the settings changed since the shown result
        bool isStale() const;

        // the Preview's Heatmap: the VAT side coloured by each vertex's error (green at 0, yellow at
        // the tolerance, red at twice it), unlit; only for a bake (imports carry no per-vertex error)
        void setHeatmap(bool on);
        bool isHeatmap() const { return heatmap; }
        bool heatmapAvailable() const;

        // the Console line of a write (the clipBakeStatusLine pattern)
        static std::string statusLine(const vat::VatWriteResult& written, const jasperengine::vat::VatData& data, bool import);

    private:
        struct ImportJob {
            std::thread thread;
            std::atomic<bool> done{false};
            vat::VatImportSpec spec;
            vat::VatImportResult result;
        };
        struct LastSettings {
            vat::VatModeChoice mode = vat::VatModeChoice::Auto;
            float fps = 30.0f;
            bool normals = true;
            vat::VatPrecisionChoice precision = vat::VatPrecisionChoice::Auto;
            vat::VatPreset preset = vat::VatPreset::MobileWeb;
            float toleranceMm = 1.0f;
        };

        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;

        Mode mode = Mode::None;
        std::string sourceLabel;           // the Source field's text
        std::string playOnLabel;           // the Play on field's text
        vat::VatBakeSpec spec;
        std::vector<std::string> sourceNotes;   // staleness, recipe notes
        std::string rebakePath;             // the .vat whose recipe is loaded ("" = none)
        std::string rebakeKey;              // specKey() of the recipe as loaded (unchanged: an exact rebake)
        bool noneChecked = false;           // the clip checklist is empty (spec.clips empty means all)
        bool reimport = false;              // its recipe is an importer's

        // estimate
        std::unique_ptr<vat::VatBakeSession> probe;
        std::string probeKey;               // specKey() the estimate is of
        double changedAt = -1.0;
        std::vector<vat::VatIssue> issues;
        vat::VatEstimate estimate;
        std::vector<ClipRow> clips;
        std::string summary;
        std::string planned;
        bool plannedRewrite = false;        // `planned` holds a .vat of this source (rebaked in place)

        // bake run
        std::unique_ptr<vat::VatBakeSession> run;
        std::string runKey;                 // specKey() of the run
        bool writeWhenDone = false;
        bool runFinished = false;           // its end was handled
        vat::VatWriteResult written;
        LastSettings last;

        // import
        vat::VatImportDetection detection;
        vat::VatImportSpec importSpec;
        vat::VatImportWriteOptions importOut;
        std::shared_ptr<ImportJob> importJob;
        std::vector<std::shared_ptr<ImportJob>> abandoned;  // cancelled, joined once they finish
        std::shared_ptr<vat::VatImportResult> importResult;
        std::string importKey;              // importSpec of the shown result
        double importStartedAt = 0.0;

        std::string status;
        bool statusError = false;
        double sourceCheckedAt = -1.0;      // the last watchSource() check (now())

        // preview
        std::unique_ptr<VatPreviewView> sourceView;
        std::unique_ptr<VatPreviewView> vatView;
        const void* previewOf = nullptr;    // the result the views show
        std::string previewVatFile;         // its preview files
        std::string previewMeshFile;
        std::string heatVatFile;            // the heatmap's preview files (written on first use)
        std::string heatMeshFile;
        bool heatmap = false;               // the toggle
        bool previewHeat = false;           // what the VAT view shows
        std::string previewNote;            // why there is no source view
        int previewClip = 0;
        float previewTime = 0.0f;
        bool previewPlaying = true;

        // UI state
        float footerHeight = 0.0f;
        char socketInput[256] = "";
        int crowdCount = 100;
        float crowdArea = 10.0f;
        int crowdSeed = 1;

        void resetSource();
        void markChanged();
        std::string specKey() const;
        std::string importSpecKey() const;
        void updateSummary();
        void finishRun();
        void finishImport();
        // what a bake or import throws (bad_alloc, a library error) ends it with an error in the
        // window and the Console instead of taking the editor down
        void failWith(const std::string& what, const std::string& message);
        // the GameObject (or file) the source or the stored result was baked from is gone: why, else ""
        std::string sourceGone(const vat::VatSourceSpec& source) const;
        // a few times a second: a closed scene resets a GameObject source; a deleted source drops
        // the stored in-memory result (Bake must not write a result whose source is gone)
        void watchSource();
        void dropStoredResult(const std::string& why);
        void setStatus(const std::string& message, bool error);
        // the Place: Crowd... seed (the field never goes below 0)
        uint32_t crowdSeedValue() const { return (uint32_t)std::max(crowdSeed, 0); }
        void reveal(const std::string& relative);
        void rememberSettings();
        bool startBake(bool write, std::string& error);
        bool startBakeFor(const std::string& key, bool write, std::string& error);
        void ensurePreview();
        // the VAT view switched to (or back from) the heatmap's mesh when the toggle says so
        void applyHeatmap();
        void drawHeatmapLegend();
        // the views posed at the playhead (advanced by dt while playing)
        void updatePreview(float dt);
        void releasePreview();
        // the model or prefab file the source view loads, "" with previewNote set
        std::string sourcePreviewFile();
        std::string defaultImportOutputPath() const;
        bool scenePlaying(std::string* reason) const;

        // UI parts
        void drawSource();
        void drawImportSource();
        void drawClips();
        void drawSettings();
        void drawSockets();
        void drawEstimate();
        void drawIssues(const std::vector<vat::VatIssue>& list, const char* id);
        void drawPreview();
        void drawOutput();
        void drawFooter();
        void drawImportSettings();
        void drawImportReport();
        bool sourceField(const char* id, std::string& text, const char* hint, bool playOn);
    };

}
