// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Procedural Material window (docs/specs/S90-procedural-material.md R7, R24, R26, R27, AC15),
// on an evaluator of the tests' own (procmat/ImageEvaluator.h: no device), driven through the
// functions its toolbar, pane, canvas and Inspector call, with the test's clock:
// - R7: a value edit reaches the preview's passes as uniforms, a structural one rebuilds once after
//   the 0.2 s debounce, and the image before stays, marked out of date, until then;
// - R24: thumbnails of every visible node on tier 1, of the selection and the Material Output on
//   tier 2, of the selection alone on tier 3, at most one finished per frame, with a cost;
// - R27: the preview follows the selection, Pin A keeps an output; the seed with Lock, Reroll and
//   Variations, each one undo step; Exposed Properties and Presets; the check report's rows in the
//   Problems strip, Fix on a single edit; the tier line and the bake estimate;
// - Assets > Create's starter graph is valid and draws;
// - every panel drawn in a frame.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "Engine.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "graph/GraphDocument.h"
#include "graph/GraphRegistry.h"
#include "procmat/ImageEvaluator.h"
#include "procmat/NodeLibrary.h"
#include "procmat/ProceduralBake.h"
#include "procmat/ProceduralRun.h"
#include "procmat/ProceduralDevice.h"
#include "procmat/ProceduralMaterialKind.h"
#include "window/ProceduralMaterialWindow.h"

#include "imgui.h"

#include "support/PosixCompat.h"

#include <chrono>
#include <thread>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using GraphJson = graph::Json;
using procmat::EvaluatorTier;
using procmat::test::ImageEvaluator;

namespace {

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-procmat-window-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            std::ofstream(out.dir / "project.yaml") << "name: procmatproject\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    bool started = false;
    Project* project() {
        started = true;
        return headless().project.get();
    }
    fs::path assets() { return project()->getAssetsPath(); }
    CommandHistory* history() { return project()->getProjectCommandHistory(); }

    struct Cleanup : public ::testing::Environment {
        void TearDown() override {
            if (!started) return;
            std::error_code ec;
            fs::remove_all(headless().dir.parent_path(), ec);
        }
    };
    [[maybe_unused]] ::testing::Environment* const cleanup = ::testing::AddGlobalTestEnvironment(new Cleanup);

    void writeAll(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // Perlin (n1) -> Blend (n2) -> Material Output (n3): base colour; Perlin -> roughness too
    std::string writeMaterial(const std::string& name) {
        writeAll(assets() / "materials" / (name + ".procmat"), R"({"formatVersion": 1, "kind": "procedural_material", "kindVersion": 1, "nextId": 7,
            "nodes": [{"id": "n1", "type": "perlin", "position": [0, 0], "params": {}},
                      {"id": "n2", "type": "blend", "position": [300, 0], "params": {"amount": 0.5}},
                      {"id": "n3", "type": "material_output", "position": [600, 0], "params": {}}],
            "edges": [{"id": "e4", "from": {"node": "n1", "port": "output"}, "to": {"node": "n2", "port": "s1"}},
                      {"id": "e5", "from": {"node": "n2", "port": "output"}, "to": {"node": "n3", "port": "base_color"}},
                      {"id": "e6", "from": {"node": "n1", "port": "output"}, "to": {"node": "n3", "port": "roughness"}}]})");
        return "assets/materials/" + name + ".procmat";
    }

    // the window's evaluators in the order it makes them: the material's, the focus', the thumbnails'
    struct Fake {
        std::vector<std::shared_ptr<ImageEvaluator::Counts>> made;
        EvaluatorTier tier = EvaluatorTier::Gpu;
        bool tierKnown = true;
        int slices = 1;
        bool ramp = false;
        std::vector<std::string> warnings;
        nlohmann::json report;
        std::vector<ProceduralMaterialWindow::BakeRequest> bakes;

        ProceduralMaterialWindow::Services services() {
            ProceduralMaterialWindow::Services out;
            out.tierKnown = tierKnown;
            out.makeEvaluator = [this]() {
                made.push_back(std::make_shared<ImageEvaluator::Counts>());
                auto evaluator = std::make_unique<ImageEvaluator>(made.back(), tier, slices);
                evaluator->ramp = ramp;
                evaluator->warnings = warnings;
                return std::unique_ptr<procmat::ProceduralEvaluator>(std::move(evaluator));
            };
            out.checks = [this](Project*, const std::string&, const graph::Graph&, const procmat::EvaluationResult&) { return report; };
            out.startBake = [this](Project*, const ProceduralMaterialWindow::BakeRequest& request, std::string&) {
                bakes.push_back(request);
                return true;
            };
            return out;
        }
        ImageEvaluator::Counts& material() { return *made.at(0); }
    };

