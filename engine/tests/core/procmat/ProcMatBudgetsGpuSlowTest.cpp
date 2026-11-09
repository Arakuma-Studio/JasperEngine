// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S90 R23 to R25, AC17: the editor's budgets for a Procedural Material, measured on the 30-node graph
// of tools/fixtures/procmat-baked (docs/perf/procedural-material.md). The device here is the headless
// one (llvmpipe or lavapipe: tier 2); tier 3 is the CPU evaluator on this process. Each prints a
// [bench] line; the wall-clock budgets are enforced only with JASPER_TEST_PERF_SLACK (perfSlack()).

#include "../../../../editor/procmat/NodeLibrary.h"
#include "../../../../editor/procmat/PassPlanner.h"
#include "../../../../editor/procmat/ProcMatThumbnails.h"
#include "../../../../editor/procmat/ProceduralCpuEvaluator.h"
#include "../../../../editor/procmat/ProceduralGpuEvaluator.h"
#include "../../../../editor/procmat/ProceduralSession.h"

#include "graph/Graph.h"
#include "jobs/JobSystem.h"
#include "render/TextureRender.h"

#include "AllocationCounter.h"
#include "GpuEnvironment.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor::procmat;
using jasperengine::test::GpuEnvironment;

namespace {

    using Clock = std::chrono::steady_clock;

