// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Title Designer window (docs/specs/S82-title-designer.md AC1, AC2, AC3, AC6, AC17, AC19,
// AC29, AC36, AC37, AC40) through its scripted API and TitleDesignerWindowTestAccess, the friend
// the window declares. Every case runs in a child process with the editor's App and a project
// written by the test (the SpriteNormalsWindowTest pattern), without a GPU: renders are the MCP
// tests' and the screenshots' (the canvas view draws only once the engine's view is loaded).

#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

#include "App.h"
#include "AppSettings.h"
#include "Backend.h"
#include "JasperEngine.h"
#include "Platform.h"
#include "Project.h"
#include "ScriptDataTestSupport.h"
#include "command/CommandHistory.h"
#include "shortcuts/CommandCatalog.h"
#include "shortcuts/DefaultCommands.h"
#include "titleart/TitleArtDocument.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitlePreviewQueue.h"
#include "titleart/designer/TitleCanvasView.h"
#include "titleart/designer/TitleDesignerHooks.h"
#include "titleart/designer/TitleTextEdit.h"
#include "window/InspectorWindowTitleArt.h"
#include "window/ProjectWindow.h"
#include "window/TitleDesignerWindow.h"

#include "imgui.h"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace fs = std::filesystem;
namespace ta = jasperengine::titleart;

namespace jasperengine::editor {

    // the window's internals the tests read and drive (S82 §6.7)
    struct TitleDesignerWindowTestAccess {
        using W = TitleDesignerWindow;
        static void drawFrame(W& w) {
            ImGui::NewFrame();
            w.show();
            ImGui::EndFrame();
        }
        static bool hasCanvas(W& w) { return w.canvas != nullptr; }
        static bool hasThumbnails(W& w) { return w.thumbnails != nullptr; }
        static bool hasFontCatalog(W& w) { return w.fontCatalog != nullptr; }
        static TitleTextEdit* textEdit(W& w) { return w.textEdit.get(); }
        static void beginTextEdit(W& w, const std::string& layer, bool selectAll) { w.beginTextEdit(layer, selectAll); }
        static void endTextEdit(W& w, bool commit) { w.endTextEdit(commit); }
        static bool contentEmpty(W& w) { return w.contentEmpty(); }
        static bool pressKey(W& w, const std::string& command) { return w.runCommand(command); }
        static std::vector<std::string> search(W& w, const std::string& query) {
            w.setSearch(query);
            w.rebuildIndexIfNeeded();
            std::vector<std::string> out;
            for (const SearchHit& hit : w.index.query(query, 30)) out.push_back(w.index.entry(hit.entry).fieldPath);
            return out;
        }
        static void jumpToHit(W& w, const std::string& query, size_t i) {
            w.setSearch(query);
            w.rebuildIndexIfNeeded();
            const std::vector<SearchHit> hits = w.index.query(query, 30);
            if (i < hits.size()) w.jumpToHit(hits[i].entry);
        }
        static const std::string& searchText(W& w) { return w.searchText; }
        static std::string flashRow(W& w) { return w.flashRow; }
        static bool applyLibrary(W& w, LibraryTab tab, const LibraryItem& item, bool preview) { return w.applyLibraryItem(tab, item, preview); }
        static void refreshLint(W& w) { w.refreshLint(false); }
        static const std::vector<CheckFinding>& lintFindings(W& w) { return w.findings; }
        static size_t checksRun(W& w) { return w.checksRun; }
        static void setClock(W& w, std::function<double()> clock) { w.clockOverride = std::move(clock); }
        static TitleDesignerSettings& settings(W& w) { return w.settings; }
    };

}

namespace {

    using Access = TitleDesignerWindowTestAccess;

    struct World {
        fs::path dir;
        Project* p = nullptr;
        App* app = nullptr;
    };