    // an ImGui frame loop of its own (the window's show(), the Inspector)
    struct Frames {
        ImGuiContext* previous = nullptr;
        ImGuiContext* context = nullptr;
        Frames() {
            previous = ImGui::GetCurrentContext();
            context = ImGui::CreateContext();
            ImGui::SetCurrentContext(context);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(1400, 900);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char* pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }
        ~Frames() {
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(previous);
        }
        bool frame(GraphEditorWindow& window) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            window.show();
            ImGui::Begin("Inspector");
            const bool drew = window.drawSelectionInspector();
            ImGui::End();
            ImGui::EndFrame();
            return drew;
        }
    };

    class ProceduralMaterialWindowTest : public ::testing::Test {
    protected:
        void SetUp() override {
            ASSERT_TRUE(headless().ready);
            procmat::registerProceduralMaterialKind();
        }
        void TearDown() override { GraphDocument::closeAll(); }

        // the editor's frame: the window's GPU work (App::engineRender), then its draw (App::show)
        void step(ProceduralMaterialWindow& window, Frames& frames, double seconds = 1.0 / 60.0) {
            clock += seconds;
            window.update(clock);
            frames.frame(window);
        }
        void settle(ProceduralMaterialWindow& window, Frames& frames) {
            for (int i = 0; i < 40; i++) step(window, frames, 0.05);
        }

        double clock = 100.0;
    };

    bool has(const std::vector<procmat::ProcMatThumbnails::Request>& requests, const std::string& id) {
        return std::any_of(requests.begin(), requests.end(), [&](const auto& r) { return r.id == id; });
    }

}

// R7, AC3 in the window: a value edit is a uniform edit; an enum edit rebuilds once, after the debounce,
// with the image before shown out of date until then
TEST_F(ProceduralMaterialWindowTest, AValueEditIsUniformAndAStructuralOneRebuildsAfterTheDebounce) {
    Fake fake;
    ProceduralMaterialWindow window(project(), fake.services());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeMaterial("live"))) << window.getStatus();
    Frames frames;
    settle(window, frames);
    ASSERT_TRUE(window.materialSession()->hasImage());
    EXPECT_FALSE(window.materialSession()->isStale());
    const int prepares = fake.material().prepares;
    const int edits = fake.material().uniformEdits;

    ASSERT_TRUE(window.setParam("n2", "amount", 0.8));
    EXPECT_TRUE(window.materialSession()->isStale()) << "marked out of date";
    step(window, frames);
    EXPECT_FALSE(window.materialSession()->isStale()) << "a uniform edit draws at once";
    EXPECT_GT(fake.material().uniformEdits, edits);
    settle(window, frames);
    EXPECT_EQ(fake.material().prepares, prepares) << "no rebuild for a value";

    ASSERT_TRUE(window.setParam("n2", "blend_type", "multiply"));
    step(window, frames, 0.05);
    EXPECT_TRUE(window.materialSession()->isRebuildPending()) << "the debounce runs";
    EXPECT_TRUE(window.materialSession()->hasImage()) << "the previous image stays";
    EXPECT_TRUE(window.materialSession()->isStale());
    for (int i = 0; i < 6; i++) step(window, frames, 0.05);
    const int afterRebuild = fake.material().prepares;
    EXPECT_GE(afterRebuild, prepares + 1);
    settle(window, frames);
    EXPECT_FALSE(window.materialSession()->isStale());
    EXPECT_EQ(fake.material().prepares, afterRebuild) << "one rebuild for the material (and none after)";
}

// R24: which nodes get an image on each tier, one finished per frame, a cost on each
TEST_F(ProceduralMaterialWindowTest, ThumbnailsFollowTheTierBudget) {
    for (EvaluatorTier tier : {EvaluatorTier::Gpu, EvaluatorTier::Software, EvaluatorTier::Cpu}) {
        SCOPED_TRACE(procmat::evaluatorTierName(tier));
        Fake fake;
        fake.tier = tier;
        ProceduralMaterialWindow window(project(), fake.services());
        window.setOpen(true);
        ASSERT_TRUE(window.openGraph(writeMaterial(std::string("tier_") + procmat::evaluatorTierName(tier))));
        Frames frames;
        step(window, frames);
        window.selectNodes({"n1"});
        // two frames: the canvas says which nodes it drew, then the requests follow
        step(window, frames);
        step(window, frames);
        const auto& requests = window.thumbnailRequests();
        EXPECT_TRUE(has(requests, "n1")) << "the selection, on every tier";
        EXPECT_EQ(has(requests, "n3"), tier != EvaluatorTier::Cpu) << "the Material Output on the device tiers";
        EXPECT_EQ(has(requests, "n2"), tier == EvaluatorTier::Gpu) << "every visible node on a GPU only";
        EXPECT_EQ(requests.front().id, "n1") << "the selection first";

        const uint64_t before = window.thumbnails().finishedCount();
        step(window, frames);
        EXPECT_LE(window.thumbnails().finishedCount(), before + 1) << "at most one image a frame";
        settle(window, frames);
        ASSERT_TRUE(window.thumbnails().image("n1"));
        EXPECT_NE(window.canvasNode(*window.getDocument()->graph().findNode("n1")).cost.find("ms"), std::string::npos);
        EXPECT_TRUE(window.canvasNode(*window.getDocument()->graph().findNode("n1")).thumbnail);
    }
}

