// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R32-R35, R46, R57, §6.9)
// - Purpose: baking a `.titleart` to its outputs: every output's content is rendered through the
//   one renderer (TitleArtRenderHost), the shared trimmed frame is the union of their content
//   bounds, each output is cut to that frame at its own scale (sizes rounded to round_to),
//   alpha-bled, encoded 8- or 16-bit (`mask`: 1-channel 8-bit) with the recipe chunk, and the
//   PNGs (a new one with a .meta of texture import defaults) and the sidecar are written all or
//   nothing inside expected-write scopes, then their textures are reloaded and recooked
//   explicitly. Bakes are not undo steps (derived files).
// - TitleArtBakeJob is resumable: step(budgetMs) advances the current render within the budget
//   (TitleRenderJob slices), so the editor keeps drawing; run() takes it to the end (export,
//   CLI). `dryRun` renders and reports sizes, bytes and VRAM without writing.
// - Auto Bake (R34): with title_art.auto_bake, a commit (or an outside edit, or a changed font,
//   image or SVG of the document) of a document whose outputs exist schedules a rebake of its
//   stale outputs 500 ms after its last change; TitleArtAutoBake::tick runs from the editor
//   frame. A newer-version document is never rebaked.
// - Thread-safety: main thread.

#pragma once

#include "TitleArtBake.h"
#include "TitleArtRenderHost.h"
#include "titleart/TitleArtDoc.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct TitleBakeOptions {
        std::vector<std::string> outputs;   // ids; empty = every output
        bool force = false;                 // also fresh outputs
        bool dryRun = false;
        int bleedPx = TITLE_ART_BLEED_PX;
    };

    struct TitleBakeFile {
        std::string output;
        std::string path;                   // project-relative
        std::string guid;
        int width = 0, height = 0;
        titleart::TitleRectI frame;
        size_t bytes = 0;                   // the PNG file
        size_t vramEstimate = 0;            // RGBA with mips (x2 for 16-bit)
        double scale = 1.0;
        std::string status;                 // "written", "would_write", "fresh" (skipped)

        titleart::Json toJson() const;
    };

    struct TitleBakeResult {
        bool ok = false;
        std::string error;
        std::string code;                   // render_failed, bit_depth_unsupported, version_newer, write_failed...
        std::vector<TitleBakeFile> files;
        std::vector<std::string> warnings;
        titleart::TitleRectI frame;
        std::vector<std::filesystem::path> written;     // absolute

        titleart::Json toJson() const;
    };

    class TitleArtBakeJob {
    public:
        // `document`: absolute; `doc`: the snapshot to bake (the open document's or the file's)
        TitleArtBakeJob(Project* project, std::filesystem::path document, titleart::TitleArtDoc doc, TitleBakeOptions options);
        ~TitleArtBakeJob();

        // advances within about `budgetMs`; true when finished (ok or failed)
        bool step(double budgetMs);
        // to the end
        const TitleBakeResult& run();
        bool done() const { return finished; }
        // renders finished / renders in all
        float progress() const;
        std::string progressText() const;
        void cancel();
        const TitleBakeResult& result() const { return outcome; }
        const std::filesystem::path& document() const { return documentPath; }

    private:
        struct OutputWork;
        Project* project = nullptr;
        std::filesystem::path documentPath;
        std::shared_ptr<const titleart::TitleArtDoc> doc;
        TitleBakeOptions options;
        TitleBakeResult outcome;
        std::vector<std::unique_ptr<OutputWork>> work;
        size_t current = 0;
        int phase = 0;                      // 0 set up, 1 content renders, 2 frame renders, 3 write
        bool finished = false;
        bool cancelled = false;
        std::unique_ptr<titleart::TitleRenderJob> rendering;
        TitleBakeStatus status;

        bool setUp();
        bool advanceRender(double budgetMs, bool framePhase);
        bool startRender(OutputWork& output, bool framePhase);
        bool finishFrame();
        bool write();
        void fail(const std::string& code, const std::string& message);
    };

    // bakes a document to the end (the export, the CLI, edit_title's bake "now")
    TitleBakeResult bakeTitleArt(Project* project, const std::filesystem::path& document, const titleart::TitleArtDoc& doc,
                                 const TitleBakeOptions& options);
    // the document of a path for baking: the open document's snapshot, else the file's; false with
    // `error` (and `code`) when it does not load or is a newer version
    bool loadTitleArtForBake(Project* project, const std::filesystem::path& document, titleart::TitleArtDoc& out, std::string& code,
                             std::string& error);
    // the bake status of a document on disk with the project's assets
    TitleBakeStatus titleArtStatusOf(Project* project, const std::filesystem::path& document, const titleart::TitleArtDoc& doc);

    // Auto Bake (R34)
    class TitleArtAutoBake {
    public:
        static TitleArtAutoBake& get();

        static constexpr int DEBOUNCE_MS = 500;
        static constexpr double FRAME_BUDGET_MS = 6.0;

        // a document changed (commit, undo, redo, outside edit); it rebakes its stale outputs once
        // DEBOUNCE_MS passed without another change, when Auto Bake is on and an output exists
        void documentChanged(Project* project, const std::filesystem::path& document);
        // an asset changed: every document that uses it (the dependency index) is scheduled
        void assetChanged(Project* project, const std::filesystem::path& asset);
        // a bake asked for now (the Inspector's Bake, Missing Assets' Bake): queued before the
        // scheduled ones, whatever the Auto Bake setting; `outputs` empty = every output
        void requestBake(Project* project, const std::filesystem::path& document, std::vector<std::string> outputs, bool force);
        // a bake of `document` is queued or running
        bool baking(const std::filesystem::path& document) const;
        // the editor frame: starts due bakes and steps the running one within FRAME_BUDGET_MS
        void tick(Project* project);
        // drops what is scheduled and the running bake (project closed)
        void reset();
        bool idle() const { return !running && pending.empty() && asked.empty(); }
        size_t scheduled() const { return pending.size(); }
        uint64_t bakesDone() const { return completed; }
        // the path of the last bake that failed and why (Console has it too)
        const std::string& lastError() const { return failure; }

    private:
        struct Asked {
            std::filesystem::path document;
            std::vector<std::string> outputs;
            bool force = false;
        };
        std::vector<Asked> asked;
        std::map<std::string, std::chrono::steady_clock::time_point> pending;   // document -> due
        std::unique_ptr<TitleArtBakeJob> running;
        uint64_t completed = 0;
        std::string failure;
    };

}