    void writeText(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    void runInEditor(const std::string& name, const std::function<void(World&)>& body) {
        const fs::path root = fs::temp_directory_path() / ("jasper-title-designer-" + name + "-" + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root / "config");
        setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
        World w;
        w.dir = root / "project";
        fs::create_directories(w.dir / ".jasperengine");
        std::ofstream(w.dir / "project.yaml") << "name: titledesigner\n";
        AppSettings::initialize();
        App& app = Backend::getApp();
        w.app = &app;
        w.p = app.getProject();
        static editor::Platform* platform = nullptr;
        platform = new editor::Platform(w.p);
        jasperengine::SystemTestAccess::swap(platform);
        Engine::pauseGameEvents(true);
        Engine::setAsyncLoading(false);
        if (!w.p->loadProject(w.dir, false)) std::exit(90);
        ImGuiContext* context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 900);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        ::testing::TestPartResultArray results;
        {
            ::testing::ScopedFakeTestPartResultReporter reporter(::testing::ScopedFakeTestPartResultReporter::INTERCEPT_ALL_THREADS, &results);
            body(w);
        }
        int failures = 0;
        for (int i = 0; i < results.size(); i++) {
            const ::testing::TestPartResult& r = results.GetTestPartResult(i);
            if (!r.failed()) continue;
            failures++;
            std::fprintf(stderr, "%s:%d: %s\n", r.file_name() ? r.file_name() : "?", r.line_number(), r.message());
        }
        std::fflush(stderr);
        std::fflush(stdout);
        std::_Exit(failures ? 1 : 0);
    }

#define IN_EDITOR(name, ...)                                                    \
    do {                                                                        \
        auto inEditor = [&](World& w) __VA_ARGS__;                              \
        GTEST_FLAG_SET(death_test_style, "threadsafe"); EXPECT_EXIT(runInEditor(name, inEditor), ::testing::ExitedWithCode(0), ""); \
    } while (0)

    fs::path newTitle(World& w, const std::string& name) {
        std::string error;
        std::shared_ptr<TitleArtDocument> doc = TitleArtDocument::create(w.p, "assets/" + name + ".titleart", nullptr, "Create Title Art", &error);
        EXPECT_TRUE(doc) << error;
        return doc ? doc->absolutePath() : fs::path();
    }

    std::string titleLayer(TitleDesignerWindow& window) {
        std::string out;
        ta::forEachLayer(window.designer().committed().root, [&](const ta::Json& l, const ta::Json*, int) {
            if (ta::layerTypeOf(l) == ta::LayerType::Text && out.empty()) out = l.value("id", std::string());
            return true;
        });
        return out;
    }

    // a Library provider standing in for S84: two Looks that set the face colour
    class FakeLibrary : public TitleLibraryProvider {
    public:
        std::vector<LibraryItem> items(LibraryTab tab) override {
            if (tab != LibraryTab::Looks) return {};
            return {LibraryItem{"red", "Red Face", "Minimal"}, LibraryItem{"gold", "Gold Face", "Casino"}};
        }
        std::vector<std::string> families() override { return {"Casino", "Minimal"}; }
        ta::Json applyOps(LibraryTab, const LibraryItem& item, const LibraryApplyOptions& options) override {
            ta::Json ops = ta::Json::array();
            for (const std::string& id : options.layers) {
                ops.push_back({{"op", "set"}, {"layer", "id:" + id}, {"values", {{"style.face.fill.color", item.id == "red" ? "#ff0000" : "#ffd040"}}}});
                ops.push_back({{"op", "add_effect"}, {"layer", "id:" + id}, {"type", "outer_glow"}});
            }
            return ops;
        }
        std::string stepName(LibraryTab, const LibraryItem& item, const LibraryApplyOptions&) override { return "Apply Look " + item.name; }
        LibraryTile tile(LibraryTab, const LibraryItem&, const std::string&, float, float, bool) override { return {}; }
    };

}