// R27: the preview follows the selection; Pin A keeps an output whatever is selected, until clicked again
TEST_F(ProceduralMaterialWindowTest, ThePreviewFollowsTheSelectionAndAPinKeepsAnOutput) {
    Fake fake;
    ProceduralMaterialWindow window(project(), fake.services());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeMaterial("pins")));
    Frames frames;
    settle(window, frames);
    EXPECT_TRUE(window.shownTarget().node.empty()) << "nothing selected: the material";

    window.selectNodes({"n1"});
    EXPECT_EQ(window.shownTarget(), (ProceduralMaterialWindow::Target{"n1", "output"}));
    settle(window, frames);
    ASSERT_EQ(window.focusSession()->getRoots().size(), 1u);
    EXPECT_EQ(window.focusSession()->getRoots()[0].node, "n1");
    EXPECT_TRUE(window.focusSession()->hasImage());

    window.clickPin(0);
    EXPECT_EQ(window.shownSlot(), 0);
    window.selectNodes({"n2"});
    EXPECT_EQ(window.shownTarget().node, "n1") << "pinned";
    EXPECT_EQ(window.focusSession()->getRoots()[0].node, "n1");
    window.clickPin(0);
    EXPECT_EQ(window.shownSlot(), -1);
    EXPECT_EQ(window.shownTarget().node, "n2") << "following the selection again";
    window.clickPin(0);
    EXPECT_EQ(window.shownTarget().node, "n1") << "a filled slot shows what it holds";
    window.clickPin(1);
    EXPECT_EQ(window.pinnedTarget(1).node, "n1") << "B took what was shown";
    window.clearPin(0);
    window.clearPin(1);
    EXPECT_EQ(window.shownTarget().node, "n2");
}

// R27: seed, Lock, Reroll and Variations as undo steps; a seed is a uniform for the preview
TEST_F(ProceduralMaterialWindowTest, SeedLockRerollAndVariations) {
    Fake fake;
    ProceduralMaterialWindow window(project(), fake.services());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeMaterial("seeds")));
    Frames frames;
    settle(window, frames);
    const int prepares = fake.material().prepares;
    const size_t start = history()->position();

    ASSERT_TRUE(window.setSeed(42));
    EXPECT_EQ(window.seed(), 42);
    EXPECT_EQ(history()->position(), start + 1) << "one step";
    EXPECT_EQ(window.getDocument()->graph().metadata.value("seed", 0), 42) << "in the file";
    settle(window, frames);

    ASSERT_TRUE(window.rerollSeed());
    EXPECT_NE(window.seed(), 42);
    EXPECT_EQ(history()->position(), start + 2);

    const std::vector<int> seeds = window.variationSeeds();
    ASSERT_EQ(seeds.size(), 9u);
    EXPECT_EQ(std::set<int>(seeds.begin(), seeds.end()).size(), 9u) << "nine different seeds";
    EXPECT_EQ(window.variationSeeds(), seeds) << "the same nine until the seed changes";
    EXPECT_EQ(std::count(seeds.begin(), seeds.end(), window.seed()), 0);
    window.setVariationsOpen(true);
    step(window, frames);
    step(window, frames);
    EXPECT_TRUE(has(window.thumbnailRequests(), "variation:0"));
    EXPECT_TRUE(has(window.thumbnailRequests(), "variation:8"));
    settle(window, frames);
    EXPECT_TRUE(window.thumbnails().image("variation:4")) << "the grid's images";
    ASSERT_TRUE(window.adoptVariation(4));
    EXPECT_EQ(window.seed(), seeds[4]) << "a click adopts its seed";

    ASSERT_TRUE(window.setSeedLocked(true));
    EXPECT_TRUE(window.seedLocked());
    const int locked = window.seed();
    EXPECT_FALSE(window.rerollSeed());
    EXPECT_FALSE(window.adoptVariation(0));
    EXPECT_FALSE(window.setSeed(7));
    EXPECT_EQ(window.seed(), locked);
    settle(window, frames);
    EXPECT_EQ(fake.material().prepares, prepares) << "a seed change compiles nothing (R7)";

    ASSERT_TRUE(history()->undo());
    EXPECT_FALSE(window.seedLocked()) << "Lock is a step too";
    while (history()->position() > start) ASSERT_TRUE(history()->undo());
    EXPECT_EQ(window.seed(), 0);
    EXPECT_FALSE(window.getDocument()->graph().metadata.contains("seed"));
}