    double msSince(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    double percentile(std::vector<double> values, double share) {
        if (values.empty()) return 0.0;
        std::sort(values.begin(), values.end());
        return values[std::min(values.size() - 1, (size_t)((double)values.size() * share))];
    }

    std::shared_ptr<const NodeLibrary> budgetsLibrary() {
        std::string error;
        auto library = NodeLibrary::fromDirectory(std::filesystem::path(JASPER_TEST_REPO_DIR) / "editor/procmat/nodes", error);
        EXPECT_TRUE(library) << error;
        return library;
    }

    graph::Graph brickWall() {
        std::ifstream file(std::filesystem::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/procmat-baked/assets/brick_wall.procmat");
        std::stringstream text;
        text << file.rdbuf();
        graph::LoadReport report;
        graph::Graph graph = graph::load(text.str(), report);
        EXPECT_TRUE(report.ok) << report.error;
        EXPECT_EQ(graph.nodes.size(), 30u) << "R23 to R25 are for a 30-node graph";
        return graph;
    }

    PlanOptions sized(int size) {
        PlanOptions options;
        options.size = size;
        options.maxBufferSize = size;
        return options;
    }

    // the window's take (ProceduralMaterialWindow::Services): the image stays on the device
    std::shared_ptr<TextureRender> takeDeviceImage(ProceduralEvaluator& evaluator, const std::string& output) {
        auto* gpu = dynamic_cast<ProceduralGpuEvaluator*>(&evaluator);
        std::unique_ptr<TextureRender> taken = gpu ? gpu->takeOutputTexture(output) : nullptr;
        if (!taken) return nullptr;
        return std::shared_ptr<TextureRender>(taken.release(), [](TextureRender* texture) {
            texture->destroyTexture();
            delete texture;
        });
    }

    // one thumbnail per node but the Material Output, on the first port it feeds on
    std::vector<ProcMatThumbnails::Request> everyNode(const graph::Graph& graph) {
        std::vector<ProcMatThumbnails::Request> requests;
        for (const graph::Node& node : graph.nodes) {
            const auto edge = std::find_if(graph.edges.begin(), graph.edges.end(), [&](const graph::Edge& e) { return e.from.node == node.id; });
            if (edge == graph.edges.end()) continue;
            ProcMatThumbnails::Request request;
            request.id = node.id;
            request.roots = {{"out", node.id, edge->from.port}};
            request.key = 1;
            requests.push_back(std::move(request));
        }
        return requests;
    }

    struct ThumbnailRun {
        std::vector<double> imageMs;    // Image::costMs of each, what the window shows on the node
        std::vector<double> updateMs;   // each frame's update() call
        size_t images = 0;
        int frames = 0;
        double wallMs = 0.0;
        int mostInOneFrame = 0;
    };

    ThumbnailRun drawThumbnails(ProcMatThumbnails& thumbs, const graph::Graph& graph) {
        ThumbnailRun run;
        thumbs.setGraph(graph);
        const std::vector<ProcMatThumbnails::Request> requests = everyNode(graph);
        run.images = requests.size();
        const auto start = Clock::now();
        uint64_t finished = 0;
        bool waiting = true;
        while (waiting && std::chrono::duration<double>(Clock::now() - start).count() < 600.0) {
            thumbs.setRequests(requests);
            const auto call = Clock::now();
            // the window's share of its 4 ms frame budget (ProceduralMaterialWindow::update)
            waiting = thumbs.update(1.0);
            run.updateMs.push_back(msSince(call));
            run.mostInOneFrame = std::max(run.mostInOneFrame, (int)(thumbs.finishedCount() - finished));
            finished = thumbs.finishedCount();
            GpuEnvironment::drawFrames(1);
            run.frames++;
        }
        run.wallMs = msSince(start);
        for (const ProcMatThumbnails::Request& request : requests) {
            const ProcMatThumbnails::Image* image = thumbs.image(request.id);
            EXPECT_TRUE(image && image->error.empty()) << request.id << ": " << (image ? image->error : "no image");
            if (image) run.imageMs.push_back(image->costMs);
        }
        return run;
    }

    void printThumbnails(const char* tier, const ThumbnailRun& run) {
        std::printf("[bench] ProcMat thumbnails %s: %zu images of 128² in %.1f s (%d update calls); per image median %.1f ms, p90 %.1f ms, worst %.1f ms; "
                    "update() per frame median %.2f ms, p99 %.2f ms, worst %.2f ms; at most %d finished in a frame\n",
                    tier, run.images, run.wallMs / 1000.0, run.frames, percentile(run.imageMs, 0.5), percentile(run.imageMs, 0.9), percentile(run.imageMs, 1.0),
                    percentile(run.updateMs, 0.5), percentile(run.updateMs, 0.99), percentile(run.updateMs, 1.0), run.mostInOneFrame);
    }

}

// R23: a slider drag is a float edit, a uniform edit of the plan the preview holds (R7): each edit is timed
// from setGraph to its new 256² image, frames included, as the window's material session runs it. R23's
// budget is tier 1's, which this host has not: the line is printed only
TEST(ProcMatBudgetsGpuSlow, SliderDragOnTheDevice) {
    JASPER_REQUIRE_GPU();
    const auto library = budgetsLibrary();
    graph::Graph file = brickWall();
    ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
    ProceduralSession session(evaluator, library, sized(256));
    session.setRoots(materialBakeRoots(file), 0.0);
    session.setGraph(file, 0.0);
    double now = 0.0;
    const auto first = Clock::now();
    while ((session.isStale() || session.isRebuildPending()) && session.error().empty() && msSince(first) < 120000.0) {
        now += 1.0 / 60.0;
        session.update(now, 2.0);
        GpuEnvironment::drawFrames(1);
    }
    ASSERT_TRUE(session.hasImage()) << session.error();
    const uint64_t compiles = evaluator.compileCount();

    std::vector<double> editMs, frameMs, workMs, setGraphMs;
    std::vector<double> frames;
    for (int i = 0; i < 40; i++) {
        file.findNode("b")->params["mortar"] = 0.04 + 0.001 * i;
        const auto edit = Clock::now();
        int count = 0;
        double work = 0.0;
        session.setGraph(file, now);
        setGraphMs.push_back(msSince(edit));
        work += setGraphMs.back();
        while (session.isStale() && session.error().empty() && msSince(edit) < 10000.0) {
            const auto frame = Clock::now();
            now += 1.0 / 60.0;
            const auto call = Clock::now();
            session.update(now, 2.0);
            work += msSince(call);
            GpuEnvironment::drawFrames(1);
            frameMs.push_back(msSince(frame));
            count++;
        }
        ASSERT_FALSE(session.isStale()) << session.error();
        editMs.push_back(msSince(edit));
        workMs.push_back(work);
        frames.push_back((double)count);
    }
    EXPECT_EQ(evaluator.compileCount(), compiles) << "R7: a float edit compiles nothing";
    std::printf("[bench] ProcMat slider drag (256², 30 nodes, %s): edit to image median %.1f ms, p95 %.1f ms; frames per edit median %.0f, "
                "worst %.0f; a frame median %.1f ms, p95 %.1f ms; the session's own CPU work per edit median %.1f ms, of which setGraph median %.1f ms, "
                "p95 %.1f ms\n",
                evaluator.deviceName().c_str(), percentile(editMs, 0.5), percentile(editMs, 0.95), percentile(frames, 0.5),
                percentile(frames, 1.0), percentile(frameMs, 0.5), percentile(frameMs, 0.95), percentile(workMs, 0.5),
                percentile(setGraphMs, 0.5), percentile(setGraphMs, 0.95));
}

// R24 on the device: at most one image finished per frame, and what each costs
TEST(ProcMatBudgetsGpuSlow, ThumbnailsOnTheDevice) {
    JASPER_REQUIRE_GPU();
    const auto library = budgetsLibrary();
    ProcMatThumbnails thumbs(library, [] { return std::make_unique<ProceduralGpuEvaluator>(EvaluatorTier::Software); }, takeDeviceImage);
    const ThumbnailRun run = drawThumbnails(thumbs, brickWall());
    printThumbnails("software device", run);
    EXPECT_LE(run.mostInOneFrame, 1) << "R24: at most one image a frame";
    EXPECT_EQ(thumbs.size(), run.images);
}

// R24 on tier 3: the images are jobs, so a frame only polls
TEST(ProcMatBudgetsGpuSlow, ThumbnailsOnTheCpu) {
    JASPER_REQUIRE_GPU();
    const auto library = budgetsLibrary();
    ProcMatThumbnails thumbs(library, [] { return std::make_unique<ProceduralCpuEvaluator>(); },
                             [](ProceduralEvaluator&, const std::string&) { return std::shared_ptr<TextureRender>(); });
    const ThumbnailRun run = drawThumbnails(thumbs, brickWall());
    printThumbnails("cpu", run);
    EXPECT_LE(run.mostInOneFrame, 1) << "R24: at most one image a frame";
}

// R25: a 1024² bake of every output, on the device (prepare and compile included, then again with the
// programs compiled) and on the CPU tier through the bake's own entry point (R39)
TEST(ProcMatBudgetsGpuSlow, ABakeOf1024OnTheDeviceAndOnTheCpu) {
    JASPER_REQUIRE_GPU();
    const auto library = budgetsLibrary();
    const graph::Graph file = brickWall();
    PassPlan plan;
    std::string error;
    ASSERT_TRUE(planGraph(file, library, materialBakeRoots(file), sized(1024), plan, error)) << error;
    ASSERT_EQ(plan.outputs.size(), 4u) << "base_color, normal, orm and height";

    const auto onDevice = [&](ProceduralGpuEvaluator& evaluator) {
        const auto start = Clock::now();
        std::string failure;
        EXPECT_TRUE(evaluator.prepare(plan, failure)) << failure;
        EvaluationResult result;
        while (!evaluator.advance(8.0, result) && msSince(start) < 120000.0) GpuEnvironment::drawFrames(1);
        EXPECT_TRUE(result.ok) << result.error;
        return msSince(start);
    };
    ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
    const double cold = onDevice(evaluator);
    const double warm = onDevice(evaluator);

    const auto start = Clock::now();
    EvaluationResult result;
    ASSERT_TRUE(evaluateOnCpu(plan, result)) << result.error;
    const double cpu = msSince(start);
    std::printf("[bench] ProcMat bake 1024² (30 nodes, %zu passes, 4 outputs): %s %.0f ms (%.0f ms compiled), cpu %.1f s on %d workers\n",
                plan.passes.size(), evaluator.deviceName().c_str(), cold, warm, cpu / 1000.0, jobs::system().getWorkerCount());
    const double slack = testsupport::perfSlack();
    if (slack > 0.0) {
        EXPECT_LE(cold, 5000.0 * slack) << "R25, tier 2";
        EXPECT_LE(cpu, 300000.0 * slack) << "R25, tier 3";
    }
}