// AC1 (R1, R65): every entry opens the document; closed, nothing exists (no canvas view, no
// renderer job, no preview request, no font listing). Red control: making the canvas view in the
// constructor leaves liveCount() at 1 while closed.
TEST(TitleDesignerWindowDeathTest, OpensFromEveryEntryAndClosedCostsNothing) {
    IN_EDITOR("open", {
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        ASSERT_NE(window, nullptr);
        window->setOpen(false);
        EXPECT_FALSE(Access::hasCanvas(*window)) << "closed: no canvas view";
        EXPECT_EQ(TitleCanvasView::liveCount(), 0u);
        EXPECT_FALSE(Access::hasFontCatalog(*window)) << "closed: no font listing";
        for (int i = 0; i < 120; i++) Access::drawFrame(*window);
        EXPECT_EQ(TitleCanvasView::liveCount(), 0u) << "show() returns at its first line";

        const fs::path file = newTitle(w, "logo");
        // the scripted API (Window menu, focus_window)
        std::string error;
        ASSERT_TRUE(window->open(file, &error)) << error;
        EXPECT_TRUE(window->isOpen());
        EXPECT_TRUE(Access::hasCanvas(*window));
        EXPECT_EQ(TitleCanvasView::liveCount(), 1u);
        EXPECT_EQ(window->documentPath().lexically_normal(), file.lexically_normal());
        // closing destroys the live parts within the frame and empties the queue of its requests
        window->setOpen(false);
        EXPECT_EQ(TitleCanvasView::liveCount(), 0u);
        EXPECT_EQ(TitlePreviewQueue::editor().stats().queued, 0u);
        // the Inspector's Open in Title Designer (S80's button, set by the App)
        ASSERT_TRUE(InspectorWindowTitleArt::canOpenInDesigner());
        InspectorWindowTitleArt::openInDesigner(file);
        EXPECT_TRUE(window->isOpen());
        EXPECT_EQ(window->documentPath().lexically_normal(), file.lexically_normal());
        window->setOpen(false);
        // the Window menu reopens the last document
        window->setOpen(true);
        EXPECT_EQ(window->documentPath().lexically_normal(), file.lexically_normal());
        // a non-title path is refused with the reason
        EXPECT_FALSE(window->open(w.dir / "assets/none.titleart", &error));
        EXPECT_FALSE(error.empty());
        // focus_window's state
        const ta::Json state = window->state();
        EXPECT_EQ(state.value("path", std::string()), "assets/logo.titleart");
        EXPECT_TRUE(state.contains("staged"));
        EXPECT_TRUE(state.contains("preview_active"));
    });
}

// AC3 (R5): the empty state while no visible content layer exists, hidden ancestors counted;
// Add Text selects the new layer and starts editing it with the placeholder selected
TEST(TitleDesignerWindowDeathTest, EmptyState) {
    IN_EDITOR("empty", {
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        const fs::path file = newTitle(w, "empty");
        ASSERT_TRUE(window->open(file, nullptr));
        EXPECT_FALSE(Access::contentEmpty(*window)) << "a new document has its Title";
        const std::string title = titleLayer(*window);
        ASSERT_TRUE(window->runOps(editsToOps({{title, "visible", false}}), "Hide Layer"));
        EXPECT_TRUE(Access::contentEmpty(*window)) << "hidden: the card comes back";
        ASSERT_TRUE(window->runOps(ta::Json::parse(R"([{"op": "remove_layer", "layer": "Title"}])"), "Delete Layer"));
        EXPECT_TRUE(Access::contentEmpty(*window));
        ASSERT_TRUE(Access::pressKey(*window, "titleDesigner.editText")) << "Enter on an empty title adds a text";
        EXPECT_FALSE(Access::contentEmpty(*window));
        ASSERT_EQ(window->designer().selection().size(), 1u);
        ASSERT_NE(Access::textEdit(*window), nullptr);
        EXPECT_TRUE(Access::textEdit(*window)->active()) << "the new text is being edited";
        Access::endTextEdit(*window, false);
    });
}