// R27: Exposed Properties from the Inspector's row menu, Presets of their values, applied as one step
TEST_F(ProceduralMaterialWindowTest, ExposedPropertiesAndPresets) {
    Fake fake;
    ProceduralMaterialWindow window(project(), fake.services());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeMaterial("presets")));
    ASSERT_TRUE(window.setExposed("n2", "amount", true));
    ASSERT_TRUE(window.setExposed("n1", "scale_x", true));
    EXPECT_TRUE(window.isExposed("n2", "amount"));
    ASSERT_EQ(window.exposedProperties().size(), 2u);
    EXPECT_FALSE(window.setExposed("n2", "no_such", true)) << "only parameters the type has";

    ASSERT_TRUE(window.setParam("n2", "amount", 0.25));
    ASSERT_TRUE(window.savePreset("Wet"));
    ASSERT_TRUE(window.setParam("n2", "amount", 0.9));
    ASSERT_TRUE(window.setParam("n1", "scale_x", 12.0));
    ASSERT_TRUE(window.savePreset("Dry"));
    EXPECT_EQ(window.presetNames(), (std::vector<std::string>{"Wet", "Dry"}));

    const size_t start = history()->position();
    ASSERT_TRUE(window.applyPreset("Wet"));
    EXPECT_EQ(history()->position(), start + 1) << "one step for every value";
    const graph::Graph& g = window.getDocument()->graph();
    EXPECT_DOUBLE_EQ(g.findNode("n2")->params["amount"].get<double>(), 0.25);
    EXPECT_DOUBLE_EQ(g.findNode("n1")->params["scale_x"].get<double>(), 4.0) << "its default when the preset was saved";
    ASSERT_TRUE(history()->undo());
    EXPECT_DOUBLE_EQ(window.getDocument()->graph().findNode("n2")->params["amount"].get<double>(), 0.9);

    ASSERT_TRUE(window.deletePreset("Dry"));
    EXPECT_EQ(window.presetNames(), (std::vector<std::string>{"Wet"}));
    ASSERT_TRUE(window.setExposed("n1", "scale_x", false));
    EXPECT_EQ(window.exposedProperties().size(), 1u);

    Frames frames;
    window.selectNodes({"n2"});
    EXPECT_TRUE(frames.frame(window)) << "the pane's rows and the Inspector draw";
}

// R27: the check report's rows are Problems strip rows, a single-edit fix is Fix, and a click selects
TEST_F(ProceduralMaterialWindowTest, TheCheckReportsRowsAreProblemsAndASingleEditIsFix) {
    Fake fake;
    fake.report = {{"checks", nlohmann::json::array({
                        {{"id", "tileability"}, {"kind", "metric"}, {"status", "FAIL"}, {"measured", 3.2}, {"threshold", 1.5},
                         {"fix_hint", "Raise the tile count."}, {"limited_by", nullptr}, {"at", nlohmann::json::array()}},
                        {{"id", "roughness"}, {"kind", "metric"}, {"status", "WARN"}, {"measured", 0.02}, {"node", "n1"}},
                        {{"id", "histogram"}, {"kind", "metric"}, {"status", "PASS"}}})},
                   {"fixes", nlohmann::json::array({
                        {{"check", "tileability"}, {"node", "n2"}, {"label", "Set Amount to 0.3"},
                         {"op", {{"op", "setParam"}, {"node", "n2"}, {"name", "amount"}, {"value", 0.3}}}},
                        {{"check", "roughness"}, {"ops", nlohmann::json::array({{{"op", "setParam"}, {"node", "n1"}, {"name", "scale_x"}, {"value", 2}},
                                                                                 {{"op", "setParam"}, {"node", "n1"}, {"name", "scale_y"}, {"value", 2}}})}}})}};
    ProceduralMaterialWindow window(project(), fake.services());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeMaterial("checks")));
    Frames frames;
    settle(window, frames);
    std::vector<GraphEditorWindow::Problem> rows = window.problems();
    auto find = [&](const std::string& code) -> const GraphEditorWindow::Problem* {
        for (const auto& row : rows) if (row.code == code) return &row;
        return nullptr;
    };
    const GraphEditorWindow::Problem* tile = find("tileability");
    const GraphEditorWindow::Problem* rough = find("roughness");
    ASSERT_TRUE(tile);
    ASSERT_TRUE(rough);
    EXPECT_FALSE(find("histogram")) << "a PASS is no problem";
    EXPECT_EQ(tile->severity, graph::Severity::Error);
    EXPECT_NE(tile->message.find("Raise the tile count."), std::string::npos) << "the row's line, its fix_hint included";
    EXPECT_EQ(tile->nodeId, "n2");
    ASSERT_EQ(tile->fix.size(), 1u);
    EXPECT_TRUE(rough->fix.empty()) << "two edits are the agent's to decide";
    EXPECT_EQ(rough->severity, graph::Severity::Warning);

    const size_t start = history()->position();
    ASSERT_TRUE(window.fixProblem(*tile));
    EXPECT_EQ(history()->position(), start + 1);
    EXPECT_DOUBLE_EQ(window.getDocument()->graph().findNode("n2")->params["amount"].get<double>(), 0.3);
    ASSERT_TRUE(window.inspectProblem(*rough));
    EXPECT_EQ(window.selectedNodes(), std::vector<std::string>{"n1"});
}

