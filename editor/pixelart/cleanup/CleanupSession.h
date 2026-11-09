// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The session layer of the Pixel Art Cleanup (docs/specs/S46-pixel-art-cleanup.md D2, §6.2): the one
// path from a request to written files, used by the Pixel Art Cleanup window, fix_pixel_art,
// fix-pixel-art, the texture import step and the cleanup_outputs validator rule, so a preview is
// the same pixels as a write everywhere.
//
//   Spec -> prepare (main thread: paths, the project, the palette asset, the source decoded)
//        -> run (any thread) -> Result -> encodeOutputs (any thread) -> writeUndoable | writeDirect
//
// A cancelled run writes nothing: encodeOutputs refuses a cancelled Result. A `needs_person` Result
// (R30) is refused by encodeOutputs too, unless Spec::force or Spec::person: the file then stores
// `forced: true` for force, nothing for a person. Errors are PixelProblem with R47's kinds.
//
// The same layer holds the lint side (LintSpec, prepareLint, runLint: lint_pixel_art, lint-pixel-art
// and the cleanup_outputs rule) and the pure import step of a texture (runImportStep, R33).

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/PixelProblem.h"
#include "pixelart/cleanup/CleanupAnimation.h"
#include "pixelart/cleanup/CleanupPipeline.h"
#include "pixelart/cleanup/CleanupRecipe.h"
#include "pixelart/cleanup/CleanupReport.h"
#include "pixelart/lint/PixelLint.h"
#include "spritenormals/SpriteNormalsSession.h"

#include "ecs/Entity.h"