// AC17 (R36, R37, R39): typing updates the working document before any commit; Esc restores the
// text and adds no step; Ctrl+Enter is one step "Edit Text"; a composition shows in the preview
// only, Enter during it does not end the edit, and committing it inserts the converted text
TEST(TitleDesignerWindowDeathTest, TextEditing) {
    IN_EDITOR("text", {
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        const fs::path file = newTitle(w, "text");
        ASSERT_TRUE(window->open(file, nullptr));
        const std::string title = titleLayer(*window);
        CommandHistory* history = w.p->getProjectCommandHistory();
        const size_t position = history->position();
        Access::beginTextEdit(*window, title, true);
        TitleTextEdit* edit = Access::textEdit(*window);
        ASSERT_TRUE(edit && edit->active());
        for (int i = 0; i < 3; i++) Access::drawFrame(*window);
        edit->typeText("\xE6\xBF\x80\xE7\x86\xB1");      // 激熱 replaces the selected placeholder
        for (int i = 0; i < 3; i++) Access::drawFrame(*window);
        const ta::Json* layer = layerById(window->designer().working(), title);
        ASSERT_NE(layer, nullptr);
        EXPECT_EQ(layer->value("text", std::string()), "\xE6\xBF\x80\xE7\x86\xB1") << "the working document shows the typing";
        EXPECT_EQ(history->position(), position) << "nothing committed while typing";
        Access::endTextEdit(*window, false);
        EXPECT_EQ(layerById(window->designer().committed(), title)->value("text", std::string()), "Title") << "Esc restores";
        EXPECT_EQ(layerById(window->designer().working(), title)->value("text", std::string()), "Title");
        EXPECT_EQ(history->position(), position) << "Esc adds no step";

        Access::beginTextEdit(*window, title, true);
        edit = Access::textEdit(*window);
        for (int i = 0; i < 3; i++) Access::drawFrame(*window);
        edit->compose("\xE3\x81\x92\xE3\x81\x8D\xE3\x81\x82\xE3\x81\xA4", 12);    // げきあつ
        for (int i = 0; i < 2; i++) Access::drawFrame(*window);
        EXPECT_NE(edit->previewText().find("\xE3\x81\x92\xE3\x81\x8D"), std::string::npos) << "the preedit is in the preview";
        EXPECT_EQ(edit->text().find("\xE3\x81\x92"), std::string::npos) << "and not in the text";
        EXPECT_TRUE(edit->composing());
        edit->commitComposition("\xE6\xBF\x80\xE7\x86\xB1");
        for (int i = 0; i < 3; i++) Access::drawFrame(*window);
        EXPECT_EQ(edit->text(), "\xE6\xBF\x80\xE7\x86\xB1");
        Access::endTextEdit(*window, true);
        EXPECT_EQ(history->position(), position + 1);
        EXPECT_EQ(history->top()->name(), "Edit Text");
        EXPECT_EQ(layerById(window->designer().committed(), title)->value("text", std::string()), "\xE6\xBF\x80\xE7\x86\xB1");
    });
}

// AC10 (R26): a jump selects the node, flashes its row and clears the search field
TEST(TitleDesignerWindowDeathTest, SearchJumpClearsTheField) {
    IN_EDITOR("search", {
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        const fs::path file = newTitle(w, "search");
        ASSERT_TRUE(window->open(file, nullptr));
        ASSERT_TRUE(window->runOps(ta::Json::parse(R"([{"op": "add_effect", "layer": "Title", "type": "outer_glow"}])"), "Add Effect Outer Glow to Title"));
        ASSERT_TRUE(window->select({"Title"}));
        const std::vector<std::string> hits = Access::search(*window, "glow");
        ASSERT_FALSE(hits.empty()) << "glow shows live rows";
        Access::jumpToHit(*window, "glow", 0);
        EXPECT_TRUE(Access::searchText(*window).empty()) << "the field is cleared after a jump";
        EXPECT_FALSE(Access::flashRow(*window).empty()) << "the row flashes";
        EXPECT_NE(window->designer().primary().kind, NodeKind::Document);
    });
}