// R27: the tier and device; on tiers 2 and 3 why, and the bake's estimate from the preview's time
TEST_F(ProceduralMaterialWindowTest, TheTierLineAndTheBakeEstimate) {
    Fake fake;
    fake.tier = EvaluatorTier::Cpu;
    fake.slices = 3;
    ProceduralMaterialWindow window(project(), fake.services());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeMaterial("bake")));
    Frames frames;
    EXPECT_LT(window.bakeEstimateSeconds(1024), 0.0) << "no estimate before an image";
    for (int i = 0; i < 10; i++) step(window, frames, 0.1);
    EXPECT_EQ(window.tier(), EvaluatorTier::Cpu);
    EXPECT_EQ(window.deviceName(), "test device");
    EXPECT_EQ(window.tierReason(), "No GPU available, baking on the CPU.");
    const double preview = window.bakeEstimateSeconds(ProceduralMaterialWindow::PREVIEW_SIZE);
    ASSERT_GT(preview, 0.0);
    EXPECT_NEAR(window.bakeEstimateSeconds(1024), preview * 16.0, 1e-9) << "per pixel: four times the side, sixteen times the time";

    window.setBakeSize(512);
    ASSERT_TRUE(window.canBake());
    ASSERT_TRUE(window.bake());
    ASSERT_EQ(fake.bakes.size(), 1u);
    EXPECT_EQ(fake.bakes[0].size, 512);
    EXPECT_EQ(fake.bakes[0].path, "assets/materials/bake.procmat");

    Fake gpu;
    ProceduralMaterialWindow fast(project(), gpu.services());
    fast.setOpen(true);
    ASSERT_TRUE(fast.openGraph(writeMaterial("bake_gpu")));
    EXPECT_EQ(fast.tierReason(), "");
}

// Assets > Create > Procedural Material: valid, laid out, and its image draws
TEST_F(ProceduralMaterialWindowTest, TheStarterGraphIsValidAndDraws) {
    writeAll(assets() / "materials" / "New Procedural Material.procmat", graph::save(procmat::starterProceduralMaterial("0123456789abcdef0123456789abcdef")));
    Fake fake;
    ProceduralMaterialWindow window(project(), fake.services());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph("assets/materials/New Procedural Material.procmat")) << window.getStatus();
    EXPECT_TRUE(window.getDocument()->issues().empty());
    Frames frames;
    settle(window, frames);
    EXPECT_TRUE(window.materialSession()->hasImage()) << window.materialSession()->error();
    EXPECT_EQ(window.materialSession()->latest().outputs.size(), 2u) << "Base Color and ORM";
    EXPECT_TRUE(window.problems().empty());
    window.prepareForProjectSwitch();
    EXPECT_FALSE(window.materialSession()) << "a project switch drops the sessions";
}

// review I3: the ball takes only the outputs the material has now: a disconnected Emission leaves it
TEST_F(ProceduralMaterialWindowTest, TheBallDropsAnOutputTheMaterialNoLongerHas) {
    Fake fake;
    ProceduralMaterialWindow window(project(), fake.services());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeMaterial("ball")));
    ASSERT_TRUE(window.connect("n1", "output", "n3", "emission"));
    Frames frames;
    settle(window, frames);
    std::vector<std::string> outputs = window.ballOutputs();
    EXPECT_NE(std::find(outputs.begin(), outputs.end(), "emission"), outputs.end());
    const graph::Edge* wire = nullptr;
    for (const graph::Edge& edge : window.getDocument()->graph().edges) {
        if (edge.to.port == "emission") wire = &edge;
    }
    ASSERT_TRUE(wire);
    ASSERT_TRUE(window.getDocument()->apply({{"disconnect", {{"id", wire->id}}}}, "Disconnect", nullptr));
    settle(window, frames);
    outputs = window.ballOutputs();
    EXPECT_EQ(std::find(outputs.begin(), outputs.end(), "emission"), outputs.end()) << "Emission is gone from the ball";
    EXPECT_NE(std::find(outputs.begin(), outputs.end(), "base_color"), outputs.end());
}

// review I4: a device whose tier nobody detected is shown as such, with R24's tier 2 budget, never as a GPU
TEST_F(ProceduralMaterialWindowTest, AnUndetectedTierIsUnknownAndBudgetedAsSoftware) {
    Fake fake;
    fake.tierKnown = false;
    ProceduralMaterialWindow window(project(), fake.services());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeMaterial("unknown_tier")));
    Frames frames;
    step(window, frames);
    window.selectNodes({"n1"});
    step(window, frames);
    step(window, frames);
    EXPECT_FALSE(window.tierKnown());
    EXPECT_EQ(window.tierLabel(), "unknown");
    EXPECT_FALSE(has(window.thumbnailRequests(), "n2")) << "no image of every visible node on a device that may be software";
    EXPECT_TRUE(has(window.thumbnailRequests(), "n3"));
    EXPECT_EQ(window.tierReason(), "");
}

