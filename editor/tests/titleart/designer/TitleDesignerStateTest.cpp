// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Title Designer's documents and selection (docs/specs/S82-title-designer.md D3, D8, R8, R12,
// R22, R23, R26, R44, R74, R75; AC5, AC6, AC7, AC8, AC9, AC29) on a headless project, CPU only:
// staging and one step per gesture, Esc, previews that commit to the very document they showed,
// outside changes, node paths, the Esc walk, and the collect pass of the Properties pages (the
// search index read from the real views, every numeric row with its schema default).

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "JasperEngine.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "titleart/TitleArtDocument.h"
#include "titleart/TitleArtIO.h"
#include "titleart/designer/TitleDesignerPrefs.h"
#include "titleart/designer/TitleDesignerState.h"
#include "titleart/designer/TitlePages.h"
#include "titleart/designer/TitlePropertyIndex.h"
#include "titleart/designer/TitleRows.h"
#include "widget/DocRow.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace fs = std::filesystem;
namespace ta = jasperengine::titleart;

namespace {

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-title-designer-state-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            std::ofstream(out.dir / "project.yaml") << "name: titledesignerstate\nassetsDir: assets\n";
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

    Project* project() { return headless().project.get(); }
    CommandHistory* history() { return project()->getProjectCommandHistory(); }
    std::string topName() {
        const UndoEntry* top = history()->top();
        return top ? top->name() : std::string();
    }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // a document with a text layer "Title" of three rims (widths 2, 3, 5) and an outer glow
    fs::path makeDocument(const std::string& name) {
        fs::create_directories(headless().dir / "assets" / "designer");
        const std::string relative = "assets/designer/" + name + ".titleart";
        fs::remove(headless().dir / relative);
        std::string error;
        std::shared_ptr<TitleArtDocument> document = TitleArtDocument::create(project(), relative, nullptr, "Create Title Art", &error);
        EXPECT_TRUE(document) << error;
        const ta::Json ops = ta::Json::parse(R"([
            {"op": "add_rim", "layer": "Title", "values": {"width": 2}},
            {"op": "add_rim", "layer": "Title", "values": {"width": 3}},
            {"op": "add_rim", "layer": "Title", "values": {"width": 5}},
            {"op": "add_effect", "layer": "Title", "type": "outer_glow"}
        ])");
        const TitleOpsResult result = document->edit(ops);
        EXPECT_TRUE(result.ok) << result.error();
        return document->absolutePath();
    }

    std::string layerId(const TitleDesignerState& state, const std::string& name) {
        std::string out;
        ta::forEachLayer(state.committed().root, [&](const ta::Json& l, const ta::Json*, int) {
            if (l.value("name", std::string()) == name) out = l.value("id", std::string());
            return true;
        });
        return out;
    }

    class TitleDesignerStateTest : public ::testing::Test {
    protected:
        void SetUp() override { ASSERT_TRUE(headless().ready) << "the headless project did not open"; }
        void TearDown() override { TitleArtDocument::closeAll(); }
    };

    // a minimal host for the collect pass
    class Host : public PropertyHost {
    public:
        explicit Host(TitleDesignerState& state) : state(state) {}
        TitleDesignerState& designerState() override { return state; }
        bool sectionOpen(const std::string&, bool defaultOpen) override { return defaultOpen; }
        void setSectionOpen(const std::string&, bool) override {}
        TitleDesignerState& state;
    };

}

