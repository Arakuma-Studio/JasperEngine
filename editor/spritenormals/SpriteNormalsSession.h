// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor side of the sprite normal generator, shared by the Sprite Normal Map window
// (Phase 4), the MCP tool generate_sprite_normals and the CLI generate-sprite-normals (Phase 3),
// so all of them resolve sources and frames, name and write outputs, and word feedback the same:
//
// - Spec: what a generation is made of (project-relative paths, settings, frames, outputs);
// - checkSpec(): every problem visible without decoding pixels; prepare(): load + Request;
// - plannedOutputs() / defaultOutputs(): the naming of contract section 5;
// - encodeOutputs() + writeOutputsUndoable(): the PNGs with their recipe (and the frames JSON),
//   ONE step of the project command history; writeOutputsDirect() for the CLI;
// - checkStaleness() / specFromRecipe(): written maps back to their source and settings;
// - framesFromSprite(), lightCheck(), sceneWarnings(): what a scene says about the result;
// - SpriteNormalsRun: generate() on a worker thread;
// - the paint layer (Phase 5): Spec::paint / paintMode, the starting overlay prepare() loads,
//   generatePrepared() compositing it, encodeOutputs() writing its chunk.

#include "SpriteMedia.h"
#include "SpriteNormals.h"
#include "SpriteNormalsPaint.h"
#include "SpriteNormalsPreview.h"
#include "SpriteNormalsRecipe.h"
#include "util/SafeWrite.h"