// review I5: a Variations cell adopts only the seed it pictures: not while its image is of other seeds
TEST_F(ProceduralMaterialWindowTest, AVariationIsAdoptedOnlyOnceItsImageIsCurrent) {
    Fake fake;
    ProceduralMaterialWindow window(project(), fake.services());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeMaterial("cells")));
    window.setVariationsOpen(true);
    Frames frames;
    settle(window, frames);
    ASSERT_TRUE(window.isVariationCurrent(2));
    ASSERT_TRUE(window.setSeed(77));
    step(window, frames);
    EXPECT_FALSE(window.isVariationCurrent(2)) << "the cell pictures the previous seeds";
    EXPECT_FALSE(window.adoptVariation(2));
    EXPECT_EQ(window.seed(), 77);
    settle(window, frames);
    EXPECT_TRUE(window.isVariationCurrent(2));
    EXPECT_TRUE(window.adoptVariation(2));
}

// review M2: the estimate is a rebuild's: a value edit, which redraws from the edited pass, leaves it
TEST_F(ProceduralMaterialWindowTest, AValueEditLeavesTheBakeEstimate) {
    Fake fake;
    fake.slices = 3;
    ProceduralMaterialWindow window(project(), fake.services());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeMaterial("estimate")));
    Frames frames;
    for (int i = 0; i < 10; i++) step(window, frames, 0.1);
    const double estimate = window.bakeEstimateSeconds(1024);
    ASSERT_GT(estimate, 0.0);
    ASSERT_TRUE(window.setParam("n2", "amount", 0.7));
    for (int i = 0; i < 10; i++) step(window, frames, 0.5);
    EXPECT_DOUBLE_EQ(window.bakeEstimateSeconds(1024), estimate);
}

// review M5, M7, M8, M9: a closed window keeps no sessions; a quality FAIL does not block a bake; a
// pinned node deleted unpins; check rows go with the image they were made from
TEST_F(ProceduralMaterialWindowTest, ClosedPinsRowsAndBake) {
    Fake fake;
    fake.report = {{"checks", nlohmann::json::array({{{"id", "tileability"}, {"kind", "metric"}, {"status", "FAIL"}, {"measured", 3.2}}})}};
    ProceduralMaterialWindow window(project(), fake.services());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeMaterial("closed")));
    Frames frames;
    settle(window, frames);
    auto hasRow = [&](const std::string& code) {
        for (const auto& row : window.problems()) if (row.code == code) return true;
        return false;
    };
    ASSERT_TRUE(hasRow("tileability"));
    EXPECT_TRUE(window.canBake()) << "a quality check's FAIL is the agent's to weigh, not a refusal";

    ASSERT_TRUE(window.setParam("n2", "amount", 0.6));
    EXPECT_FALSE(hasRow("tileability")) << "rows of the image before the edit";
    settle(window, frames);
    EXPECT_TRUE(hasRow("tileability"));

    window.selectNodes({"n1"});
    window.clickPin(0);
    window.selectNodes({"n1"});
    ASSERT_TRUE(window.removeSelection());
    EXPECT_TRUE(window.pinnedTarget(0).node.empty()) << "the pin of a deleted node";
    EXPECT_EQ(window.shownSlot(), -1);

    window.setOpen(false);
    ProceduralMaterialWindow::processFrame();
    EXPECT_FALSE(window.materialSession()) << "a closed window keeps no sessions";
    ASSERT_TRUE(window.setParam("n2", "amount", 0.2));
    EXPECT_FALSE(window.materialSession());
    window.setOpen(true);
    frames.frame(window);
    ASSERT_TRUE(window.materialSession());
    settle(window, frames);
    EXPECT_TRUE(window.materialSession()->hasImage());
}

// Wiring: the window's own services are the tiers' evaluator in R8's order (the device the editor has, its
// tier claimed), preview_procedural_material's checks and the bake
TEST_F(ProceduralMaterialWindowTest, TheDefaultsAreTheTiersTheChecksAndTheBake) {
    ProceduralMaterialWindow::Services defaults = ProceduralMaterialWindow::defaultServices();
    EXPECT_TRUE(defaults.tierKnown);
    EXPECT_TRUE(defaults.checks);
    EXPECT_TRUE(defaults.startBake);
    EXPECT_TRUE(defaults.bakeState);
    EXPECT_TRUE(defaults.cancelBake);
    ASSERT_TRUE(defaults.makeEvaluator);
    std::unique_ptr<procmat::ProceduralEvaluator> evaluator = defaults.makeEvaluator();
    ASSERT_TRUE(evaluator);
    const procmat::EvaluatorDevice device = procmat::editorDevice();
    EXPECT_EQ(evaluator->tier(), device.tier) << "the unit tests have no context: the CPU tier";
    EXPECT_EQ(evaluator->deviceName(), device.name);
}