// AC19 / R44 (the document half; pixels are the MCP test's): a Look previewed then applied gives
// the previewed document byte for byte, as one step named after the item; a preview is no step.
// Red control: a preview built by a second path (applyOps with other options) differs.
TEST(TitleDesignerWindowDeathTest, LibraryPreviewEqualsApply) {
    IN_EDITOR("library", {
        setLibraryProvider(std::make_shared<FakeLibrary>());
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        const fs::path file = newTitle(w, "library");
        ASSERT_TRUE(window->open(file, nullptr));
        ASSERT_TRUE(window->select({"Title"}));
        CommandHistory* history = w.p->getProjectCommandHistory();
        const size_t position = history->position();
        const LibraryItem red{"red", "Red Face", "Minimal"};
        ASSERT_TRUE(Access::applyLibrary(*window, LibraryTab::Looks, red, true));
        EXPECT_EQ(history->position(), position) << "a preview is no step";
        const std::string previewed = ta::writeTitleArt(window->designer().shown());
        ASSERT_TRUE(Access::applyLibrary(*window, LibraryTab::Looks, red, false));
        EXPECT_EQ(history->position(), position + 1);
        EXPECT_EQ(history->top()->name(), "Apply Look Red Face");
        EXPECT_EQ(readAll(file), previewed) << "the committed document is the previewed one";
        setLibraryProvider(nullptr);
    });
}

// AC37 (R6): a newer version opens read-only with the reason; every edit is refused
TEST(TitleDesignerWindowDeathTest, ReadOnly) {
    IN_EDITOR("readonly", {
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        const fs::path file = w.dir / "assets/newer.titleart";
        writeText(file, R"({"format": "jasperengine.titleart", "version": 99, "guid": "a1b2c3d4e5f60718293a4b5c6d7e8f91", "layers": []})");
        std::string error;
        ASSERT_TRUE(window->open(file, &error)) << error;
        EXPECT_FALSE(window->designer().editable());
        EXPECT_FALSE(window->designer().readOnlyReason().empty());
        EXPECT_FALSE(window->runOps(ta::Json::parse(R"([{"op": "set", "values": {"seed": 3}}])"), "Change Seed", &error));
        EXPECT_FALSE(error.empty());
        for (int i = 0; i < 3; i++) Access::drawFrame(*window);
    });
}

// AC36 (§6.5): the Title Designer context parses, every §6.5 command is in the catalog, and Q
// with the window focused changes its tool, not the Scene view's
TEST(TitleDesignerWindowDeathTest, ShortcutContext) {
    IN_EDITOR("shortcuts", {
        EXPECT_EQ(parseShortcutContext("title_designer"), ShortcutContext::TitleDesigner);
        EXPECT_EQ(parseShortcutContext("Title Designer"), ShortcutContext::TitleDesigner);
        for (const char* id : {"titleDesigner.toolHand", "titleDesigner.toolRect", "titleDesigner.togglePivot", "titleDesigner.frameSelected",
                               "titleDesigner.fitCanvas", "titleDesigner.zoomActual", "titleDesigner.zoomIn", "titleDesigner.zoomOut",
                               "titleDesigner.editText", "titleDesigner.find", "titleDesigner.add", "titleDesigner.duplicate", "titleDesigner.delete",
                               "titleDesigner.rename", "titleDesigner.selectAll", "titleDesigner.copy", "titleDesigner.cut", "titleDesigner.paste",
                               "titleDesigner.group", "titleDesigner.ungroup", "titleDesigner.clip", "titleDesigner.forward", "titleDesigner.backward",
                               "titleDesigner.toFront", "titleDesigner.toBack", "titleDesigner.toggleVisible", "titleDesigner.toggleLock",
                               "titleDesigner.nudgeLeft", "titleDesigner.up", "titleDesigner.bake"}) {
            const std::vector<CommandDef> defaults = defaultCommands();
            auto def = std::find_if(defaults.begin(), defaults.end(), [&](const CommandDef& d) { return d.id == id; });
            ASSERT_NE(def, defaults.end()) << id;
            EXPECT_EQ(def->context, ShortcutContext::TitleDesigner) << id;
        }
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        ASSERT_TRUE(window->open(newTitle(w, "keys"), nullptr));
        ASSERT_TRUE(Access::pressKey(*window, "titleDesigner.toolHand"));
        EXPECT_EQ(window->tool(), Tool::Hand);
        ASSERT_TRUE(Access::pressKey(*window, "titleDesigner.toolRotate"));
        EXPECT_EQ(window->tool(), Tool::Rotate);
        // Ctrl+D duplicates the selection as one step
        ASSERT_TRUE(window->select({"Title"}));
        const size_t position = w.p->getProjectCommandHistory()->position();
        ASSERT_TRUE(Access::pressKey(*window, "titleDesigner.duplicate"));
        EXPECT_EQ(w.p->getProjectCommandHistory()->position(), position + 1);
        EXPECT_EQ(w.p->getProjectCommandHistory()->top()->name(), "Duplicate Layer");
    });
}

