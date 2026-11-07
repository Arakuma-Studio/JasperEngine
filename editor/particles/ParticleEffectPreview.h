// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLEEFFECTPREVIEW_H
#define PARTICLEEFFECTPREVIEW_H

// A deterministic run of an effect in private scenes (docs/specs/S91-effect-authoring.md R24, R26-R29,
// D3, D4): preview_particle_effect, the Particle Effect window's Filmstrip and Checks, Bake Sprite
// Sheet and check --particles all go through it, so they show the same rows.
//
// - Pass 1 steps every system tick by tick from its seed (no world budget, colliders or force
//   fields) and measures the count, births, bounds and pops; a second run of bare runtimes with
//   Max Particles lifted counts the births it refused.
// - The whole run's bounds frame the camera once.
// - Pass 2 builds the scene again, steps it the same way and, at each cell's time, draws it twice
//   (on black and on white: the alpha follows from the difference) into an offscreen target, as a
//   view of its own (Engine::drawView) from the editor's frame (processFrame), so it never depends
//   on a shown view and requests the frames it needs. A cell waits for loading meshes, textures
//   and pipelines (a stand-in is never read back) and the run fails past RENDER_TIMEOUT_SECONDS.
//   The analytic overdraw and coverage are taken every tick of pass 2 with that camera.
// - The state hash at pass 2's last tick must equal pass 1's: the same request gives the same bytes
//   on one device, and a run that did not repeat fails instead of showing something else.
//
// frames 0 without overdraw_map draws nothing and runs anywhere, with no graphics view (tests, the
// CLI): runCpu. Main thread only.

#include "particles/ParticleEffect.h"
#include "particles/ParticleEffectChecks.h"
#include "particles/ParticleQaSettings.h"
#include "math/Matrix4.h"
#include "pose/PoseImage.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct ParticlePreviewSystem {
        std::string name;                              // the GameObject path in the subtree, or the asset
        std::string asset;                             // assets-relative .particles (empty: inline)
        particles::ParticleEffectDocument document;    // what it plays (its exposed values applied)
        Matrix4 transform;                             // relative to the subtree's root
        uint32_t seed = 0;                             // the component's pinned seed (0: none)
    };

    struct ParticlePreviewRequest {
        std::vector<ParticlePreviewSystem> systems;
        uint32_t seed = 0;              // 0: each system's own (its pinned seed, its effect's, else 1 + index)
        std::vector<double> times;      // the cells' times; empty: `frames` spread over the run
        // 0 to 24; 0: metrics only. Each cell is the middle of its equal part of the run (a flipbook's
        // frames; neither the empty start nor the over end of a one-shot)
        int frames = 6;
        double seconds = 0.0;           // 0: one play (a cycle of a looping effect, else until it is over)
        std::string view = "three_quarter";   // front, side, top, three_quarter
        std::string background = "dark";      // dark, light, checker
        bool overdrawMap = false;
        std::string platform = "mobile";
        int cellSize = 192;
        // a sheet's cells: the straight-alpha colour (Bake Sprite Sheet) instead of a background
        bool transparentCells = false;
        bool labels = true;             // time and count under each cell
        ParticleQaSettings qa;
        std::map<std::string, int> rounds;   // consecutive FAILs before this run, by check id
        PoseImage reference;            // drawn as one more cell, labelled "reference"
    };

    class ParticleEffectPreview {
    public:
        static constexpr int MAX_FRAMES = 24;
        static constexpr double MAX_SECONDS = 60.0;
        static constexpr double ONE_SHOT_LIMIT = 30.0;   // past its duration, a one-shot that has not ended
        static constexpr double RENDER_TIMEOUT_SECONDS = 120.0;
        static constexpr double FRAME_BUDGET_MS = 12.0;
        static constexpr int GRID = 64;

        explicit ParticleEffectPreview(ParticlePreviewRequest request);
        ~ParticleEffectPreview();
        ParticleEffectPreview(const ParticleEffectPreview&) = delete;
        ParticleEffectPreview& operator=(const ParticleEffectPreview&) = delete;

        // A request with a mistake (no system, an effect that does not compile, frames out of
        // range): the error, else empty
        static std::string validate(const ParticlePreviewRequest& request);

        bool needsGpu() const;
        bool isFinished() const { return finished; }
        bool isFailed() const { return !error.empty(); }
        const std::string& getError() const { return error; }
        // 0..1 and what it does now
        double getProgress() const;
        std::string getStage() const;
        void cancel();

        // seed, ticks, metrics, checks, verdict, thresholds, guidance, cells, state_hash
        const qa::Json& getReport() const { return report; }
        const ParticleMeasurements& getMeasurements() const { return measured; }
        // the filmstrip: the cells in a row (the heat row under them with overdraw_map), labelled;
        // empty with frames 0
        const PoseImage& getImage() const { return filmstrip; }
        // the cells without labels or background: straight alpha (transparentCells)
        const std::vector<PoseImage>& getCells() const { return cells; }
        // the height of the view (world units) at the framed centre: a cell's size in the world
        float getFrameWorldSize() const;
        // the framed centre, relative to the source's origin (the subtree's root)
        Vector3 getFrameCenter() const;

        // Called when the run ends (ok or not), on the main thread from processFrame
        void setOnFinished(std::function<void()> done) { onFinished = std::move(done); }
        // Starts it: its work runs in the editor's frames from now on
        void start();

        // Metrics only (frames 0, no overdraw_map), now, without a graphics view
        static bool runCpu(const ParticlePreviewRequest& request, qa::Json& report, std::string& error);

        // The editor calls these from its frame (App::engineRender): advance every started run;
        // true while one still runs. setFrameRequester keeps an idle editor drawing.
        static bool processFrame();
        static void setFrameRequester(std::function<void()> requester);
        static void setInEditorFrame(bool inFrame);
        // the graphics view goes away (editor shutdown): every run fails, the GPU objects go
        static void releaseGpu();

        struct Impl;

    private:
        std::unique_ptr<Impl> impl;
        ParticlePreviewRequest request;
        ParticleMeasurements measured;
        qa::Json report;
        PoseImage filmstrip;
        std::vector<PoseImage> cells;
        std::string error;
        bool started = false;
        bool finished = false;
        std::function<void()> onFinished;

        // one slice of work; true when the run ended
        bool advance(double budgetMs, bool drawing);
        void finish(const std::string& failure);
        void buildReport();
        void composeFilmstrip();
    };

}

#endif //PARTICLEEFFECTPREVIEW_H
