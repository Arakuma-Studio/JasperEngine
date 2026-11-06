// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S90 R7, AC3: a float edit causes no rebuild; an enum edit causes exactly one, after the debounce; the
// previous image stays, marked out of date, until the new one is ready.

#include "../../procmat/NodeLibrary.h"
#include "../../procmat/ProceduralMaterialKind.h"
#include "../../procmat/ProceduralSession.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor::procmat;

namespace {

    // Counts what the session asks; an evaluation takes as many advance() calls as `slices`
    class CountingEvaluator : public ProceduralEvaluator {
    public:
        EvaluatorTier tier() const override { return EvaluatorTier::Software; }
        std::string deviceName() const override { return "counting"; }
        bool prepare(const PassPlan&, std::string& error) override {
            prepares++;
            if (refusePrepare) {
                error = "refused";
                return false;
            }
            remaining = slices;
            return true;
        }
        void setUniforms(uint32_t, const UniformBlock&) override {
            uniformEdits++;
            remaining = slices;
        }
        bool advance(double, EvaluationResult& result) override {
            if (remaining > 0 && --remaining > 0) return false;
            evaluations++;
            result.ok = !failNext;
            if (failNext) result.error = "device failure";
            failNext = false;
            result.outputs.push_back({"image", 1, 1, {(uint8_t)evaluations, 0, 0, 255}});
            return true;
        }
        void cancel() override { remaining = 0; }

        int prepares = 0;
        int uniformEdits = 0;
        int evaluations = 0;
        int slices = 1;
        int remaining = 0;
        bool refusePrepare = false;
        bool failNext = false;
    };

    // Graph::findNode looks nodes up in id order, as load() and the edit ops keep them
    void sortById(graph::Graph& file) {
        std::sort(file.nodes.begin(), file.nodes.end(), [](const graph::Node& a, const graph::Node& b) { return graph::idLess(a.id, b.id); });
        std::sort(file.edges.begin(), file.edges.end(), [](const graph::Edge& a, const graph::Edge& b) { return graph::idLess(a.id, b.id); });
    }

    class ProceduralSessionTest : public ::testing::Test {
    protected:
        void SetUp() override {
            std::string error;
            library = NodeLibrary::fromDirectory(std::filesystem::path(JASPER_TEST_REPO_DIR) / "editor/procmat/nodes", error);
            ASSERT_TRUE(library) << error;
            file.kind = PROCEDURAL_MATERIAL_KIND;
            graph::Node noise;
            noise.id = "n";
            noise.type = "perlin";
            graph::Node blend;
            blend.id = "b";
            blend.type = "blend";
            graph::Node output;
            output.id = "out";
            output.type = MATERIAL_OUTPUT_NODE;
            file.nodes = {noise, blend, output};
            graph::Edge in;
            in.id = "e1";
            in.from = {"n", "output"};
            in.to = {"b", "s1"};
            graph::Edge out;
            out.id = "e2";
            out.from = {"b", "output"};
            out.to = {"out", "base_color"};
            file.edges = {in, out};
            sortById(file);
            options.size = 64;
        }

        // the first image, at time 0
        void start(ProceduralSession& session) {
            session.setGraph(file, 0.0);
            session.update(0.0, 8.0);
            ASSERT_TRUE(session.hasImage());
            ASSERT_FALSE(session.isStale());
        }

        std::shared_ptr<const NodeLibrary> library;
        graph::Graph file;
        PlanOptions options;
        CountingEvaluator evaluator;
    };

}

TEST_F(ProceduralSessionTest, TheFirstImageNeedsNoDebounce) {
    ProceduralSession session(evaluator, library, options);
    session.setGraph(file, 0.0);
    EXPECT_TRUE(session.isStale());
    session.update(0.0, 8.0);
    EXPECT_EQ(evaluator.prepares, 1);
    EXPECT_FALSE(session.isStale());
    EXPECT_TRUE(session.hasImage());
}

