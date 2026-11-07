// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Noise Graph window's logic, headless (T2 R55-R59, N10, AC38, AC42): every gesture is one step of the
// project history, a parameter drag is one step on release, issues land on their nodes, a stale preview is
// dropped, the file is what is true (an outside edit refreshes, a broken file refuses edits), and the node
// search filters by a dragged pin's type. With the window closed there is no job at all.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "Engine.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/UndoTimeline.h"
#include "io/AtomicFile.h"
#include "noise/NoiseGraphDocument.h"
#include "noise/NoiseGraphRuntime.h"
#include "noise/TerrainPresets.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-noise-document-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            std::ofstream(out.dir / "project.yaml") << "name: noisedocumentproject\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            editor::test::releaseAtExit(out.project.get());
            return out;
        }();
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    // perlin -> remap -> output_height, and a lone fbm
    const char* kGraph = R"({
  "formatVersion": 1, "kind": "noise", "kindVersion": 1, "nextId": 10,
  "nodes": [
    {"id": "n1", "type": "perlin", "position": [0.0, 0.0], "params": {"frequency": 2.0}},
    {"id": "n2", "type": "remap", "position": [240.0, 0.0], "params": {"clamp": true}},
    {"id": "n3", "type": "output_height", "position": [480.0, 0.0], "params": {"unit": "normalized"}},
    {"id": "n4", "type": "fbm", "position": [0.0, 200.0], "params": {"octaves": 3}}
  ],
  "edges": [
    {"id": "e5", "from": {"node": "n1", "port": "out"}, "to": {"node": "n2", "port": "in"}},
    {"id": "e6", "from": {"node": "n2", "port": "out"}, "to": {"node": "n3", "port": "in"}}
  ],
  "variables": [], "groups": [], "viewport": {"offset": [0.0, 0.0], "zoom": 1.0}, "metadata": {"noise": {"space": "terrain"}}
})";

    class NoiseDocumentFixture {
    public:
        NoiseDocumentFixture() {
            noise::registerNoiseKind();
            project = headless().project.get();
            history = project->getProjectCommandHistory();
            history->clear();
            file = project->getAssetsPath() / "test.noisegraph";
            writeFile(kGraph);
            document = std::make_unique<NoiseGraphDocument>(project);
            opened = document->open(file);
        }
        ~NoiseDocumentFixture() {
            document.reset();
            history->clear();
            std::error_code error;
            fs::remove(file, error);
        }

        void writeFile(const std::string& text) const { ASSERT_TRUE(io::writeFileAtomic(file, text).ok()); }
        std::string readFile() const {
            std::ifstream in(file, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        // refresh() compares a stat: wait for the clock to move so a same-size rewrite shows
        void refreshUntilChanged() const {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!document->refresh() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::string undoName() const { return history->undoCommand() ? history->undoCommand()->describe() : std::string(); }

        Project* project = nullptr;
        CommandHistory* history = nullptr;
        fs::path file;
        std::unique_ptr<NoiseGraphDocument> document;
        bool opened = false;
    };

}

// T2 R57, AC42: a gesture is one undo step, named by graph::describe
TEST(NoiseGraphDocument, GesturesAreOneUndoStep) {
    NoiseDocumentFixture f;
    ASSERT_TRUE(f.opened) << f.document->loadError();
    NoiseGraphDocument& d = *f.document;
    const std::string original = f.readFile();

    const std::string added = d.addNode("simplex", graph::Vec2{0.0, 400.0});
    ASSERT_FALSE(added.empty()) << d.status();
    EXPECT_EQ(f.undoName(), "Add Simplex");
    EXPECT_NE(f.readFile(), original);
    ASSERT_TRUE(f.history->undo());
    EXPECT_EQ(f.readFile(), original);
    f.refreshUntilChanged();
    EXPECT_EQ(d.graph().findNode(added), nullptr);
    ASSERT_TRUE(f.history->redo());
    f.refreshUntilChanged();
    EXPECT_NE(d.graph().findNode(added), nullptr);

    // A rewire of a taken input is one step too: the old edge goes with the same undo
    const std::string beforeConnect = f.readFile();
    ASSERT_TRUE(d.connectPins(d.pinOf("n4", "out"), d.pinOf("n2", "in"))) << d.status();
    EXPECT_EQ(f.undoName(), "Connect FBm \xE2\x86\x92 Remap");
    size_t feedingRemap = 0;
    for (const graph::Edge& edge : d.graph().edges) feedingRemap += edge.to.node == "n2" && edge.to.port == "in";
    EXPECT_EQ(feedingRemap, 1u);
    ASSERT_TRUE(f.history->undo());
    EXPECT_EQ(f.readFile(), beforeConnect);
    f.refreshUntilChanged();

    // Delete of several: one step, by count
    d.setSelection({"n1", "n4", added}, {});
    const size_t depth = f.history->canUndo() ? 1 : 0;
    EXPECT_EQ(depth, 1u);
    const std::string beforeDelete = f.readFile();
    ASSERT_TRUE(d.deleteSelection()) << d.status();
    EXPECT_EQ(f.undoName(), "Delete 3 Nodes");
    EXPECT_EQ(d.graph().findNode("n1"), nullptr);
    ASSERT_TRUE(f.history->undo());
    EXPECT_EQ(f.readFile(), beforeDelete);
    f.refreshUntilChanged();
    EXPECT_NE(d.graph().findNode("n1"), nullptr);

    // A single wire by its own name, and a move
    d.setSelection({}, {"e5"});
    ASSERT_TRUE(d.deleteSelection());
    EXPECT_EQ(f.undoName(), "Disconnect");
    ASSERT_TRUE(f.history->undo());
    f.refreshUntilChanged();
    ASSERT_TRUE(d.moveNodes({{"n1", graph::Vec2{20.0, 20.0}}, {"n2", graph::Vec2{260.0, 20.0}}}));
    EXPECT_EQ(f.undoName(), "Move 2 Nodes");
    ASSERT_TRUE(f.history->undo());
    f.refreshUntilChanged();

    // Copy, paste and duplicate: new ids, the inner wire kept, one step each
    d.setSelection({"n1", "n2"}, {});
    ASSERT_TRUE(d.copySelection());
    const size_t nodesBefore = d.graph().nodes.size();
    const size_t edgesBefore = d.graph().edges.size();
    ASSERT_TRUE(d.paste(graph::Vec2{600.0, 600.0})) << d.status();
    EXPECT_EQ(f.undoName(), "Paste 2 Nodes");
    EXPECT_EQ(d.graph().nodes.size(), nodesBefore + 2);
    EXPECT_EQ(d.graph().edges.size(), edgesBefore + 1);
    EXPECT_EQ(d.selectedNodes().size(), 2u);
    ASSERT_TRUE(f.history->undo());
    f.refreshUntilChanged();
    d.setSelection({"n1"}, {});
    ASSERT_TRUE(d.duplicateSelection()) << d.status();
    EXPECT_EQ(f.undoName(), "Duplicate Node");
    EXPECT_EQ(d.graph().nodes.size(), nodesBefore + 1);
    ASSERT_TRUE(f.history->undo());
    f.refreshUntilChanged();
    EXPECT_EQ(d.graph().nodes.size(), nodesBefore);
}

// T2 R57, AC42: a parameter drag writes nothing until release, and is one step
TEST(NoiseGraphDocument, DragCommitsOnRelease) {
    NoiseDocumentFixture f;
    ASSERT_TRUE(f.opened);
    NoiseGraphDocument& d = *f.document;
    const std::string original = f.readFile();
    const uint64_t startRevision = d.revision();
    const bool hadUndo = f.history->canUndo();

    d.beginParamDrag("n1", "frequency");
    d.updateParamDrag(3.0);
    d.updateParamDrag(4.0);
    EXPECT_GT(d.revision(), startRevision) << "the previews see the working value";
    EXPECT_EQ(d.graph().findNode("n1")->params["frequency"].get<double>(), 4.0);
    EXPECT_EQ(f.readFile(), original) << "a drag does not write";
    EXPECT_EQ(f.history->canUndo(), hadUndo) << "a drag is no step yet";

    ASSERT_TRUE(d.commitParamDrag()) << d.status();
    EXPECT_NE(f.readFile(), original);
    EXPECT_EQ(f.undoName(), "Set frequency");
    ASSERT_TRUE(f.history->undo());
    EXPECT_EQ(f.readFile(), original) << "one undo takes the whole drag back";
    EXPECT_EQ(f.history->canUndo(), hadUndo);
    f.refreshUntilChanged();
    EXPECT_EQ(d.graph().findNode("n1")->params["frequency"].get<double>(), 2.0);

    // Cancelled: the value comes back and nothing is written
    d.beginParamDrag("n1", "frequency");
    d.updateParamDrag(9.0);
    d.cancelParamDrag();
    EXPECT_EQ(d.graph().findNode("n1")->params["frequency"].get<double>(), 2.0);
    EXPECT_EQ(f.readFile(), original);
}

// T2 R58, AC42: an issue with a node is on that node, its port highlighted; one without goes to the bar;
// Apply's tooltip is the first error
TEST(NoiseGraphDocument, IssuesMapToNodes) {
    NoiseDocumentFixture f;
    ASSERT_TRUE(f.opened);
    NoiseGraphDocument& d = *f.document;
    EXPECT_FALSE(d.hasErrors()) << d.firstError();
    EXPECT_TRUE(d.isDead("n4")) << "the lone fbm reaches no output";

    // no input on the remap: missing-required-input on its port
    d.setSelection({}, {"e5"});
    ASSERT_TRUE(d.deleteSelection());
    EXPECT_TRUE(d.hasErrors());
    const std::string message = d.nodeError("n2");
    EXPECT_FALSE(message.empty());
    EXPECT_EQ(d.firstError(), message);
    bool portMarked = false;
    for (const NoiseGraphNodeView& view : d.nodeViews()) {
        if (view.nodeId != "n2") continue;
        EXPECT_EQ(view.errorText, message);
        for (const NoiseGraphPinView& pin : view.inputs) portMarked = portMarked || (pin.port == "in" && pin.error);
    }
    EXPECT_TRUE(portMarked);
    EXPECT_TRUE(d.nodeError("n1").empty());

    // no output node at all: an issue without a node
    d.setSelection({"n3"}, {});
    ASSERT_TRUE(d.deleteSelection());
    bool noOutput = false;
    for (const graph::Issue& issue : d.issuesWithoutNode()) noOutput = noOutput || issue.code == "no-output";
    EXPECT_TRUE(noOutput);
}

// T2 R56, N10, AC42: a result of an older revision is dropped, at the service and at the document
TEST(NoiseGraphDocument, StaleResultsAreDropped) {
    NoiseDocumentFixture f;
    ASSERT_TRUE(f.opened);
    NoiseGraphDocument& d = *f.document;

    d.update(true);
    const uint64_t firstRevision = d.revision();
    EXPECT_EQ(d.previewService().requestedRevision(), firstRevision);
    EXPECT_EQ(d.previewService().jobsSubmitted(), 1u);
    // the edit comes before the first job's result is read
    d.previewService().waitForCurrent();
    ASSERT_TRUE(d.setParam("n1", "frequency", 8.0));
    ASSERT_GT(d.revision(), firstRevision);
    EXPECT_FALSE(d.update(true)) << "the first revision's result is not taken";
    EXPECT_EQ(d.droppedPreviews(), 1u);
    EXPECT_EQ(d.previewedRevision(), 0u);
    EXPECT_EQ(d.previewService().jobsSubmitted(), 2u);
    EXPECT_EQ(d.previewService().requestedRevision(), d.revision());

    d.previewService().waitForCurrent();
    EXPECT_TRUE(d.update(true));
    EXPECT_EQ(d.previewedRevision(), d.revision());
    const NoiseNodePreview* perlin = d.nodePreview("n1");
    ASSERT_NE(perlin, nullptr);
    EXPECT_EQ(perlin->rgba.size(), static_cast<size_t>(64 * 64 * 4));
    EXPECT_LE(perlin->analytic.min, perlin->observed.min);
    EXPECT_GE(perlin->analytic.max, perlin->observed.max);
    EXPECT_NE(d.nodePreview("n3"), nullptr) << "the output node shows what it writes";

    // A newer request cancels the older: only the newest is ever polled
    NoisePreviewService service;
    NoisePreviewRequest older;
    older.graph = d.graph();
    older.revision = 100;
    NoisePreviewRequest newer = older;
    newer.revision = 101;
    service.request(std::move(older));
    service.request(std::move(newer));
    EXPECT_EQ(service.jobsSubmitted(), 2u);
    service.waitForCurrent();
    NoisePreviewResult result;
    ASSERT_TRUE(service.poll(result));
    EXPECT_EQ(result.revision, 101u);
    EXPECT_FALSE(service.poll(result)) << "a result comes once";

    // And the document drops one from any older request it is handed
    NoisePreviewResult stale;
    stale.revision = d.revision() - 1;
    stale.ok = true;
    const uint64_t droppedBefore = d.droppedPreviews();
    EXPECT_FALSE(d.acceptPreview(std::move(stale)));
    EXPECT_EQ(d.droppedPreviews(), droppedBefore + 1);
}

// T2 R50, R56: a coord output is two channels, thumbnails go up at most 8 a frame
TEST(NoiseGraphDocument, ThumbnailUploadsAreBudgeted) {
    NoiseDocumentFixture f;
    ASSERT_TRUE(f.opened);
    NoiseGraphDocument& d = *f.document;
    for (int i = 0; i < 12; i++) ASSERT_FALSE(d.addNode("position", graph::Vec2{-300.0, i * 100.0}).empty());
    d.update(true);
    d.previewService().waitForCurrent();
    ASSERT_TRUE(d.update(true));
    size_t total = 0;
    for (std::vector<std::string> batch = d.takeThumbnailUploads(8); !batch.empty(); batch = d.takeThumbnailUploads(8)) {
        EXPECT_LE(batch.size(), 8u);
        total += batch.size();
    }
    EXPECT_GE(total, 12u);
    bool coord = false;
    for (const graph::Node& node : d.graph().nodes) {
        if (node.type != "position") continue;
        const NoiseNodePreview* preview = d.nodePreview(node.id);
        ASSERT_NE(preview, nullptr);
        coord = preview->twoChannels;
    }
    EXPECT_TRUE(coord);
}

// T2 R57, AC42: the file moved by someone else (MCP, a text editor): the window follows it
TEST(NoiseGraphDocument, ExternalEditRefreshes) {
    NoiseDocumentFixture f;
    ASSERT_TRUE(f.opened);
    NoiseGraphDocument& d = *f.document;
    d.setSelection({"n4"}, {});

    graph::LoadReport report;
    graph::Graph edited = graph::load(f.readFile(), report);
    ASSERT_TRUE(report.ok);
    edited.findNode("n1")->params["frequency"] = 5.0;
    edited.nodes.erase(std::remove_if(edited.nodes.begin(), edited.nodes.end(), [](const graph::Node& n) { return n.id == "n4"; }), edited.nodes.end());
    const uint64_t revision = d.revision();
    f.writeFile(graph::save(edited));
    f.refreshUntilChanged();

    EXPECT_GT(d.revision(), revision);
    EXPECT_EQ(d.graph().findNode("n1")->params["frequency"].get<double>(), 5.0);
    EXPECT_EQ(d.graph().findNode("n4"), nullptr);
    EXPECT_TRUE(d.selectedNodes().empty()) << "a node that went leaves the selection";
    EXPECT_FALSE(d.refresh()) << "nothing moved since";

    // An edit made on a stale read is refused, not written over the other's work
    f.writeFile(graph::save(edited) + "\n");
    EXPECT_FALSE(d.setParam("n1", "frequency", 1.0));
    EXPECT_NE(d.status().find("changed on disk"), std::string::npos) << d.status();
    EXPECT_EQ(f.readFile(), graph::save(edited) + "\n");
}

// T2 R57, AC42: a file that stops parsing shows its error, refuses every edit, is left as it is, and a repair clears it
TEST(NoiseGraphDocument, BrokenFileRefusesEdits) {
    NoiseDocumentFixture f;
    ASSERT_TRUE(f.opened);
    NoiseGraphDocument& d = *f.document;
    const std::string good = f.readFile();
    const std::string broken = "{ this is not a graph";
    f.writeFile(broken);
    f.refreshUntilChanged();

    EXPECT_FALSE(d.loadError().empty());
    EXPECT_FALSE(d.canEdit());
    EXPECT_TRUE(d.addNode("perlin", graph::Vec2{}).empty());
    EXPECT_FALSE(d.setParam("n1", "frequency", 1.0));
    EXPECT_FALSE(d.moveNodes({{"n1", graph::Vec2{1.0, 1.0}}}));
    EXPECT_FALSE(d.connectPins(d.pinOf("n4", "out"), d.pinOf("n2", "in")));
    d.beginParamDrag("n1", "frequency");
    EXPECT_FALSE(d.isParamDragging());
    d.setSelection({"n1"}, {});
    EXPECT_FALSE(d.deleteSelection());
    EXPECT_TRUE(d.hasErrors());
    EXPECT_EQ(d.firstError(), d.loadError());
    EXPECT_EQ(f.readFile(), broken) << "nothing overwrote the file";

    // no preview job on a file that does not parse
    const uint64_t jobs = d.previewService().jobsSubmitted();
    d.update(true);
    EXPECT_EQ(d.previewService().jobsSubmitted(), jobs);

    f.writeFile(good);
    f.refreshUntilChanged();
    EXPECT_TRUE(d.loadError().empty());
    EXPECT_TRUE(d.canEdit());
    EXPECT_TRUE(d.setParam("n1", "frequency", 1.0)) << d.status();
}

// T2 R59, AC42: by name and category; a dragged pin's type filters
TEST(NoiseGraphDocument, SearchFiltersByPinType) {
    NoiseDocumentFixture f;
    ASSERT_TRUE(f.opened);
    NoiseGraphDocument& d = *f.document;
    const auto has = [](const std::vector<NoiseGraphSearchEntry>& list, const char* type) {
        return std::any_of(list.begin(), list.end(), [type](const NoiseGraphSearchEntry& e) { return e.typeId == type; });
    };

    const std::vector<NoiseGraphSearchEntry> all = d.search("");
    EXPECT_TRUE(has(all, "perlin"));
    EXPECT_TRUE(has(all, "output_height"));
    EXPECT_TRUE(has(d.search("PERL"), "perlin"));
    EXPECT_FALSE(has(d.search("perl"), "fbm"));
    const std::vector<NoiseGraphSearchEntry> inputs = d.search("input");
    EXPECT_TRUE(has(inputs, "constant")) << "by category";

    // from a field output: what has a field input, not what only makes values
    const uint64_t fieldOut = d.pinOf("n1", "out");
    ASSERT_NE(fieldOut, 0u);
    const std::vector<NoiseGraphSearchEntry> fromField = d.search("", fieldOut);
    EXPECT_TRUE(has(fromField, "output_height"));
    EXPECT_TRUE(has(fromField, "remap"));
    EXPECT_FALSE(has(fromField, "constant")) << "a constant has no input";
    for (const NoiseGraphSearchEntry& entry : fromField) EXPECT_FALSE(entry.port.empty()) << entry.typeId;

    // from a field input: what has a field output, not the outputs
    const uint64_t fieldIn = d.pinOf("n2", "in");
    const std::vector<NoiseGraphSearchEntry> intoField = d.search("", fieldIn);
    EXPECT_TRUE(has(intoField, "perlin"));
    EXPECT_TRUE(has(intoField, "constant"));
    EXPECT_FALSE(has(intoField, "output_height"));
}

// T2 R59, AC42: a wire dropped on empty space adds the node and connects its first compatible port, as one step
TEST(NoiseGraphDocument, DropAddsAndConnects) {
    NoiseDocumentFixture f;
    ASSERT_TRUE(f.opened);
    NoiseGraphDocument& d = *f.document;
    const std::string original = f.readFile();

    // dragged out of an output: the new node's input
    const std::string remap = d.addFromSearch("remap", graph::Vec2{0.0, 500.0}, d.pinOf("n4", "out"));
    ASSERT_FALSE(remap.empty()) << d.status();
    bool connected = false;
    for (const graph::Edge& edge : d.graph().edges) connected = connected || (edge.from.node == "n4" && edge.from.port == "out" && edge.to.node == remap);
    EXPECT_TRUE(connected);
    EXPECT_EQ(f.undoName(), "Add Remap");
    ASSERT_TRUE(f.history->undo());
    EXPECT_EQ(f.readFile(), original) << "the node and its wire were one step";
    f.refreshUntilChanged();

    // dragged out of an input already fed: the new node's output feeds it, the old wire goes
    const std::string constant = d.addFromSearch("constant", graph::Vec2{0.0, 500.0}, d.pinOf("n2", "in"));
    ASSERT_FALSE(constant.empty()) << d.status();
    size_t feeding = 0;
    bool fromNew = false;
    for (const graph::Edge& edge : d.graph().edges) {
        if (edge.to.node == "n2" && edge.to.port == "in") {
            feeding++;
            fromNew = edge.from.node == constant;
        }
    }
    EXPECT_EQ(feeding, 1u);
    EXPECT_TRUE(fromNew);

    // refused pins: a loop, and a pin with itself
    EXPECT_FALSE(d.canConnectPins(d.pinOf("n3", "in"), d.pinOf("n2", "out"))) << "wrong way round";
    EXPECT_FALSE(d.canConnectPins(d.pinOf("n2", "out"), d.pinOf("n2", "in"))) << "a node with itself";
    ASSERT_TRUE(d.connectPins(d.pinOf("n1", "out"), d.pinOf("n2", "in")));
    const std::string math = d.addNode("math", graph::Vec2{});
    ASSERT_TRUE(d.connectPins(d.pinOf("n2", "out"), d.pinOf(math, "a")));
    EXPECT_FALSE(d.canConnectPins(d.pinOf(math, "out"), d.pinOf("n2", "in"))) << "a loop";
}

// T2 R50, AC38: closed, no job and nothing kept
TEST(NoiseGraphDocument, PreviewsAreSkippedWhenClosed) {
    NoiseDocumentFixture f;
    ASSERT_TRUE(f.opened);
    NoiseGraphDocument& d = *f.document;

    for (int i = 0; i < 5; i++) EXPECT_FALSE(d.update(false));
    ASSERT_TRUE(d.setParam("n1", "frequency", 3.0));
    EXPECT_FALSE(d.update(false));
    EXPECT_EQ(d.previewService().jobsSubmitted(), 0u) << "a closed window runs no job";
    EXPECT_FALSE(d.previewService().busy());

    // open: one job. Closing it cancels the job and drops what it showed.
    d.update(true);
    EXPECT_EQ(d.previewService().jobsSubmitted(), 1u);
    d.previewService().waitForCurrent();
    EXPECT_TRUE(d.update(true));
    EXPECT_NE(d.nodePreview("n1"), nullptr);
    EXPECT_FALSE(d.takeThumbnailUploads(1).empty());
    d.update(false);
    EXPECT_EQ(d.nodePreview("n1"), nullptr);
    EXPECT_EQ(d.previewImage().size, 0);
    EXPECT_TRUE(d.takeThumbnailUploads(100).empty());
    EXPECT_FALSE(d.previewService().busy());
    d.update(false);
    EXPECT_EQ(d.previewService().jobsSubmitted(), 1u);

    // reopening previews again
    d.update(true);
    EXPECT_EQ(d.previewService().jobsSubmitted(), 2u);
}

// T2 R57, S24 R17: what Ctrl+Z, Edit > Undo and the History panel run is the one timeline, which holds the
// project history a graph edit lands in. The Noise Graph window claims no Undo of its own (AppCommands.cpp),
// so the last graph edit is what Ctrl+Z takes back, whatever scene is selected.
TEST(NoiseGraphDocument, CtrlZUndoesTheLastGraphEdit) {
    NoiseDocumentFixture f;
    ASSERT_TRUE(f.opened) << f.document->loadError();
    constexpr uint32_t sceneId = 9301;
    CommandHandle::get(sceneId);
    const std::string original = f.readFile();
    ASSERT_FALSE(f.document->addNode("simplex", graph::Vec2{0.0, 400.0}).empty()) << f.document->status();
    EXPECT_EQ(UndoTimeline::undoLabel(sceneId).find("Add Simplex") != std::string::npos, true) << UndoTimeline::undoLabel(sceneId);
    std::string why;
    EXPECT_TRUE(UndoTimeline::undo(sceneId, &why)) << why;
    EXPECT_EQ(f.readFile(), original);
    EXPECT_TRUE(UndoTimeline::redo(sceneId, &why)) << why;
    EXPECT_NE(f.readFile(), original);
    CommandHandle::remove(sceneId);
}
