// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCEDURALRUN_H
#define PROCEDURALRUN_H

// One evaluation of a Procedural Material in the editor, with its lit ball when asked for
// (docs/specs/S90-procedural-material.md R34, R35, R38): preview_procedural_material, bake_procedural_material
// and, later, the window's Bake button go through it.
//
// - A plan runs on the editor's own device through ProceduralEvaluator, a slice in each editor frame
//   (App::engineRender, outside any render pass), its images read back.
// - The ball is a MaterialRender of the images (or of the material given), drawn again each frame until
//   isPending() says it shows that material, then read back. Its textures have ids of this run's own, so a
//   texture of an earlier run can never stand in for one of this run.
// - Past timeoutSeconds the run fails: a preview never shows an older image than the one asked for.
//
// Main thread only.

#include "ProceduralEvaluator.h"
#include "PassPlanner.h"

#include "texture/Material.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::procmat {

    struct ProceduralRunRequest {
        // a graph's plan, evaluated on the editor's device; none: `images` are the material's already
        std::optional<PassPlan> plan;
        std::vector<OutputImage> images;
        // draw the lit ball
        bool sphere = false;
        // the ball's material when no plan runs (a .material, a submesh's); with a plan it is made from
        // the outputs
        Material material;
        double timeoutSeconds = 120.0;
        // tests: while this file exists the ball counts as still pending (a slow shader variant)
        std::filesystem::path pendingMarker;
    };

    class ProceduralRun {
    public:
        static constexpr double FRAME_BUDGET_MS = 8.0;

        explicit ProceduralRun(ProceduralRunRequest request);
        ~ProceduralRun();
        ProceduralRun(const ProceduralRun&) = delete;
        ProceduralRun& operator=(const ProceduralRun&) = delete;

        // Its work runs in the editor's frames from now on
        void start();
        void cancel();
        bool isFinished() const { return finished; }
        bool isFailed() const { return !error.empty(); }
        const std::string& getError() const { return error; }
        std::string getStage() const;

        // the evaluated (or given) images, in plan order
        const std::vector<OutputImage>& getOutputs() const { return outputs; }
        // the lit ball, RGBA8 rows top first; empty without one
        const OutputImage& getSphere() const { return sphereImage; }
        // the editor's device before the evaluation, the one it ran on after (cpu after a device failure)
        EvaluatorTier tier() const;
        std::string deviceName() const;
        // a device failure the evaluation continued past on the CPU (R9): the pass, device and log
        const std::vector<std::string>& getWarnings() const { return warnings; }

        // Called when the run ends (ok or not), on the main thread from processFrame
        void setOnFinished(std::function<void()> done) { onFinished = std::move(done); }
        // Called with getStage() each time it changes
        void setOnStage(std::function<void(const std::string&)> changed) { onStage = std::move(changed); }

        // The editor calls these from its frame: advance every started run; true while one still runs.
        // setFrameRequester keeps an idle editor drawing.
        static bool processFrame();
        static void setFrameRequester(std::function<void()> requester);
        // the graphics view goes away (editor shutdown): every run fails, the GPU objects go
        static void releaseGpu();
        // the editor is about to open another project (App::prepareForProjectSwitch): every run fails
        static void projectSwitching();

    private:
        struct Impl;

        void advance(double budgetMs);
        void finish(const std::string& failure);

        std::unique_ptr<Impl> impl;
        ProceduralRunRequest request;
        std::vector<OutputImage> outputs;
        std::vector<std::string> warnings;
        OutputImage sphereImage;
        std::string error;
        bool started = false;
        bool finished = false;
        std::function<void()> onFinished;
        std::function<void(const std::string&)> onStage;
    };

}

#endif //PROCEDURALRUN_H
