// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor side of the texture synthesis, shared by the Texture Synthesis window
// (window/TextureSynthesisWindow) and the MCP tool synthesize_texture (mcp/tools/SynthesisTools.cpp),
// so both build the request, name and write the outputs and word the quality feedback the same way:
//
// - SynthesisSpec: what a synthesis is made of, in project-relative paths (what the window edits and
//   the tool's arguments become), and buildRequest() turning it into a texsynth::Request;
// - findCompanions(): the PBR maps next to an example (rock_albedo.png -> rock_normal.png, ...);
// - defaultOutputPath() / plannedOutputs(): the output naming (the core's companion rule);
// - encodeOutputs() + writeOutputsUndoable(): the PNGs with their recipe chunk, written as ONE step
//   of the project command history (undo removes the whole set);
// - assess(): the metrics in plain words with an actionable hint;
// - specFromRecipe(): a written PNG's recipe back into a spec (Edit Synthesis..., regenerate);
// - SynthesisRun: synthesize() on a worker thread with its progress, live preview and cancel flag.

#include "SynthesisRecipe.h"
#include "TextureSynthesis.h"

#include "json.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::texsynth {

    // Paths are project-relative with '/' ("assets/rock_albedo.png")
    struct ExampleSpec {
        std::string path;
        std::string sampleMask;   // optional: white may be copied, black never
        std::string guide;        // Guided mode, optional
        bool sampleable = true;
    };

    struct CompanionSpec {
        std::string name;                // "normal": output suffix and recipe role
        std::vector<std::string> paths;  // one per example
        std::string output;              // "" = the naming rule (companionOutputPath)
        bool enabled = true;
    };

    // Where the inpaint region comes from
    enum class MaskSource { Painted, Image, Alpha };

    struct SynthesisSpec {
        Mode mode = Mode::Tileable;
        std::vector<ExampleSpec> examples;
        std::vector<CompanionSpec> companions;
        MaskSource maskSource = MaskSource::Painted;
        std::string maskPath;                       // MaskSource::Image
        MaskChannel maskChannel = MaskChannel::Luma;
        bool maskInvert = false;
        std::shared_ptr<const Image> paintedMask;   // MaskSource::Painted: 8-bit grey, examples[0]'s size, white = regenerate
        std::string targetGuide;                    // Guided
        int width = 1024;                           // Extend, Tileable, Guided (0 = the target guide's size)
        int height = 1024;
        Quality quality = Quality::Balanced;
        ParamOverrides overrides;
        uint64_t seed = 0;
        std::string output;                         // the primary output, a project-relative .png
        std::string uncertaintyOutput;              // optional

        // Everything that changes the result, as text: the window's "stale" test
        std::string fingerprint() const;
    };

    // Default output size for the modes that take one
    inline constexpr int kDefaultSize = 1024;

    // ---- modes

    bool isTileableMode(Mode mode);        // Tileable, MakeTileable: the result is meant to repeat
    bool keepsExampleSize(Mode mode);      // MakeTileable, Inpaint
    const char* modeTitle(Mode mode);      // "Make Tileable"
    const char* modeHelp(Mode mode);       // one line

    // ---- paths and naming

    // `rel` resolved against the project, refused when absolute or climbing out of it
    bool resolveProjectFile(const std::filesystem::path& projectDir, const std::string& rel, std::filesystem::path& absolute,
                            std::string& error);
    std::string projectRelative(const std::filesystem::path& projectDir, const std::filesystem::path& absolute);
    // <example stem>_synth.png, or _tile.png for the tileable modes, next to the example; the
    // stem without its map word (rock_albedo.png -> rock_tile.png, companions rock_tile_normal.png)
    std::string defaultOutputPath(const std::string& examplePath, Mode mode);
    // The file a painted inpaint mask is saved to with the result: <output stem>_inpaint_mask.png
    std::string paintedMaskPath(const std::string& output);

    // A PBR map next to an example: same folder, same name up to the example's map keyword
    // (rock_albedo.png -> rock_normal.png, rock_roughness.png, rock_normal_gl.png; rock.png ->
    // rock_height.png), named by its suffix (companionSuffix). Only images whose suffix starts
    // with a map keyword (normal, roughness, height, ao, metallic, ...), so earlier outputs
    // (rock_albedo_synth.png) are not taken.
    struct CompanionCandidate {
        std::string name;
        std::string path;      // project-relative
        int width = 0;
        int height = 0;
    };
    std::vector<CompanionCandidate> findCompanions(const std::filesystem::path& projectDir, const std::string& examplePath);
    // Fills spec.companions with the companions every example has (matched by name); the maps of
    // another size than their example are left out with a warning, as are names some examples lack
    void autoCompanions(const std::filesystem::path& projectDir, SynthesisSpec& spec, std::vector<std::string>& warnings);

    // The files a synthesis of `spec` writes, in order: "primary", the enabled companions (their
    // names), "inpaint_mask" (a painted mask, saved so the recipe can name it) and "uncertainty"
    struct PlannedOutput {
        std::string role;
        std::string path;      // project-relative
    };
    std::vector<PlannedOutput> plannedOutputs(const SynthesisSpec& spec);

    // ---- request

    // Every problem of the spec that can be seen without decoding images (paths missing or outside
    // the project, outputs that are not .png or collide with an input, companion sizes)
    std::vector<Problem> checkSpec(const std::filesystem::path& projectDir, const SynthesisSpec& spec);
    // checkSpec, then the request (paths absolute); false with the problems
    bool buildRequest(const std::filesystem::path& projectDir, const SynthesisSpec& spec, Request& request, std::vector<Problem>& errors);

    // ---- outputs

    struct OutputFile {
        std::string role;
        std::string path;                  // project-relative
        std::filesystem::path absolute;
        std::string bytes;                 // PNG, with the jasper.synthesis recipe (not the inpaint mask, a source)
    };
    // The PNGs of a finished synthesis of `spec` (request: the one synthesize() ran, holding the
    // loaded inputs and their hashes). A painted mask is encoded too and recorded in the recipe as
    // the inpaint mask file.
    bool encodeOutputs(const std::filesystem::path& projectDir, const SynthesisSpec& spec, Request& request, const Result& result,
                       std::vector<OutputFile>& files, std::string& error);
    // Writes them as one step of `project`'s command history (undo deletes or restores the whole
    // set; ProjectFileWrites::writeUndoable, no MCP server needed), then lets the editor notice them
    // (textures reloaded, the asset database, thumbnails, Project window; with the MCP server
    // running, its file sync). Every write, removal and restore stays beneath the project (or its
    // assets directory, when every file is in it) when it happens: a file resolving outside it
    // through a symbolic link is refused, and no link planted on the way since is followed
    // (WriteFilesCmd::writeBeneath, util/SafeWrite.h). Main thread.
    bool writeOutputsUndoable(Project* project, const std::vector<OutputFile>& files, std::string& error);

    // ---- feedback

    // "none (0.95)", "faint (1.32)", "visible (2.10)"
    std::string seamWords(double seamScore);
    struct Assessment {
        std::vector<std::string> lines;    // "Seams: none (0.95)", "Uncertain pixels: 2%"
        std::string hint;                  // what to try next ("" when it looks fine)
        bool seamVisible = false;
        bool highUncertainty = false;
    };
    Assessment assess(const SynthesisSpec& spec, const Result& result);

    // ---- recipes

    // A written PNG's recipe (readRecipe) back into a spec; `notes` say what could not be restored
    // (a source given in memory). False with `error` when the recipe can't be used.
    bool specFromRecipe(const nlohmann::json& recipe, const std::filesystem::path& pngFile, const std::filesystem::path& projectDir,
                        SynthesisSpec& spec, std::vector<std::string>& notes, std::string& error);

    // ---- project switches

    // The editor is about to open another project (App::prepareForProjectSwitch): every run is
    // cancelled and projectGeneration() grows. Whoever writes a run's result checks the
    // generation it started in, so a result finishing later never lands in the next project.
    void projectSwitching();
    uint64_t projectGeneration();

    // ---- worker

    // synthesize() on a detached worker thread. The run keeps itself alive until it finishes, and
    // the thread never lets an exception out: an out-of-memory run (or one whose thread can't be
    // created) finishes with Status::Failed.
    class SynthesisRun : public std::enable_shared_from_this<SynthesisRun> {
    public:
        struct Options {
            double previewIntervalSeconds = 0.0;   // 0: no live previews
            int previewMaxSize = 512;
            int threads = 0;                       // 0: all cores
            // Called on the worker thread with each progress report (cheap: copy what it needs)
            std::function<void(const Progress&)> onProgress;
        };
        using Finished = std::function<void(SynthesisRun& run)>;

        // onFinished runs on the worker thread once result() is ready (on the calling thread when
        // no thread could be started)
        static std::shared_ptr<SynthesisRun> start(Request request, const Options& options, Finished onFinished = nullptr);
        // Editor shutdown: no callback runs any more, every running synthesis is cancelled and
        // waited for (they end within about a second); false when one outlived the timeout
        static bool cancelAll(double timeoutSeconds = 30.0);
        // Cancels every running synthesis without waiting (a project switch)
        static void cancelRunning();

        void cancel() { cancelFlag = true; }
        bool cancelRequested() const { return cancelFlag.load(); }
        bool finished() const { return done.load(); }
        // The latest progress report (its preview may be null: see latestPreview)
        Progress progress() const;
        // The newest partial-output snapshot and a counter that grows with each new one
        std::shared_ptr<const Image> latestPreview(uint64_t* serial = nullptr) const;
        // After finished(): the request (inputs loaded, hashed) and the result
        Request& request() { return req; }
        const Result& result() const { return res; }

    private:
        SynthesisRun() = default;
        Request req;
        Result res;
        std::atomic<bool> cancelFlag{false};
        std::atomic<bool> done{false};
        mutable std::mutex mutex;
        Progress last;
        std::shared_ptr<const Image> preview;
        uint64_t previewSerial = 0;
    };

}