#include "ecs/Entity.h"
#include "json.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::spritenormals {

    // The refusal (kind project_changed) of a run's result when another project was opened
    // meanwhile (run->generation() != texsynth::projectGeneration()): every surface words it so
    inline constexpr const char* kProjectChangedMessage = "Another project was opened while generating.";

    // How the frames are chosen (contract `frames`)
    struct FramesSpec {
        enum class Mode { Auto, None, Grid, Cells };
        Mode mode = Mode::Auto;
        Grid grid;                   // Mode::Grid
        std::vector<Cell> cells;     // Mode::Cells (a Sprite's frames, or given rects)
        FramesFrom from = FramesFrom::Given;   // Mode::Cells: what the cells are (Sprite or Given), recorded
    };

    // Output paths, project-relative ('/'); "" = the default name (contract section 5)
    struct OutputSpec {
        std::string normal;
        std::string atlas;           // animated sources only
        std::string height;          // settings.height only
        bool framesJson = false;
        std::string framesJsonPath;  // "" = <atlas stem>.frames.json (or <normal stem>.frames.json)
    };

    struct Spec {
        std::string source;          // project-relative image path; "" when sprite is set (its texture)
        uint32_t sceneId = 0;        // with entity: a Sprite (or any mesh) GameObject as the source
        Entity entity = NULL_ENTITY;
        unsigned submesh = 0;        // with entity: the submesh whose base colour texture is the source
        Settings settings;
        FramesSpec frames;
        OutputSpec output;
        bool overwrite = false;
        // Phase 5: paint operations on the output (atlas cells for animations), and where the
        // starting overlay comes from: Keep = the paint chunk of the file at the planned normal
        // output (a preview reads the file the default output would write), Replace = none,
        // Clear = none and no ops (the written map has no paint). The window paints its own
        // overlay and generates with Replace and no ops.
        std::vector<PaintOp> paint;
        PaintMode paintMode = PaintMode::Keep;

        bool hasSprite() const { return entity != NULL_ENTITY; }
        // Everything that changes the result, as text (the window's "stale" test; submesh and paint
        // included)
        std::string fingerprint() const;
    };

    // ---- paths and naming

    // `rel` against the project; refused when absolute, climbing out of it or inside a hidden
    // directory (the texsynth rules)
    bool resolveProjectFile(const std::filesystem::path& projectDir, const std::string& rel, std::filesystem::path& absolute,
                            std::string& error);
    std::string projectRelative(const std::filesystem::path& projectDir, const std::filesystem::path& absolute);
    // The link-resolving walk and containment test are the shared ones (util/SafeWrite.h: the MCP
    // write sandbox uses them too): resolvedPath follows every symbolic link (and junction) on the
    // way, a dangling one too (empty when a link loops); isInsideResolved compares resolved paths
    using safewrite::resolvedPath;
    using safewrite::isInsideResolved;

    struct PlannedOutput {
        std::string role;            // "normal", "atlas", "height", "frames_json"
        std::string path;            // project-relative
        // role normal, paint mode Keep: the paint the file there already holds (readPaintSummary)
        std::optional<PaintSummary> existingPaint;
    };
    // Contract section 5 names for a source: <stem>_normal.png, or <stem>_sheet.png +
    // <stem>_sheet_normal.png for GIF/APNG; <stem>_height.png / <stem>_sheet_height.png;
    // <stem>_sheet.frames.json (animations) or <stem>_normal.frames.json (stills)
    std::vector<PlannedOutput> defaultOutputs(const std::string& sourcePath, SourceKind kind, bool height, bool framesJson);
    // The files `spec` writes (its explicit paths, else the defaults), in the order above; with
    // paint mode Keep the normal output's existingPaint (a head read of that file)
    std::vector<PlannedOutput> plannedOutputs(const std::filesystem::path& projectDir, const Spec& spec, SourceKind kind);

    // ---- frames

    // The Sprite's frames (SpriteComponent framesRect, pixels; only when submesh is 0, else no
    // cells) and the colour texture of `submesh` (its base colour, project-relative); false with
    // `error` (kind not_a_sprite / invalid_source)
    bool framesFromSprite(const Scene* scene, Entity entity, unsigned submesh, std::string& texturePath,
                          std::vector<Cell>& cells, std::string& error);

    // ---- check and prepare

    // Every problem visible without decoding pixels: settings, source missing / outside the
    // project / unsupported (probeSource), frames (a grid or none on a GIF/APNG: frames_mismatch;
    // grid empty or too many cells; overlapping Sprite frames: frames_mismatch), outputs (not
    // .png, outside the project's assets directory, lexically or once symbolic links are resolved
    // (isInsideResolved), colliding with the source or each other (links resolved too), existing
    // without overwrite: output_exists, not valid UTF-8), paint (invalid_paint keyed
    // paint[i].<field>: ranges, counts, directions; ops with paint mode Clear)
    std::vector<Problem> checkSpec(const std::filesystem::path& projectDir, const Spec& spec, const Scene* scene = nullptr);

    // A spec resolved and loaded: what generate() runs and encodeOutputs() writes
    struct Prepared {
        std::filesystem::path projectDir;
        Spec spec;
        std::string sourcePath;              // project-relative (or absolute outside a project)
        std::filesystem::path sourceAbsolute;
        Source source;                       // decoded
        FramesFrom framesFrom = FramesFrom::Single;
        std::string spriteEntityPath;        // hierarchy path when from a Sprite
        Request request;
        std::vector<PlannedOutput> outputs;
        std::vector<Cell> plannedSpriteCells;   // preparePlan: the Sprite's frames for prepareLoad
        // Phase 5: the starting overlay (paint mode Keep: the planned normal output's paint chunk)
        // and the ops; after generatePrepared() the overlay with the ops applied and their stats.
        // encodeOutputs() writes this overlay into the normal map.
        PaintLayer paint;
        std::string paintKeptFrom;           // project-relative file the overlay was read from ("" none)
        std::vector<Problem> paintWarnings;  // paint_unreadable (dropped), paint_layout_changed
    };
    // checkSpec, then the source decoded and the Request built; false with the problems. With
    // paint mode Keep the existing normal map's paint is loaded: an overlay of another size than
    // the new output is paint_size_mismatch (nothing to write), a chunk that can't be read a
    // paint_unreadable warning (the overlay starts empty), other frame cells than the recipe's a
    // paint_layout_changed warning. Ops naming a frame that is not one: invalid_paint.
    bool prepare(const std::filesystem::path& projectDir, const Spec& spec, const Scene* scene, Prepared& out,
                 std::vector<Problem>& problems);
    // prepare() in two halves, for a worker thread: preparePlan is checkSpec's whole work (main
    // thread: it reads the project, the Platform's asset path and the scene) and fills everything
    // but the decoded source and the Request; prepareLoad then needs nothing but `planned`'s plain
    // data (any thread: it never touches System, Project or a scene): the source decoded, the
    // Request built. prepare() == preparePlan() + prepareLoad().
    bool preparePlan(const std::filesystem::path& projectDir, const Spec& spec, const Scene* scene, Prepared& out,
                     std::vector<Problem>& problems);
    bool prepareLoad(Prepared& planned, std::vector<Problem>& problems);

    // ---- outputs

    struct OutputFile {
        std::string role;
        std::string path;                    // project-relative
        std::filesystem::path absolute;
        std::string bytes;                   // PNG with its recipe, or the frames JSON
    };
    // The normal map carries prepared.paint.overlay as its paint chunk (none when empty) and the
    // recipe "paint":{"painted":N}. `problem` (optional): the kind (paint_too_large, write_failed).
    bool encodeOutputs(const Prepared& prepared, const Result& result, std::vector<OutputFile>& files, std::string& error,
                       Problem* problem = nullptr);
    // Every file inside `project`'s assets directory, also once symbolic links are resolved
    // (outputs are checked by checkSpec; this checks them again where they are written: a link
    // made since). False with the problem (output_invalid, field output.<role>) otherwise.
    bool outputsContained(Project* project, const std::vector<OutputFile>& files, Problem& problem);
    // One step of the project command history (ProjectFileWrites::writeUndoable: no MCP server
    // needed), then the editor notices them (textures reloaded, the asset database, thumbnails,
    // the Project window, the watcher; with the MCP server running, its file sync). The files are
    // checked with outputsContained first (refused: `problem` output_invalid, nothing written) and
    // again before each redo and undo of the step (refused: that step is not applied, said in the
    // console with the fix; a refused redo or undo leaves the history where it is). Every write,
    // removal and restore stays beneath the assets directory when it happens (outputsRoot,
    // WriteFilesCmd::writeBeneath). `problem` (optional): the refusal,
    // or write_failed. Main thread.
    // The step is named "Generate Normal Map <file name of the first normal output>" ("Generate
    // Normal Maps (N)" for N normal outputs).
    bool writeOutputsUndoable(Project* project, const std::vector<OutputFile>& files, std::string& error, Problem* problem = nullptr);
    // All or nothing without history (the CLI, the watcher): a WriteFilesCmd executed alone (a
    // failure puts back what was written before it; `failedPath` and `error` say which file and
    // why, "...; nothing was written"). Every write and rollback stays beneath `beneath` when it
    // happens (util/SafeWrite.h: a link planted on the way since the check is never followed, a
    // link at a file's name is replaced); empty (the CLI): each file beneath its own folder once resolved.
    bool writeOutputsDirect(const std::vector<OutputFile>& files, std::string& failedPath, std::string& error,
                            const std::filesystem::path& beneath = {});
    // The folder `project`'s outputs must stay in (its assets directory, under the project's
    // spelling; outputsContained checks against it); empty without a project
    std::filesystem::path outputsRoot(Project* project);

    // The frames JSON (contract section 5): {"version":1, "source":<recipe path>, "width":W,
    // "height":H, "animation":{"frame_width", "frame_height", "columns", "rows", "frame_count",
    // "delays_ms":[[num,den],...], "frame_ms":[float,...], "loop":<loopCount>},
    // "settings":settingsToJson}; a still with cells has "animation" with frame_count = cells,
    // delays_ms/frame_ms/loop omitted
    nlohmann::json framesJson(const Prepared& prepared);

    // ---- recipes and staleness

    struct Staleness {
        // NoRecipe: a PNG without a recipe chunk; Invalid: the recipe chunk is garbled;
        // Unreadable: the file can't be opened or is not a PNG (no recipe to speak of, not a
        // garbled one: never report it as Invalid)
        enum class State { Fresh, Changed, Missing, NoRecipe, Invalid, Unreadable };
        State state = State::NoRecipe;
        std::string sourcePath;              // as recorded
        std::filesystem::path sourceAbsolute;
        std::string message;                 // "hero.png changed since this map was made"
    };
    const char* toString(Staleness::State state);   // "fresh", "changed", "missing", "no_recipe", "invalid", "unreadable"
    // projectDir empty: a recipe source.path is resolved against the PNG's own directory
    Staleness checkStaleness(const std::filesystem::path& projectDir, const std::filesystem::path& png);

    // A written map's recipe back into a spec (same settings, frames, outputs = that set,
    // overwrite on); `notes` say what could not be restored (a Sprite no longer in the scene ->
    // the texture path is used). False with `error` when the recipe can't be used.
    bool specFromRecipe(const nlohmann::json& recipe, const std::filesystem::path& png, const std::filesystem::path& projectDir,
                        Spec& spec, std::vector<std::string>& notes, std::string& error);

    // ---- what the scene says

    struct LightCheck {
        int lights = 0;                      // enabled Light 2Ds
        int lightsWithHeight = 0;            // ... with height > 0 (they light normal maps)
        float ambientIntensity = 0.0f;       // scene ambient 2D intensity
        float ambientBrightness = 0.0f;      // ambientLight2DBrightness (util/Ambient2D.h)
    };
    LightCheck lightCheck(const Scene* scene);

    // The larger side of a frame in px: of the largest cell, else of the whole image
    int framePixels(const std::vector<Cell>& cells, int width, int height);
    // light_low (S78 R14): the enabled Light 2Ds with a height above 0 but under half the
    // sprite's larger side in world units, which light a big sprite from the side only. The
    // sprite's size: a Sprite's drawn size (its pixels over its Pixels Per Unit), else
    // `framePixels` over the entity's (a Tilemap's) or the project's Pixels Per Unit; times the
    // entity's world scale. `raiseTo` is that larger side rounded up (to whole units from 10, to
    // 0.01 below), the height the fix sets. Scene null: none.
    struct LowLights {
        std::vector<Entity> lights;          // in the scene's component order
        float spriteSize = 0.0f;             // the larger side, world units
        float raiseTo = 0.0f;
    };
    LowLights lowLights(const Scene* scene, Entity entity, int framePixels);
    // The scene-aware warnings of section 8.2, the only ones every surface shows: no_light (no
    // enabled Light 2D), no_light_height (lights > 0 but none with height), light_low
    // (lowLights, with `framePixels`; 0: the normal map's larger side), ambient_high
    // (isAmbientLight2DBright), and for an entity: size_mismatch (its base texture size !=
    // normal size), filter_mismatch (a normal texture set, or the result will be applied, and its min/mag
    // filter or wrap differ from the base texture's; apply copies them, so a willApply result is
    // compared after that copy), material_linked (submesh material from a .material file),
    // too_many_frames (delays.size() > 128), directx_in_engine (settings.directx),
    // shader_ignores_normals (a manifest shader for which !Catalog::shaderReadsMaterialNormalMap);
    // and zero_delay (some delay's playbackMs replaced a delay under 10 ms by 100). Scene null:
    // only zero_delay. `submesh`: the submesh the map is for (size, filter and material checks).
    std::vector<Problem> sceneWarnings(const Scene* scene, Entity entity, const Settings& settings, int normalWidth,
                                       int normalHeight, const std::vector<Delay>& delays, bool willApply,
                                       unsigned submesh = 0, int framePixels = 0);

    // ---- generation

    // generatePainted(prepared.request, prepared.paint): the generation with the paint layer
    // composited (exactly generate() without paint); prepare's paint warnings first in
    // result.warnings. What SpriteNormalsRun and the CLI run.
    Result generatePrepared(Prepared& prepared, const Control& control = Control());

    // ---- worker

    // generatePrepared() on a detached worker thread (the texsynth SynthesisRun pattern): the run keeps
    // itself alive until it finishes; no exception escapes (out of memory -> Status::Failed).
    class SpriteNormalsRun : public std::enable_shared_from_this<SpriteNormalsRun> {
    public:
        using Finished = std::function<void(SpriteNormalsRun& run)>;
        // onFinished runs on the worker thread once result() is ready
        static std::shared_ptr<SpriteNormalsRun> start(Prepared prepared, int threads = 0, Finished onFinished = nullptr);
        // Editor shutdown: callbacks stop, every run cancelled and waited for; false on timeout
        static bool cancelAll(double timeoutSeconds = 10.0);
        // A project switch: cancel without waiting
        static void cancelRunning();

        void cancel() { cancelFlag = true; }
        bool finished() const { return done.load(); }
        Progress progress() const;
        // texsynth::projectGeneration() when the run started
        uint64_t generation() const { return startGeneration; }
        const Prepared& prepared() const { return prep; }
        const Result& result() const { return res; }

    private:
        SpriteNormalsRun() = default;
        Prepared prep;
        Result res;
        uint64_t startGeneration = 0;
        std::atomic<bool> cancelFlag{false};
        std::atomic<bool> done{false};
        mutable std::mutex mutex;
        Progress last;
    };
}