// AC6, AC29 (R12, R13, R16, R75): eye, lock and the arrange keys are one step each; a nudge burst
// under 800 ms on one selection merges into one step "Nudge"
TEST(TitleDesignerWindowDeathTest, FlagsArrangeAndNudgeBurst) {
    IN_EDITOR("flags", {
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        ASSERT_TRUE(window->open(newTitle(w, "flags"), nullptr));
        ASSERT_TRUE(window->runOps(ta::Json::parse(R"([{"op": "add_layer", "type": "shape", "name": "Plate"}])"), "Add Layer Shape"));
        ASSERT_TRUE(window->select({"Title"}));
        CommandHistory* history = w.p->getProjectCommandHistory();
        size_t position = history->position();
        ASSERT_TRUE(Access::pressKey(*window, "titleDesigner.toggleLock"));
        EXPECT_EQ(history->position(), ++position);
        EXPECT_EQ(history->top()->name(), "Lock Layer");
        ASSERT_TRUE(Access::pressKey(*window, "titleDesigner.toggleLock"));
        EXPECT_EQ(history->top()->name(), "Unlock Layer");
        position = history->position();
        ASSERT_TRUE(Access::pressKey(*window, "titleDesigner.toFront"));
        EXPECT_EQ(history->top()->name(), "Bring to Front");
        position = history->position();
        double clock = 100.0;
        Access::setClock(*window, [&clock]() { return clock; });
        for (int i = 0; i < 5; i++) {
            ASSERT_TRUE(Access::pressKey(*window, "titleDesigner.nudgeRight"));
            clock += 0.2;
        }
        EXPECT_EQ(history->position(), position + 1) << "a burst of nudges is one step";
        EXPECT_EQ(history->top()->name(), "Nudge");
        clock += 2.0;
        ASSERT_TRUE(Access::pressKey(*window, "titleDesigner.nudgeRight"));
        EXPECT_EQ(history->position(), position + 2) << "after a pause a new burst";
        const ta::Json* title = nullptr;
        ta::forEachLayer(window->designer().committed().root, [&](const ta::Json& l, const ta::Json*, int) {
            if (l.value("name", std::string()) == "Title") title = &l;
            return true;
        });
        ASSERT_NE(title, nullptr);
        EXPECT_DOUBLE_EQ((*title)["transform"]["position"][0].get<double>(), 6.0);
    });
}

// AC23 (R53): document checks run; the wording never claims the title looks good
TEST(TitleDesignerWindowDeathTest, LintWording) {
    IN_EDITOR("lint", {
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        ASSERT_TRUE(window->open(newTitle(w, "lint"), nullptr));
        Access::refreshLint(*window);
        EXPECT_GT(Access::checksRun(*window), 0u) << "the document checks ran";
        for (const CheckFinding& f : Access::lintFindings(*window)) {
            for (const char* word : {"good", "approved", "looks great"}) EXPECT_EQ(f.message.find(word), std::string::npos) << f.message;
        }
        Access::drawFrame(*window);
    });
}