// AC29 (R75, D3): a gesture stages every frame and commits once on release; Esc adds nothing.
// Red control: setCommitPerStageForTesting (a commit per frame) gives N steps and fails.
TEST_F(TitleDesignerStateTest, OneStepPerGestureAndEscRestores) {
    TitleDesignerState state(project());
    ASSERT_TRUE(state.open(makeDocument("gesture"), nullptr));
    const std::string title = layerId(state, "Title");
    const std::string before = state.document()->text();
    const size_t position = history()->position();

    state.beginGesture("Rotate Layer");
    for (int frame = 1; frame <= 30; frame++) {
        ASSERT_TRUE(state.stage(editsToOps({{title, "transform.rotation", frame * 1.5}}), nullptr));
    }
    EXPECT_EQ(state.working().layers()[0]["transform"]["rotation"].get<double>(), 45.0) << "the working copy shows the drag";
    EXPECT_EQ(state.document()->text(), before) << "nothing written while staged";
    ASSERT_TRUE(state.endGesture(nullptr));
    EXPECT_EQ(history()->position(), position + 1) << "one step for the 30-frame drag";
    EXPECT_EQ(topName(), "Rotate Layer");

    const std::string after = state.document()->text();
    state.beginGesture("Rotate Layer");
    ASSERT_TRUE(state.stage(editsToOps({{title, "transform.rotation", 90.0}}), nullptr));
    state.cancelGesture();
    EXPECT_EQ(state.document()->text(), after) << "Esc restores";
    EXPECT_EQ(state.working().root, state.committed().root);
    EXPECT_EQ(history()->position(), position + 1) << "Esc adds no step";
}

// AC5 (R22): shift-each moves each rim from its own start; set-all gives each the value; the
// step says how many. Red control: applying the scrub as set-all gives 4, 4, 4.
TEST_F(TitleDesignerStateTest, ShiftEachVersusSetAll) {
    TitleDesignerState state(project());
    ASSERT_TRUE(state.open(makeDocument("multitarget"), nullptr));
    const std::string title = layerId(state, "Title");
    std::vector<FieldRef> rims;
    for (int i = 0; i < 3; i++) rims.push_back({title, "style.rims[" + std::to_string(i) + "].width"});
    ui::DocRowOptions options;
    options.hardMin = 0;
    options.hardMax = 100;
    std::vector<FieldEdit> edits;
    for (const FieldRef& rim : rims) {
        const double start = fieldValue(state.committed(), rim)->get<double>();
        edits.push_back({rim.layer, rim.path, ui::shiftEach(start, 2.0, options)});
    }
    const std::string name = stepName("Change", "Rim Width", rims.size(), "Rims");
    EXPECT_EQ(name, "Change Rim Width on 3 Rims");
    state.beginGesture(name);
    ASSERT_TRUE(state.stage(editsToOps(edits), nullptr));
    ASSERT_TRUE(state.endGesture(nullptr));
    EXPECT_EQ(topName(), "Change Rim Width on 3 Rims");
    EXPECT_DOUBLE_EQ(fieldValue(state.committed(), rims[0])->get<double>(), 4.0);
    EXPECT_DOUBLE_EQ(fieldValue(state.committed(), rims[1])->get<double>(), 5.0);
    EXPECT_DOUBLE_EQ(fieldValue(state.committed(), rims[2])->get<double>(), 7.0);
    std::vector<FieldEdit> set;
    for (const FieldRef& rim : rims) set.push_back({rim.layer, rim.path, 6.0});
    ASSERT_TRUE(state.commit(editsToOps(set), stepName("Change", "Rim Width", 3, "Rims"), nullptr));
    for (const FieldRef& rim : rims) EXPECT_DOUBLE_EQ(fieldValue(state.committed(), rim)->get<double>(), 6.0);
}

