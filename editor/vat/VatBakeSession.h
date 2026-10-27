// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 4.2-4.5, 4.7)
// - Purpose: one Vertex Animation bake, the same for the baker window, Inspector actions, the
//   MCP tool and the CLI (the texsynth SynthesisSession pattern):
//     setSpec -> check() (spec 4.7 validation, before any sampling; lists the source's clips)
//             -> estimate() (sizes from vertex/bone counts and clip lengths)
//             -> start() -> advance(budgetMs) until it returns true (progress(), cancel())
//             -> result() (in memory: VatData, VatMeshData, payload, socket tracks, the report,
//                and a preview asset registered with VertexAnimationPool::put under previewKey)
//             -> write() (the files, one undo step).
// - Posing runs on the main thread inside advance(): at most `budgetMs` per call (0 = to the
//   end), each slice restoring the scene before it returns, so between two calls the scene is
//   at rest and editable. An open scene is checked with placementSceneEditStamp at every slice:
//   an edit in between fails the bake ("the scene changed during the bake: bake again"). Clip
//   and pose previews of the source are suspended first (PoseEditSession::suspendActiveFor, as
//   LatticeBake does). Encoding (computeVatBake) runs on a worker thread; advance() polls it.
//   cancel() stops either stage; nothing is written and the scene is at rest.
// - write(), spec 4.4: the output is spec.output or "<source dir>/<source stem>.vat". The
//   existing-file rule (PoseBaker's): a default path holding a .vat baked from the same recipe
//   source is rebaked (GUID and hand edits kept); any other existing file makes the bake take a
//   free name ("hero 2.vat"); an explicit path that exists is refused (output_exists) unless
//   `overwrite` (its GUID is kept, and its edits when it is from the same source). Images a
//   glTF embeds are written next to the .vat ("<stem>_<image>.png"). place none: WriteFilesCmd
//   on the project history; beside / replace / crowd: CreateVatCmd on the scene's history (a
//   new GameObject "<name> (VAT)" at the source's transform with Model = the .vat.glb and a
//   Vertex Animator = the .vat, selected; replace also disables the source; crowd needs the
//   VatCrowd placement and is refused until it is available).
// - Rebake (spec 4.5): loadRebake(vatPath) runs the .vat's recipe again; clip names, events,
//   speeds, wraps, interpolate and socket names are kept: clips are matched to the source's by
//   name, then the rest in order (preferring equal lengths); clips removed by hand stay removed.
// - Staleness: checkVatStaleness compares the recipe's file hashes and the .vat.glb.
// - Thread-safety: main thread, except the worker it owns. One session per bake.

#ifndef VAT_BAKE_SESSION_H
#define VAT_BAKE_SESSION_H

#include "vat/VatAssetWriter.h"
#include "vat/VatBakeCompute.h"
#include "vat/VatBakeSource.h"
#include "vat/VatBakeTypes.h"

#include "ecs/Entity.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine::vat{
    class VertexAnimation;
}

namespace jasperengine::editor{
    class Project;
}

namespace jasperengine::editor::vat{

    enum class VatBakeState{ Idle, Posing, Encoding, Done, Failed, Cancelled };

    struct VatBakeProgress{
        std::string stage;      // "Posing clip 2 of 5: Walk", "Encoding", "Done"
        float fraction = 0.0f;  // 0..1 over the whole bake
    };

    // the in-memory result of a finished bake
    struct VatBakeResult{
        // generated fields filled, layout hash set, recipe (with hashes) filled; guid and mesh
        // are set by write()
        jasperengine::vat::VatData data;
        VatMeshData mesh;
        jasperengine::vat::VatSocketTracks sockets;
        VatBakeReport report;
        std::vector<VatEmbeddedImage> embeddedImages;
        std::vector<std::string> boneNames;
        // the preview asset (owns the payload), registered as VertexAnimationPool::put(previewKey)
        std::shared_ptr<const jasperengine::vat::VertexAnimation> preview;
        std::string previewKey;
        // the source clip each baked clip came from (data.clips order)
        std::vector<std::string> sourceClips;

        const jasperengine::vat::VatPayload& payload() const;
    };

    struct VatWriteResult{
        std::string vat;        // project-relative
        std::string mesh;
        std::vector<std::string> files;     // every file written, project-relative
        bool rewritten = false; // an existing .vat of this source was rebaked (GUID kept)
        std::string guid;
        uint32_t sceneId = 0;   // the scene history the step went to (0: the project history)
        Entity entity = NULL_ENTITY;        // the placed GameObject
        std::string undoName;   // "Bake Vertex Animation 'assets/models/hero.vat'"
    };