TEST_F(ProceduralSessionTest, AFloatEditCausesNoRebuild) {
    ProceduralSession session(evaluator, library, options);
    start(session);
    file.findNode("n")->params["persistence"] = 0.25;
    session.setGraph(file, 1.0);
    EXPECT_EQ(evaluator.uniformEdits, 1) << "the edit reaches the pass at once";
    EXPECT_TRUE(session.isStale());
    EXPECT_FALSE(session.isRebuildPending());
    session.update(1.0, 8.0);
    session.update(1.5, 8.0);
    session.update(2.0, 8.0);
    EXPECT_EQ(evaluator.prepares, 1) << "no rebuild, now or after the debounce";
    EXPECT_FALSE(session.isStale());
    EXPECT_EQ(evaluator.evaluations, 2);
}

TEST_F(ProceduralSessionTest, AColourAndARampEditCauseNoRebuild) {
    file.nodes.push_back(graph::Node());
    graph::Node& colorize = file.nodes.back();
    colorize.id = "c";
    colorize.type = "colorize";
    const auto gradient = [](double middle) {
        return graph::Json{{"interpolation", 1}, {"points", {{{"pos", 0}, {"r", 0}, {"g", 0}, {"b", 0}, {"a", 1}}, {{"pos", middle}, {"r", 1}, {"g", 0}, {"b", 0}, {"a", 1}}, {{"pos", 1}, {"r", 1}, {"g", 1}, {"b", 1}, {"a", 1}}}}};
    };
    colorize.params["gradient"] = gradient(0.5);
    file.edges[1].from = {"c", "output"};
    file.edges.push_back(graph::Edge());
    file.edges.back().id = "e3";
    file.edges.back().from = {"n", "output"};
    file.edges.back().to = {"c", "input"};
    sortById(file);
    ProceduralSession session(evaluator, library, options);
    start(session);
    ASSERT_EQ(evaluator.prepares, 1);

    file.findNode("c")->params["gradient"] = gradient(0.6);
    session.setGraph(file, 1.0);
    session.update(2.0, 8.0);
    EXPECT_EQ(evaluator.prepares, 1) << "a ramp of the same size is uniforms";
    EXPECT_GE(evaluator.uniformEdits, 1);

    // a key more changes the generated function: that is a rebuild
    graph::Json larger = gradient(0.6);
    larger["points"].push_back({{"pos", 0.8}, {"r", 0}, {"g", 1}, {"b", 0}, {"a", 1}});
    file.findNode("c")->params["gradient"] = larger;
    session.setGraph(file, 3.0);
    session.update(3.5, 8.0);
    EXPECT_EQ(evaluator.prepares, 2);
}

TEST_F(ProceduralSessionTest, AnEnumEditCausesExactlyOneRebuildAfterTheDebounce) {
    ProceduralSession session(evaluator, library, options);
    start(session);
    const EvaluationResult before = session.latest();

    file.findNode("b")->params["blend_type"] = "multiply";
    session.setGraph(file, 1.0);
    EXPECT_TRUE(session.isRebuildPending());
    EXPECT_TRUE(session.isStale());
    session.update(1.1, 8.0);
    EXPECT_EQ(evaluator.prepares, 1) << "still within the debounce";
    EXPECT_EQ(session.latest().outputs[0].rgba, before.outputs[0].rgba) << "the previous image stays";
    EXPECT_TRUE(session.isStale());

    session.update(1.0 + ProceduralSession::REBUILD_DEBOUNCE_SECONDS, 8.0);
    EXPECT_EQ(evaluator.prepares, 2);
    EXPECT_FALSE(session.isStale());
    EXPECT_NE(session.latest().outputs[0].rgba, before.outputs[0].rgba);

    session.update(5.0, 8.0);
    EXPECT_EQ(evaluator.prepares, 2) << "exactly one";
}