// R44 / AC19's core: a preview is the same ops on the working copy with the seed its commit
// uses, so the committed document equals the previewed one byte for byte, and a preview is
// never a step (D8)
TEST_F(TitleDesignerStateTest, CommittedPreviewEqualsThePreviewedDocument) {
    TitleDesignerState state(project());
    ASSERT_TRUE(state.open(makeDocument("preview"), nullptr));
    const std::string title = layerId(state, "Title");
    const size_t position = history()->position();
    PreviewItem item;
    item.kind = PreviewItem::Kind::Effect;
    item.id = "drop_shadow";
    item.ops = ta::Json::array({ta::Json{{"op", "add_effect"}, {"layer", "id:" + title}, {"type", "drop_shadow"}}});
    item.stepName = "Add Effect Drop Shadow to Title";
    ASSERT_TRUE(state.setPreview(item, nullptr));
    EXPECT_TRUE(state.previewActive());
    EXPECT_EQ(history()->position(), position) << "a preview is no step";
    EXPECT_NE(ta::writeTitleArt(state.shown()), state.document()->text());
    const std::string previewed = ta::writeTitleArt(state.shown());
    ASSERT_TRUE(state.commitPreview(nullptr));
    EXPECT_EQ(history()->position(), position + 1);
    EXPECT_EQ(topName(), "Add Effect Drop Shadow to Title");
    EXPECT_EQ(state.document()->text(), previewed) << "the commit is the previewed document, ids included";
    // clearing a preview restores the working document
    item.id = "inner_shadow";
    item.ops = ta::Json::array({ta::Json{{"op", "add_effect"}, {"layer", "id:" + title}, {"type", "inner_shadow"}}});
    ASSERT_TRUE(state.setPreview(item, nullptr));
    state.clearPreview();
    EXPECT_EQ(ta::writeTitleArt(state.shown()), ta::writeTitleArt(state.working()));
}

// R74: an outside commit (an agent's edit_title) while a drag is staged cancels the drag, keeps
// the selection by id and reports the change
TEST_F(TitleDesignerStateTest, OutsideChangeCancelsTheStagedDrag) {
    TitleDesignerState state(project());
    ASSERT_TRUE(state.open(makeDocument("outside"), nullptr));
    const std::string title = layerId(state, "Title");
    state.selectOne({NodeKind::Layer, title, {}, {}});
    state.beginGesture("Move Layer");
    ASSERT_TRUE(state.stage(editsToOps({{title, "transform.position", ta::Json::array({40, 0})}}), nullptr));
    TitleArtEditOptions options;
    options.origin = TitleArtChangeOrigin::Agent;
    ASSERT_TRUE(state.document()->edit(ta::Json::parse(R"([{"op": "set", "layer": "Title", "values": {"size": 240}}])"), options).ok);
    EXPECT_FALSE(state.gestureActive()) << "the drag is cancelled";
    const std::optional<ExternalChange> change = state.takeExternalChange();
    ASSERT_TRUE(change.has_value());
    EXPECT_TRUE(change->cancelledGesture);
    EXPECT_EQ(change->origin, TitleArtChangeOrigin::Agent);
    ASSERT_EQ(state.selection().size(), 1u);
    EXPECT_EQ(state.selection()[0].layer, title) << "kept by id";
    EXPECT_EQ(state.working().layers()[0]["size"].get<double>(), 240.0);
}

// AC6 (R12): Esc walks effect -> layer -> group -> document; selection is never a step
TEST_F(TitleDesignerStateTest, NodePathsAndTheEscWalk) {
    TitleDesignerState state(project());
    ASSERT_TRUE(state.open(makeDocument("paths"), nullptr));
    const std::string title = layerId(state, "Title");
    ASSERT_TRUE(state.commit(ta::Json::parse(R"([{"op": "group", "layers": ["Title"], "name": "Logo"}])"), "Group Layers", nullptr));
    const size_t position = history()->position();
    NodeRef node;
    std::string error;
    ASSERT_TRUE(nodeFromPath(state.committed(), "Logo/Title/fx:outer_glow", node, &error)) << error;
    EXPECT_EQ(node.kind, NodeKind::Effect);
    EXPECT_EQ(nodePath(state.committed(), node), "Logo/Title/fx:outer_glow");
    ASSERT_TRUE(nodeFromPath(state.committed(), "Logo/Title/rim#1", node, &error)) << error;
    EXPECT_EQ(node.kind, NodeKind::Part);
    EXPECT_EQ(nodePath(state.committed(), node), "Logo/Title/rim#1");
    EXPECT_FALSE(nodeFromPath(state.committed(), "Logo/Nope", node, &error));
    EXPECT_NE(error.find("valid here: Title"), std::string::npos) << error;

    ASSERT_TRUE(nodeFromPath(state.committed(), "Logo/Title/fx:outer_glow", node, &error));
    state.selectOne(node);
    state.selectParent();
    EXPECT_EQ(state.primary().kind, NodeKind::Layer);
    EXPECT_EQ(state.primary().layer, title);
    state.selectParent();
    EXPECT_EQ(state.primary().kind, NodeKind::Layer);
    EXPECT_EQ(layerPathOf(state.committed(), state.primary().layer), "Logo");
    state.selectParent();
    EXPECT_EQ(state.primary().kind, NodeKind::Document);
    EXPECT_EQ(history()->position(), position) << "selection is view state";
}