#include "json.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart::cleanup {

    // ---- limits (R1, R45)

    constexpr int kMaxSourceSide = 4096;
    constexpr int kMaxSourceFrames = 256;
    constexpr int kMinPreviewWidth = 256;
    constexpr int kMaxPreviewWidth = 2048;
    constexpr int kDefaultPreviewWidth = 1024;

    // ---- what to do

    // Paths are project-relative, or plain paths without a project; "" takes R31's default
    struct OutputSpec {
        bool write = false;          // false: a preview, nothing is written
        std::string png, aseprite, json;
    };

    enum class SheetArg : uint8_t { Auto, Off, On };

    struct Spec {
        std::filesystem::path projectDir;  // the CLI's --project, read when `project` is null; "" plain files
        std::string source;                // an image, an .aseprite/.ase, or (rerun) an output; "" with `entity`
        uint32_t sceneId = 0;              // with entity: a Sprite GameObject's texture and frames
        Entity entity = NULL_ENTITY;
        std::optional<int> frame;          // an .aseprite source: this frame alone (1-based)
        std::string layer;                 // an .aseprite source: this layer alone ("" all visible)
        nlohmann::json recipe;             // R28, partial; null: every default
        nlohmann::json palette;            // overrides recipe.palette: a name, a .palette path, a list of "#rrggbb"
        SheetArg sheet = SheetArg::Auto;   // overrides recipe.sheet unless it is an object
        std::optional<OutputFormat> format;
        OutputSpec output;
        bool rerun = false;                // `source` is an output: its stored recipe and source
        bool overwrite = false;
        bool force = false;                // write a needs_person result, recording `forced` (R30)
        bool person = false;               // the window: a person writes whatever the verdict, nothing recorded (R30)
        bool failOnWarn = false;           // fail_on: warn (R29)

        bool hasSprite() const { return entity != NULL_ENTITY; }
    };

    // A spec resolved and its source decoded: plain data, any thread from here on
    struct Prepared {
        std::filesystem::path projectDir;     // "" plain files
        std::filesystem::path assetsDir;      // outputs stay beneath this ("" no containment)
        Spec spec;
        std::string sourcePath;               // project-relative, or as given
        std::filesystem::path sourceAbsolute;
        std::string sourceSha256;             // the source file's bytes
        std::vector<PixelImage> frames;       // one for a still picture; every frame of an animated source
        std::vector<int> delaysMs;            // an animated source's own delays (ms), else empty
        std::string animationName;            // one animation for an animated source: the file's name
        std::vector<SheetAnimation> tagAnimations;   // .aseprite: one per tag (empty when not a partition)
        Recipe recipe;                        // decoded, palette and source fields resolved
        ReportOptions report;                 // the backdrops (R26), failOnWarn
        std::vector<std::string> warnings;    // reported with the result
        // rerun: the output being re-run and the source hash it recorded
        std::string rerunOutput;              // project-relative ("" not a rerun)
        std::filesystem::path rerunOutputAbsolute;
        std::string recordedSha256;
    };

    // Reused between runs of the same Prepared in the window (R39): unchanged steps are not run again
    struct RunCache {
        PipelineCache pipeline;
        AnimationCache animation;
    };

    struct Stale {
        bool sourceChanged = false;           // rerun: the source's SHA-256 differs from the recorded one
    };

    // What a preview and a write share
    struct Result {
        PixelProblem problem;                 // a failed run (kind set); everything below is then default
        Report report;                        // verdict, confidence, guards, summary, grid, lints
        std::optional<Outcome> outcome;       // single pictures, for overlays
        std::optional<AnimationOutcome> animation;   // sheets and animated sources, for overlays
        Recipe resolved;                      // grid, palette, sheet and source resolved; re-runs alone (R28)
        std::vector<PixelImage> frames;       // the output frames (a sheet: every exported clip's, one PNG sheet in `frames[0]` only when written as PNG)
        OutputFormat format = OutputFormat::Png;
        std::vector<std::string> warnings;
        Stale stale;
        std::string defaultOutput;            // where a plain `output: true` writes
        bool cancelled = false;
    };

    // ---- the path of one request

    // Main thread. The bad_recipe problems all at once, or one problem; nullopt on any problem.
    // `project` may be null (the CLI: plain files, or spec.projectDir).
    std::optional<Prepared> prepare(Project* project, const Spec& spec, std::vector<PixelProblem>& problems);

    // Any thread. `cache` may be null; `cancel` is polled between steps and leaves Result::cancelled.
    Result run(const Prepared& prepared, RunCache* cache = nullptr, const std::atomic<bool>* cancel = nullptr);

    // {verdict, confidence, guards, summary, grid, output, palette?, animation?, lints, partial_alpha,
    //  specks?, background?, recipe, default_output, warnings, stale, cancelled}
    nlohmann::json toJson(const Prepared& prepared, const Result& result);

    // ---- the preview image (R45)

    enum class PreviewKind : uint8_t { None, Compare, Result, Grid, Lost, Lints };
    const char* previewKindName(PreviewKind kind);       // "none", "compare", "result", "grid", "lost", "lints"
    bool parsePreviewKind(const std::string& text, PreviewKind& out);

    // The preview at an integer scale fitting maxWidth (clamped to R45's range), with its scale in a
    // corner label; empty for None. No timestamp: the same run gives the same picture.
    PixelImage renderPreview(const Prepared& prepared, const Result& result, PreviewKind kind, int maxWidth = kDefaultPreviewWidth);

    // ---- writing

    using OutputFile = spritenormals::OutputFile;   // role "png", "aseprite", "meta", "json"

    // The files of the result, recipe included (R31). False with the problem, nothing to write:
    // cancelled, needs_person (R30), no_grid, output_exists, output_edited, output_is_source,
    // output_outside_assets. Any thread (it reads the disk: existing outputs, .meta files).
    bool encodeOutputs(const Prepared& prepared, const Result& result, std::vector<OutputFile>& files, PixelProblem& problem);

    // One step of the project history, "Pixel Art Cleanup (<file>)"; every file (.aseprite and its
    // .meta included) in that step; AsepriteImporter::reimport on write, undo and redo (R31). Main thread.
    bool writeUndoable(Project* project, const std::vector<OutputFile>& files, PixelProblem& problem);
    // All or nothing without history (the CLI)
    bool writeDirect(const Prepared& prepared, const std::vector<OutputFile>& files, PixelProblem& problem);

    // ---- recipes and staleness (R32)

    // The stored recipe of an output: a PNG's jasper.pixelcleanup chunk or an .aseprite's .meta
    std::optional<Recipe> readOutputRecipe(const std::filesystem::path& output, PixelProblem& problem);

    struct Staleness {
        enum class State { Fresh, Changed, Missing, NoRecipe, Invalid, Unreadable };
        State state = State::NoRecipe;
        std::string sourcePath;               // as recorded
        std::filesystem::path sourceAbsolute;
        bool outputEdited = false;            // the output's pixels no longer match result_sha256
        bool forced = false;                  // the recipe records `forced: true`
        std::string message;
    };
    const char* toString(Staleness::State state);    // "fresh", "changed", "missing", "no_recipe", "invalid", "unreadable"
    // projectDir empty: a relative source.path is resolved against the output's own directory
    Staleness checkStaleness(const std::filesystem::path& projectDir, const std::filesystem::path& output);

    // SHA-256 of the decoded RGBA pixels of a file: what result_sha256 records and output_edited compares
    std::optional<std::string> outputPixelsSha256(const std::filesystem::path& output);

    // ---- the import step (R33): pure, callable from the cook

    // Runs the recipe on `source`: sheets included, `out` then the packed sheet. False with `error`
    // when the recipe text is not a valid recipe; a run with a needs_person verdict still fills `out`
    // and returns true with `verdict` and `guard` (the first guard's id, "" when none).
    bool runImportStep(const PixelImage& source, const std::string& recipeText, PixelImage& out, Verdict& verdict, std::string& guard, std::string& error);

    // ---- linting (lint_pixel_art, lint-pixel-art, the cleanup_outputs rule)

    struct LintSpec {
        std::filesystem::path projectDir;      // the CLI's --project, read when `project` is null
        std::string source;                    // an image or an .aseprite/.ase; "" with `entity`
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        nlohmann::json frames;                 // null: every frame; a tag name; or a list of frames (1-based)
        std::string layer;                     // an .aseprite's layer ("" all visible)
        std::vector<std::string> lints;        // ids; empty: every applicable one
        nlohmann::json palette;                // a name, a .palette path, a list of "#rrggbb"
        lint::LintRules rules;
        std::vector<std::string> backdrops;    // "#rrggbb"
        std::optional<bool> loop;
        bool failOnWarn = false;               // fail_on: warn

        bool hasSprite() const { return entity != NULL_ENTITY; }
    };

    struct LintPrepared {
        std::filesystem::path sourceAbsolute;  // of the source file ("" a Sprite's)
        std::string sourcePath;                // project-relative, or as given
        std::vector<PixelImage> frames;
        std::vector<std::string> frameNames;
        std::optional<jasperengine::pixelart::Palette> palette;
        std::vector<Rgba8> backdrops;
        std::string backdropsSource;
        lint::LintRules rules;
        bool loop = true;
        std::vector<std::string> only;
        bool failOnWarn = false;
        std::optional<Recipe> outputRecipe;    // the source carries a cleanup recipe
        bool forced = false;                   // that recipe records `forced: true` (R30)
        std::string sheetLayout;               // "sheet" for a file reading as an animation sheet
    };

    struct LintOutcome {
        Verdict verdict = Verdict::Clean;
        lint::LintReport report;
        std::vector<PixelImage> frames;
        bool forced = false;
        std::string summary;
    };

    // Main thread (it reads the project for palettes and backdrops); `project` may be null
    std::optional<LintPrepared> prepareLint(Project* project, const LintSpec& spec, std::vector<PixelProblem>& problems);
    // Any thread: what `prepared` holds, linted. Unknown ids in `only` are prepareLint's to refuse.
    LintOutcome runLint(const LintPrepared& prepared);

    // {verdict, results, counts, frames, forced?}
    nlohmann::json toJson(const LintPrepared& prepared, const LintOutcome& outcome);

    // The frames with each listed spot in magenta and labelled S1, S2, ... (`image` of lint_pixel_art)
    PixelImage renderLintImage(const LintPrepared& prepared, const LintOutcome& outcome, int maxWidth = kDefaultPreviewWidth);

    // ---- wiring (done once by the editor and the CLI)

    // Installs jobs::parallelFor as detection's loop (R36); the results never depend on it (R40)
    void installEditorJobs();
    // Registers S42's cleanup_outputs rule (R49): editor startup and the CLI's check path
    void registerCleanupOutputsRule();

}