// Wiring (R27): preview_procedural_material's rows in the Problems strip; a tileability FAIL fed through a
// fractional repeat count gets its single setParam as Fix, one undo step
TEST_F(ProceduralMaterialWindowTest, TheCheckRowsFixIsOneUndoStep) {
    Fake fake;
    fake.ramp = true;
    ProceduralMaterialWindow::Services services = fake.services();
    services.checks = ProceduralMaterialWindow::defaultServices().checks;
    ASSERT_TRUE(services.checks);
    const std::string path = writeMaterial("tiles");
    ProceduralMaterialWindow window(project(), services);
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(path));
    ASSERT_TRUE(window.setParam("n1", "scale_x", 4.5));
    Frames frames;
    settle(window, frames);
    const GraphEditorWindow::Problem* tile = nullptr;
    const std::vector<GraphEditorWindow::Problem> rows = window.problems();
    for (const auto& row : rows) {
        if (row.code == "tileability" && !row.fix.empty()) tile = &row;
    }
    ASSERT_TRUE(tile) << "a tileability row with its Fix";
    EXPECT_EQ(tile->nodeId, "n1");
    const size_t start = history()->position();
    ASSERT_TRUE(window.fixProblem(*tile)) << window.getStatus();
    EXPECT_EQ(history()->position(), start + 1);
    const double scale = window.getDocument()->graph().findNode("n1")->params["scale_x"].get<double>();
    EXPECT_EQ(scale, std::round(scale)) << "a whole number of repeats";
}

// Wiring (R9): a device failure the evaluation continued past on the CPU is a Problems row
TEST_F(ProceduralMaterialWindowTest, ADeviceFailureTheCpuTierContinuedPastIsAProblem) {
    Fake fake;
    fake.warnings = {"Pass 1 (normal) failed on test device: no. Continuing on the CPU tier, which is slower."};
    ProceduralMaterialWindow window(project(), fake.services());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeMaterial("fallback")));
    Frames frames;
    settle(window, frames);
    bool found = false;
    for (const auto& row : window.problems()) {
        found = found || (row.severity == graph::Severity::Warning && row.message.find("Pass 1 (normal) failed") != std::string::npos);
    }
    EXPECT_TRUE(found);
}

// re-review N1: between an edit and the next frame, no Variations cell counts as current
TEST_F(ProceduralMaterialWindowTest, AVariationCellIsNotCurrentRightAfterAnEdit) {
    Fake fake;
    ProceduralMaterialWindow window(project(), fake.services());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeMaterial("cells_now")));
    window.setVariationsOpen(true);
    Frames frames;
    settle(window, frames);
    ASSERT_TRUE(window.isVariationCurrent(0));
    ASSERT_TRUE(window.setSeed(5));
    EXPECT_FALSE(window.isVariationCurrent(0)) << "no frame since the edit";
    EXPECT_FALSE(window.adoptVariation(0));
}

// R27: a check with two fixes (two nodes named) is not a single edit: no Fix
TEST(ProceduralMaterialWindowReport, TwoFixesOfOneCheckAreNoFix) {
    const nlohmann::json report = {{"checks", nlohmann::json::array({{{"id", "tileability"}, {"status", "FAIL"}}})},
                                   {"fixes", nlohmann::json::array({
                                        {{"check", "tileability"}, {"node", "n1"}, {"op", {{"op", "setParam"}, {"node", "n1"}, {"name", "scale_x"}, {"value", 4}}}},
                                        {{"check", "tileability"}, {"node", "n2"}, {"op", {{"op", "setParam"}, {"node", "n2"}, {"name", "rows"}, {"value", 6}}}}})}};
    const std::vector<GraphEditorWindow::Problem> rows = ProceduralMaterialWindow::problemsFromReport(report);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].fix.empty());
}

namespace {

    // the window's own bake (bake_procedural_material's steps) on the tests' evaluators for the preview:
    // with no context the bake runs on the CPU tier
    ProceduralMaterialWindow::Services withTheBake(Fake& fake) {
        ProceduralMaterialWindow::Services services = fake.services();
        ProceduralMaterialWindow::Services defaults = ProceduralMaterialWindow::defaultServices();
        services.startBake = defaults.startBake;
        services.bakeState = defaults.bakeState;
        services.cancelBake = defaults.cancelBake;
        return services;
    }