// AC9 (R26, D4): the index comes from the drawn views: a row hidden by its `when` is not a hit
// until its condition holds, a section only a 3D layer has never appears on a 2D one, and the
// Japanese and synonym searches reach the real rows. Red control: an index built from the
// schema tables (every pattern) would list bevel.fit and the 3D rows.
TEST_F(TitleDesignerStateTest, IndexFromViews) {
    TitleDesignerState state(project());
    ASSERT_TRUE(state.open(makeDocument("index"), nullptr));
    const std::string title = layerId(state, "Title");
    state.selectOne({NodeKind::Layer, title, {}, {}});
    // a section S86 would register: only on Dimension 3D layers
    SectionDef section;
    section.page = SectionDef::Page::Layer;
    section.id = "test3d";
    section.title = "Extrusion";
    section.applies = [](const TitleDesignerState& s) {
        for (const std::string& id : s.selectedLayers()) {
            const ta::Json* l = layerById(s.shown(), id);
            if (l && l->value("dimension", std::string("2d")) == "3d") return true;
        }
        return false;
    };
    section.draw = [](RowContext& ctx, const std::vector<std::string>& layers, const std::vector<std::string>&) {
        fieldRow(ctx, {{layers[0], "transform.rotation"}}, {.label = "Extrusion Depth"});
    };
    addSection(section);
    Host host(state);
    PropertyIndex index;
    collectIndex(state, &host, index, false);
    removeSection("test3d");
    auto hasLabel = [&](const std::string& query, const std::string& label) {
        for (const SearchHit& hit : index.query(query, 0)) {
            if (index.entry(hit.entry).label == label) return true;
        }
        return false;
    };
    auto labels = [&](const std::string& query) {
        std::set<std::string> out;
        for (const SearchHit& hit : index.query(query, 0)) out.insert(index.entry(hit.entry).fieldPath);
        return out;
    };
    EXPECT_FALSE(hasLabel("Extrusion Depth", "Extrusion Depth")) << "a 3D-only row is never a hit on a 2D layer";
    EXPECT_TRUE(labels("Fit").count("style.face.bevel.fit") == 0) << "hidden by size_mode: fixed";
    const std::set<std::string> rimRows = labels("\xE3\x83\x95\xE3\x83\x81");   // フチ
    EXPECT_TRUE(rimRows.count("style.rims[0].width")) << "フチ finds the rim widths";
    const std::set<std::string> blurJa = labels("\xE3\x81\xBC\xE3\x81\x8B\xE3\x81\x97");   // ぼかし
    const std::set<std::string> blurEn = labels("blur");
    EXPECT_FALSE(blurJa.empty());
    EXPECT_EQ(blurJa, blurEn) << "ぼかし and blur find the same rows";
    EXPECT_TRUE(labels("Ring").count("style.face.bevel.gloss")) << "an enum value label finds its row";
    bool glowOffer = false;
    for (const OfferHit& hit : index.queryOffers("\xE5\x85\x89\xE5\xBD\xA9", 10)) glowOffer = glowOffer || index.offer(hit.offer).type == "outer_glow";   // 光彩
    EXPECT_TRUE(glowOffer) << "光彩 offers Outer Glow";
    // the condition holds: the row appears
    ASSERT_TRUE(state.commit(editsToOps({{title, "style.face.bevel.size_mode", "fit"}}), "Change Bevel Size Mode", nullptr));
    collectIndex(state, &host, index, false);
    EXPECT_TRUE(labels("Fit").count("style.face.bevel.fit")) << "shown once size_mode is fit";
}