TEST_F(ProceduralSessionTest, ARunOfStructuralEditsIsOneRebuild) {
    ProceduralSession session(evaluator, library, options);
    start(session);
    const char* modes[] = {"multiply", "screen", "overlay", "burn"};
    double now = 1.0;
    for (const char* mode : modes) {
        file.findNode("b")->params["blend_type"] = mode;
        session.setGraph(file, now);
        session.update(now, 8.0);
        now += 0.1;
    }
    EXPECT_EQ(evaluator.prepares, 1) << "each edit moved the deadline";
    session.update(now + ProceduralSession::REBUILD_DEBOUNCE_SECONDS, 8.0);
    EXPECT_EQ(evaluator.prepares, 2);
}

TEST_F(ProceduralSessionTest, AnEditBackBeforeTheDebounceRebuildsNothing) {
    ProceduralSession session(evaluator, library, options);
    start(session);
    file.findNode("b")->params["blend_type"] = "multiply";
    session.setGraph(file, 1.0);
    file.findNode("b")->params.erase("blend_type");
    session.setGraph(file, 1.05);
    EXPECT_FALSE(session.isRebuildPending());
    session.update(2.0, 8.0);
    EXPECT_EQ(evaluator.prepares, 1);
    EXPECT_FALSE(session.isStale()) << "what is drawn is the graph again";
}

TEST_F(ProceduralSessionTest, AGraphThatCannotBePlannedKeepsThePreviousImageStale) {
    ProceduralSession session(evaluator, library, options);
    start(session);
    const EvaluationResult before = session.latest();
    file.findNode("n")->params["persistence"] = "not a number";
    session.setGraph(file, 1.0);
    session.update(2.0, 8.0);
    EXPECT_EQ(evaluator.prepares, 1);
    EXPECT_TRUE(session.isStale());
    EXPECT_FALSE(session.error().empty());
    EXPECT_EQ(session.latest().outputs[0].rgba, before.outputs[0].rgba);

    file.findNode("n")->params["persistence"] = 0.25;
    session.setGraph(file, 3.0);
    session.update(3.0, 8.0);
    EXPECT_TRUE(session.error().empty());
    EXPECT_FALSE(session.isStale());
}

TEST_F(ProceduralSessionTest, ADeviceFailureKeepsThePreviousImageAndSaysWhy) {
    ProceduralSession session(evaluator, library, options);
    start(session);
    const EvaluationResult before = session.latest();
    evaluator.failNext = true;
    file.findNode("n")->params["persistence"] = 0.25;
    session.setGraph(file, 1.0);
    session.update(1.0, 8.0);
    EXPECT_EQ(session.error(), "device failure");
    EXPECT_TRUE(session.isStale());
    EXPECT_EQ(session.latest().outputs[0].rgba, before.outputs[0].rgba);
}

TEST_F(ProceduralSessionTest, ASliceAtATimeFinishesTheImage) {
    evaluator.slices = 3;
    ProceduralSession session(evaluator, library, options);
    session.setGraph(file, 0.0);
    session.update(0.0, 1.0);
    EXPECT_FALSE(session.hasImage());
    session.update(0.1, 1.0);
    EXPECT_FALSE(session.hasImage());
    session.update(0.2, 1.0);
    EXPECT_TRUE(session.hasImage());
    EXPECT_EQ(evaluator.prepares, 1);
}

TEST_F(ProceduralSessionTest, ASeedEditIsAValueEditAndASizeEditRebuilds) {
    ProceduralSession session(evaluator, library, options);
    start(session);
    PlanOptions reseeded = options;
    reseeded.seed = 5.0;
    session.setOptions(reseeded, 1.0);
    EXPECT_GE(evaluator.uniformEdits, 1);
    session.update(2.0, 8.0);
    EXPECT_EQ(evaluator.prepares, 1);

    PlanOptions larger = reseeded;
    larger.size = 128;
    session.setOptions(larger, 3.0);
    EXPECT_TRUE(session.isRebuildPending());
    session.update(3.5, 8.0);
    EXPECT_EQ(evaluator.prepares, 2);
}