    class VatBakeSession{
    public:
        explicit VatBakeSession(Project* project);
        ~VatBakeSession();
        VatBakeSession(const VatBakeSession&) = delete;
        VatBakeSession& operator=(const VatBakeSession&) = delete;

        // a new spec forgets the previous check, run and result (a running bake is cancelled)
        void setSpec(const VatBakeSpec& spec);
        const VatBakeSpec& spec() const{ return bakeSpec; }

        // A rebake of an existing .vat: its recipe as the spec (output = the file, overwrite),
        // the clip matching that keeps its hand edits. False with `error` when the file can't
        // be read or its recipe can't be run.
        bool loadRebake(const std::string& vatPath, std::string& error);

        // spec 4.7, before any sampling (resolves the source; cached until setSpec)
        const std::vector<VatIssue>& check();
        // the clips the source offers (after check), all of them, in source order
        const std::vector<VatSourceClip>& sourceClips() const;
        // sizes and the likely mode (after check; runs it when needed)
        VatEstimate estimate();

        // check() then the bake starts; false (state Failed, error()) when check has errors
        bool start();
        // see the contract; true once the bake is Done, Failed or Cancelled
        bool advance(double budgetMs);
        // start() + advance(0): the CLI's and tests' straight run. True when Done.
        bool run();
        void cancel();

        VatBakeState state() const{ return bakeState; }
        VatBakeProgress progress() const;
        const std::string& error() const{ return errorText; }
        // the errors that failed the bake (check's, or the bake's own: too_large, bone_limit...)
        const std::vector<VatIssue>& failureIssues() const{ return failure; }
        // after Done
        const VatBakeResult* result() const{ return bakeState == VatBakeState::Done ? &bakeResult : nullptr; }

        // the preview as files under <project>/.jasperengine/cache/vat-preview (not the
        // project's assets), so a Model can load the .vat.glb: absolute paths out
        bool writePreviewFiles(std::string& vatPath, std::string& meshPath, std::string& error);

        // where write() would write (the existing-file rule), without writing
        bool plannedOutput(std::string& vatPath, bool& rewrite, std::string& error);
        // the files, one undo step (see the contract). After Done.
        bool write(VatWriteResult& out, std::string& error);

    private:
        struct PlanItem{
            int clip = 0;           // into input.clips
            bool mid = false;
            int index = 0;          // sample or mid index
            float time = 0.0f;
        };

        Project* project = nullptr;
        VatBakeSpec bakeSpec;
        std::unique_ptr<VatBakeTarget> target;
        std::unique_ptr<VatPoser> poser;
        std::vector<VatIssue> checkIssues;
        bool checked = false;
        std::vector<int> selected;          // indices into target->clips, baking order

        VatBakeState bakeState = VatBakeState::Idle;
        std::string errorText;
        std::vector<VatIssue> failure;
        VatBakeInput input;
        std::vector<PlanItem> plan;
        size_t planDone = 0;
        uint64_t sceneStamp = 0;

        std::thread worker;
        std::atomic<bool> cancelFlag{false};
        std::atomic<bool> workerDone{false};
        std::atomic<float> workerProgress{0.0f};
        bool workerOk = false;
        std::string workerError;
        VatBakeOutput output;

        VatBakeResult bakeResult;
        uint64_t previewId = 0;

        // rebake: the .vat being rebaked and, per source clip name, the clip it keeps edits of
        std::string rebakePath;
        bool rebakeLoaded = false;
        jasperengine::vat::VatData rebakeOld;
        std::vector<std::pair<std::string, int>> rebakeClipMap;   // source clip name -> old clip index

        void reset();
        void finishFailed(const std::string& message, bool cancelled);
        void buildPlan();
        void startWorker();
        void finalize();
        bool stampChanged();
        void releasePreview();
    };

    // Staleness of a .vat (spec 4.5): [{kind vat-source-changed | vat-source-missing |
    // vat-mesh-mismatch | vat-layout-mismatch, message, fix}]; empty when up to date. A .vat that
    // doesn't parse gives vat-layout-mismatch with the parse error.
    std::vector<VatIssue> checkVatStaleness(Project* project, const std::string& vatPath);

    // Rebake straight through (CLI, MCP rebake, the Inspector button): loadRebake, run, write
    bool rebakeVat(Project* project, const std::string& vatPath, VatWriteResult& out, std::string& error,
                   std::vector<VatIssue>* warnings = nullptr);

    // which old clips (a .vat's) are which new clips: name first, then in order preferring equal
    // lengths; result[i] = the old clip index of new clip i, or -1
    std::vector<int> matchClips(const std::vector<jasperengine::vat::VatClip>& oldClips, const std::vector<std::string>& newNames,
                                const std::vector<float>& newLengths);

}

#endif //VAT_BAKE_SESSION_H