    // Bake, and the editor's frames until it ends; its outcome
    std::string bakeAndWait(ProceduralMaterialWindow& window) {
        if (!window.bake()) return "refused: " + window.getBakeError();
        for (int i = 0; i < 6000 && window.bakeState().running; i++) {
            procmat::ProceduralRun::processFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return window.bakeState().message;
    }

    // a graph the editor's scan has seen (its GUID written then, as a project's files have theirs), so a bake
    // writes nothing into it
    std::string indexed(const std::string& path) {
        project()->onAssetFilesChanged(project()->getAssetDatabase().importAssets(project()->getProjectPath() / path));
        return path;
    }

    std::string bakedHash(const fs::path& png) {
        const std::optional<procmat::BakeRecipe> recipe = procmat::readBakeRecipe(png);
        return recipe ? recipe->inputHash : std::string();
    }

    std::string planHash(const graph::Graph& graph, int size, uint32_t seed) {
        procmat::BakeOptions options;
        options.size = size;
        options.seed = seed;
        procmat::PassPlan plan;
        std::vector<std::string> outputs;
        std::string error;
        if (!procmat::planBake(graph, procmat::NodeLibrary::shipped(), options, plan, outputs, error)) return "no plan: " + error;
        return procmat::bakeInputHash(plan);
    }

}

// Wiring round 2: an edit made just before Bake is baked (every window edit is written as its step, before Bake
// reads the file)
TEST_F(ProceduralMaterialWindowTest, AnEditMadeJustBeforeBakeIsBaked) {
    Fake fake;
    ProceduralMaterialWindow window(project(), withTheBake(fake));
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(indexed(writeMaterial("edit_bake"))));
    window.setBakeSize(64);
    const std::string before = planHash(window.getDocument()->graph(), 64, 0);
    ASSERT_TRUE(window.setParam("n2", "amount", 0.9));
    const std::string message = bakeAndWait(window);
    ASSERT_NE(message.find("Baked"), std::string::npos) << message;
    const std::string baked = bakedHash(assets() / "materials" / "edit_bake" / "edit_bake_base_color.png");
    EXPECT_EQ(baked, planHash(window.getDocument()->graph(), 64, 0)) << "the bake is of the graph as edited";
    EXPECT_NE(baked, before);
}

// Wiring round 2: Bake again re-bakes its own files; files of another graph at those paths are refused, never replaced
TEST_F(ProceduralMaterialWindowTest, BakeTwiceRebakesAndAnotherGraphsFilesAreRefused) {
    Fake fake;
    ProceduralMaterialWindow window(project(), withTheBake(fake));
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(indexed(writeMaterial("twice"))));
    window.setBakeSize(64);
    std::string message = bakeAndWait(window);
    ASSERT_NE(message.find("Baked"), std::string::npos) << message;
    ASSERT_TRUE(window.setParam("n2", "amount", 0.2));
    message = bakeAndWait(window);
    ASSERT_NE(message.find("Baked"), std::string::npos) << "a second Bake re-bakes its own files: " << message;
    const fs::path first = assets() / "materials" / "twice" / "twice_base_color.png";
    EXPECT_EQ(bakedHash(first), planHash(window.getDocument()->graph(), 64, 0));

    // another graph whose output path holds this one's bake
    const std::string other = indexed(writeMaterial("other_graph"));
    fs::create_directories(assets() / "materials" / "other_graph");
    const fs::path foreign = assets() / "materials" / "other_graph" / "other_graph_base_color.png";
    fs::copy_file(first, foreign, fs::copy_options::overwrite_existing);
    const std::string foreignBytes = readAll(foreign);
    ASSERT_TRUE(window.openGraph(other));
    message = bakeAndWait(window);
    EXPECT_EQ(message.rfind("refused: ", 0), 0u) << "refused before it runs: " << message;
    EXPECT_NE(window.getBakeError().find("other_graph_base_color.png"), std::string::npos) << window.getBakeError();
    EXPECT_NE(window.getBakeError().find("not a bake of this graph"), std::string::npos) << window.getBakeError();
    EXPECT_EQ(readAll(foreign), foreignBytes) << "nothing replaced";
}

// Wiring round 2: a row's Fix is for the revision it was found on: after an edit, even in the same frame, it is refused
TEST_F(ProceduralMaterialWindowTest, AFixFoundBeforeAnEditIsRefused) {
    Fake fake;
    fake.report = {{"checks", nlohmann::json::array({{{"id", "tileability"}, {"kind", "metric"}, {"status", "FAIL"}, {"measured", 3.2}}})},
                   {"fixes", nlohmann::json::array({{{"check", "tileability"}, {"node", "n2"},
                                                     {"op", {{"op", "setParam"}, {"node", "n2"}, {"name", "amount"}, {"value", 0.3}}}}})}};
    ProceduralMaterialWindow window(project(), fake.services());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeMaterial("revision")));
    Frames frames;
    settle(window, frames);
    GraphEditorWindow::Problem tile;
    for (const auto& row : window.problems()) {
        if (row.code == "tileability") tile = row;
    }
    ASSERT_EQ(tile.fix.size(), 1u);
    ASSERT_TRUE(window.setParam("n2", "amount", 0.7));
    const size_t start = history()->position();
    EXPECT_FALSE(window.fixProblem(tile)) << "found on the revision before the edit";
    EXPECT_EQ(history()->position(), start);
    EXPECT_DOUBLE_EQ(window.getDocument()->graph().findNode("n2")->params["amount"].get<double>(), 0.7);
}