// AC7 (R20, R21): every numeric row the collect pass sees on every page of a document with every
// layer type has its schema (or ParamDef) default and a hard range, so its reset arrow has a
// target. Red control: a row whose default the lookup drops (an override that returns null) is
// reported by name.
TEST_F(TitleDesignerStateTest, EveryNumericRowHasItsDefault) {
    const fs::path fixture = fs::path(JASPER_TEST_REPO_DIR) / "engine/tests/core/titleart/fixtures/v1/every-layer-type.titleart";
    std::string text = readAll(fixture);
    ASSERT_FALSE(text.empty()) << fixture;
    fs::create_directories(headless().dir / "assets" / "designer");
    const fs::path file = headless().dir / "assets/designer/every.titleart";
    std::ofstream(file, std::ios::binary) << text;
    TitleDesignerState state(project());
    ASSERT_TRUE(state.open(file, nullptr));
    Host host(state);
    PropertyIndex index;
    collectIndex(state, &host, index, true);
    size_t numeric = 0;
    for (const RowAudit& row : lastCollectAudit()) {
        if (!row.numeric) continue;
        numeric++;
        EXPECT_TRUE(row.hasDefault) << row.label << " (" << row.fieldPath << ") has no default";
    }
    EXPECT_GT(numeric, 50u) << "the pages draw the numeric rows";
}

// R84 / AC2: the per-user settings and the per-document view state round-trip; the document is
// never touched by them
TEST_F(TitleDesignerStateTest, SettingsAndWorkspaceRoundTrip) {
    TitleDesignerSettings settings;
    settings.layersWidth = 300.0f;
    settings.libraryCollapsed = true;
    settings.sections["rim/mat_bevel"] = false;
    settings.previewBudgetMs = 6.0f;
    settings.budgetSet = true;
    settings.quality = Quality::Full;
    settings.qualitySet = true;
    const TitleDesignerSettings back = TitleDesignerSettings::decode(settings.encode());
    EXPECT_EQ(back.layersWidth, 300.0f);
    EXPECT_TRUE(back.libraryCollapsed);
    EXPECT_FALSE(back.sections.at("rim/mat_bevel"));
    EXPECT_EQ(back.previewBudgetMs, 6.0f);
    EXPECT_EQ(back.quality, Quality::Full);
    EXPECT_EQ(TitleDesignerSettings::decode(YAML::Node()).layersWidth, 260.0f) << "a missing block takes its defaults";

    TitleDesignerWorkspace workspace;
    workspace.load(headless().dir);
    workspace.touch("designer/a.titleart");
    workspace.touch("designer/b.titleart");
    DocumentView& view = workspace.view("0123456789abcdef0123456789abcdef");
    view.guides.push_back({true, 120.0});
    view.device.profile = "iphone-15";
    view.zoom = 2.0f;
    view.fit = false;
    view.backdrop.kind = "black";
    ASSERT_TRUE(workspace.save());
    TitleDesignerWorkspace again;
    again.load(headless().dir);
    ASSERT_EQ(again.recent().size(), 2u);
    EXPECT_EQ(again.recent()[0], "designer/b.titleart");
    const DocumentView& loaded = again.view("0123456789abcdef0123456789abcdef");
    ASSERT_EQ(loaded.guides.size(), 1u);
    EXPECT_EQ(loaded.guides[0].position, 120.0);
    EXPECT_EQ(loaded.device.profile, "iphone-15");
    EXPECT_FALSE(loaded.fit);
    EXPECT_EQ(loaded.zoom, 2.0f);
    EXPECT_EQ(loaded.backdrop.kind, "black");
}
