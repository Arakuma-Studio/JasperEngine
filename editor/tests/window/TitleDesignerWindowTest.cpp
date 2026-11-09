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
#include "window/EditorModal.h"
#include "window/InspectorWindowTitleArt.h"
#include "window/ProjectWindow.h"
#include "window/TitleDesignerWindow.h"

#include "imgui.h"

#include <unistd.h>

#include <algorithm>
#include <cmath>
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
            EditorModal::endFrame();
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
        static void exposeField(W& w, const FieldRef& target, const std::string& label) { w.exposeField(target, label); }
        static TitleLibraryProvider* library(W& w) { return w.library(); }
        static TitleLetterGizmo& letters(W& w) { return w.letterGizmo; }
        static void commitLetters(W& w, const ta::Json& values, const std::string& name) { w.commitLetters(values, name); }
        static void requestFocus(W& w) { w.focusRequested = true; }
        static void setIndexUnitsPerFrame(W& w, int units) { w.indexUnitsPerFrame = units; }
        static size_t servedHits(W& w, const std::string& query) { return w.searchIndex().query(query, 0).size(); }
        static std::vector<std::string> indexRows(W& w) {
            std::vector<std::string> out;
            for (size_t i = 0; i < w.index.size(); i++) out.push_back(w.index.entry(i).rowId + "|" + w.index.entry(i).label);
            return out;
        }
        static void setSystemCjk(W& w, std::function<int()> found) { w.systemCjkOverride = std::move(found); }
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

// S87 R34: an SVG dropped on the Designer comes in through import_svg: from outside the assets it
// is copied beside the document, from inside it is referenced where it is.
TEST(TitleDesignerWindowDeathTest, DroppedSvgComesInThroughImportSvg) {
    IN_EDITOR("dropsvg", {
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        const fs::path file = newTitle(w, "dropsvg");
        ASSERT_TRUE(window->open(file, nullptr));
        const char* svg = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><rect width="10" height="10" fill="#ff0000"/></svg>)";
        const fs::path outside = fs::temp_directory_path() / ("je_dropped_badge_" + std::to_string(::getpid()) + ".svg");
        std::ofstream(outside) << svg;
        const auto vectors = [&]() {
            int count = 0;
            ta::forEachLayer(window->designer().committed().root, [&](const ta::Json& l, const ta::Json*, int) {
                if (ta::layerTypeOf(l) == ta::LayerType::Vector) count++;
                return true;
            });
            return count;
        };
        std::string error;
        ASSERT_TRUE(window->dropFiles({outside.string()}, std::string(), &error)) << error;
        EXPECT_EQ(vectors(), 1);
        EXPECT_TRUE(fs::exists(file.parent_path() / "dropsvg_assets" / outside.filename())) << "copied beside the document";
        const fs::path inside = file.parent_path() / "inside.svg";
        std::ofstream(inside) << svg;
        ASSERT_TRUE(window->dropFiles({inside.string()}, std::string(), &error)) << error;
        EXPECT_EQ(vectors(), 2);
        EXPECT_FALSE(fs::exists(file.parent_path() / "dropsvg_assets" / "inside.svg")) << "a file in the assets stays where it is";
        fs::remove(outside);
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

// R21, R43: the window's dialogs are EditorModals, so an agent sees and answers Expose as Parameter as a
// person does (editor_status, respond_dialog). Red control: the raw BeginPopupModal it had is
// reported without its question or its Name field, so an agent could not fill it in.
TEST(TitleDesignerWindowDeathTest, ExposeDialogIsAnEditorModal) {
    IN_EDITOR("expose", {
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        const fs::path file = newTitle(w, "expose");
        ASSERT_TRUE(window->open(file, nullptr));
        const std::string layer = titleLayer(*window);
        ASSERT_FALSE(layer.empty());
        const size_t exposedBefore = window->designer().committed().root.value("exposed", ta::Json::array()).size();
        Access::exposeField(*window, FieldRef{layer, "text"}, "Text");
        for (int i = 0; i < 3; i++) Access::drawFrame(*window);
        EditorDialogInfo info;
        ASSERT_TRUE(EditorModal::topModal(info));
        EXPECT_NE(info.title.find("Expose as Parameter"), std::string::npos) << info.title;
        EXPECT_NE(info.message.find("Text"), std::string::npos) << info.message;
        ASSERT_EQ(info.fields.size(), 1u);
        EXPECT_EQ(info.fields[0].name, "name");
        EXPECT_EQ(info.fields[0].value.text, "text") << "the label is the suggested name";
        std::string error;
        ASSERT_TRUE(EditorModal::answer("ok", {{"name", EditorDialogValue::ofText("headline")}}, error)) << error;
        for (int i = 0; i < 3; i++) Access::drawFrame(*window);
        EXPECT_FALSE(EditorModal::topModal(info)) << "Expose closes it";
        const ta::Json& exposed = window->designer().committed().root.value("exposed", ta::Json::array());
        ASSERT_EQ(exposed.size(), exposedBefore + 1);
        EXPECT_EQ(exposed.back().value("name", std::string()), "headline");
        EXPECT_EQ(w.p->getProjectCommandHistory()->top()->name(), "Expose Parameter headline");
    });
}

// R40, R41, D18: with no Library provider registered the strip is in view with the starter Looks,
// and a Look's hover preview is the document its click commits. Red control: the window without
// its starter Library (library() returning only a registered provider) shows no strip and has
// nothing to apply.
TEST(TitleDesignerWindowDeathTest, StarterLibraryInViewByDefault) {
    IN_EDITOR("starter", {
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        const fs::path file = newTitle(w, "starter");
        ASSERT_TRUE(window->open(file, nullptr));
        for (int i = 0; i < 3; i++) Access::drawFrame(*window);
        EXPECT_EQ(window->state()["library"].value("provider", std::string()), "starter");
        EXPECT_TRUE(window->state()["library"].value("shown", false)) << "the strip is in view by default";
        TitleLibraryProvider* library = Access::library(*window);
        ASSERT_NE(library, nullptr);
        const std::vector<LibraryItem> looks = library->items(LibraryTab::Looks);
        ASSERT_FALSE(looks.empty());
        EXPECT_FALSE(library->items(LibraryTab::ColorSchemes).empty());
        auto gold = std::find_if(looks.begin(), looks.end(), [](const LibraryItem& i) { return i.id == "starter_gold"; });
        ASSERT_NE(gold, looks.end());
        ASSERT_TRUE(window->select({"Title"}));
        CommandHistory* history = w.p->getProjectCommandHistory();
        const size_t position = history->position();
        ASSERT_TRUE(Access::applyLibrary(*window, LibraryTab::Looks, *gold, true));
        EXPECT_EQ(history->position(), position) << "a preview is no step";
        const std::string previewed = ta::writeTitleArt(window->designer().shown());
        ASSERT_TRUE(Access::applyLibrary(*window, LibraryTab::Looks, *gold, false));
        EXPECT_EQ(history->top()->name(), "Apply Look Casino Gold");
        EXPECT_EQ(readAll(file), previewed) << "the committed document is the previewed one";
        const ta::Json* layer = layerById(window->designer().committed(), titleLayer(*window));
        ASSERT_NE(layer, nullptr);
        EXPECT_EQ((*layer)["style"]["rims"].size(), 3u);
        const ta::Json effects = layer->value("effects", ta::Json::array());
        EXPECT_TRUE(std::any_of(effects.begin(), effects.end(), [](const ta::Json& e) { return e.value("type", std::string()) == "outer_glow"; }));
        // applying it again replaces, never stacks
        ASSERT_TRUE(Access::applyLibrary(*window, LibraryTab::Looks, *gold, false));
        layer = layerById(window->designer().committed(), titleLayer(*window));
        EXPECT_EQ((*layer)["style"]["rims"].size(), 3u);
        EXPECT_EQ((*layer)["effects"].size(), 2u);
    });
}

// AC18 (R38): selecting characters 2-3 while editing gives each its gizmo; a quarter turn of one
// knob writes chars.1.rotation and chars.2.rotation in one step and the edit goes on. Red control:
// the gizmo given no characters (the selection ignored) fails the first check.
TEST(TitleDesignerWindowDeathTest, LettersGizmo) {
    IN_EDITOR("letters", {
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        const fs::path file = newTitle(w, "letters");
        ASSERT_TRUE(window->open(file, nullptr));
        const std::string layer = titleLayer(*window);
        ASSERT_FALSE(layer.empty());
        Access::beginTextEdit(*window, layer, false);
        ASSERT_NE(Access::textEdit(*window), nullptr);
        Access::textEdit(*window)->select(1, 3);
        for (int i = 0; i < 3; i++) Access::drawFrame(*window);
        TitleLetterGizmo& gizmo = Access::letters(*window);
        ASSERT_EQ(gizmo.selected().size(), 2u) << "one gizmo per selected character";
        EXPECT_EQ(gizmo.selected()[0].index, 1);
        EXPECT_EQ(gizmo.selected()[1].index, 2);
        const ImVec2 c = gizmo.centreOf(0), k = gizmo.knobOf(0);
        const float r = std::sqrt((k.x - c.x) * (k.x - c.x) + (k.y - c.y) * (k.y - c.y));
        gizmo.begin(gizmo.hitTest(k), k);
        ASSERT_TRUE(gizmo.dragging());
        const ta::Json values = gizmo.values(ImVec2(c.x + r, c.y), false);
        CommandHistory* history = w.p->getProjectCommandHistory();
        const size_t position = history->position();
        Access::commitLetters(*window, values, gizmo.stepName());
        gizmo.end();
        EXPECT_EQ(history->position(), position + 1) << "one step";
        EXPECT_EQ(history->top()->name(), "Rotate Letters");
        const ta::Json* committed = layerById(window->designer().committed(), layer);
        ASSERT_NE(committed, nullptr);
        const ta::Json chars = committed->value("chars", ta::Json::object());
        ASSERT_TRUE(chars.contains("1") && chars.contains("2")) << chars.dump();
        EXPECT_NEAR(chars["1"].value("rotation", 0.0), 90.0, 1e-3);
        EXPECT_NEAR(chars["2"].value("rotation", 0.0), 90.0, 1e-3);
        EXPECT_FALSE(chars.contains("0") && chars["0"].value("rotation", 0.0) != 0.0) << "unselected characters keep theirs";
        EXPECT_TRUE(window->state().value("text_editing", false)) << "the edit goes on";
    });
}

// R76: Tab moves the keyboard through toolbar, Layers, canvas, Properties and Library, Shift+Tab
// back. Red control: the window not taking Tab leaves the focus on the toolbar.
TEST(TitleDesignerWindowDeathTest, TabCyclesRegions) {
    IN_EDITOR("tab", {
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        const fs::path file = newTitle(w, "tab");
        ASSERT_TRUE(window->open(file, nullptr));
        Access::requestFocus(*window);
        for (int i = 0; i < 3; i++) Access::drawFrame(*window);
        auto focus = [&]() { return window->state().value("keyboard_focus", std::string()); };
        auto tab = [&](bool back) {
            ImGuiIO& io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Shift, back);
            io.AddKeyEvent(ImGuiKey_Tab, true);
            Access::drawFrame(*window);
            io.AddKeyEvent(ImGuiKey_Tab, false);
            io.AddKeyEvent(ImGuiMod_Shift, false);
            Access::drawFrame(*window);
            return focus();
        };
        EXPECT_EQ(focus(), "toolbar");
        EXPECT_EQ(tab(false), "layers");
        EXPECT_EQ(tab(false), "canvas");
        EXPECT_EQ(tab(false), "properties");
        EXPECT_EQ(tab(false), "library");
        EXPECT_EQ(tab(false), "toolbar");
        EXPECT_EQ(tab(true), "library");
    });
}

// R69 (AC31's load-insensitive half): an idle frame makes no whole-document pass (hash, bake
// status, layer bounds, index, lint), with 40 layers as with 256, Layers or Properties shown, so
// its cost does not grow with the document. Red control: the toolbar's bake status made every
// frame (it hashes the document and reads every output's recipe) adds a pass per frame.
TEST(TitleDesignerWindowDeathTest, IdleFramesMakeNoDocumentPasses) {
    IN_EDITOR("passes", {
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        for (int count : {40, 256}) {
            const fs::path file = newTitle(w, "passes" + std::to_string(count));
            ASSERT_TRUE(window->open(file, nullptr));
            ta::Json ops = ta::Json::array();
            for (int i = 1; i < count; i++) {
                ta::Json layer = i % 2 ? ta::Json{{"type", "text"}, {"name", "Text " + std::to_string(i)}, {"text", "L" + std::to_string(i)},
                                                  {"effects", ta::Json::array({{{"type", "outer_glow"}}})}}
                                       : ta::Json{{"type", "shape"}, {"name", "Shape " + std::to_string(i)}};
                ops.push_back({{"op", "add_layer"}, {"from", layer}});
            }
            std::string error;
            ASSERT_TRUE(window->runOps(ops, "Add Layers", &error)) << error;
            ASSERT_TRUE(window->select({"Text 1"}));
            for (const char* panel : {"layers", "properties"}) {
                ASSERT_TRUE(window->setPanel(panel, &error)) << error;
                // past the lint's 300 ms debounce and the first layout of every row
                for (int i = 0; i < 40; i++) Access::drawFrame(*window);
                const uint64_t before = window->state()["timings"].value("document_passes", (uint64_t)0);
                for (int i = 0; i < 30; i++) Access::drawFrame(*window);
                const uint64_t after = window->state()["timings"].value("document_passes", (uint64_t)0);
                EXPECT_EQ(after - before, 0u) << count << " layers, " << panel << " shown: passes on idle frames";
            }
        }
    });
}

// R69: the search index builds across frames, never more than the frame's share of pages (here
// one, counted, not timed), the previous index answering a search meanwhile, and the finished
// index is the one a single full pass makes. Red control: the whole pass in one frame (the per-frame
// cap ignored) builds in one frame.
TEST(TitleDesignerWindowDeathTest, IndexBuildSpansFrames) {
    IN_EDITOR("indexframes", {
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        const fs::path file = newTitle(w, "indexframes");
        ASSERT_TRUE(window->open(file, nullptr));
        const std::string layer = titleLayer(*window);
        ta::Json ops = ta::Json::array();
        for (int i = 0; i < 4; i++) ops.push_back({{"op", "add_rim"}, {"layer", "id:" + layer}});
        for (const char* type : {"outer_glow", "drop_shadow", "inner_glow", "gradient_overlay"}) ops.push_back({{"op", "add_effect"}, {"layer", "id:" + layer}, {"type", type}});
        std::string error;
        ASSERT_TRUE(window->runOps(ops, "Add Rims and Effects", &error)) << error;
        ASSERT_TRUE(window->select({"Title"}, &error)) << error;
        ASSERT_TRUE(window->setPanel("properties", &error)) << error;
        window->setSearch("glow");
        Access::setIndexUnitsPerFrame(*window, 1);
        auto timings = [&]() { return window->state()["timings"]; };
        auto drain = [&](int& frames, int& most) {
            frames = 0;
            most = 0;
            for (int i = 0; i < 200; i++) {
                Access::drawFrame(*window);
                most = std::max(most, timings().value("index_units_frame", 0));
                if (timings().value("index_building", false)) frames++;
                else if (timings().value("index_entries", (size_t)0) > 0) break;
            }
        };
        int frames = 0, most = 0;
        drain(frames, most);
        EXPECT_GE(timings().value("index_frames", 0), 10) << "the first build spans a frame per page";
        EXPECT_LE(most, 1) << "no frame collects more than its share";
        // an edit: the old index answers the search while the new one builds
        ASSERT_TRUE(window->runOps(ta::Json::array({{{"op", "set"}, {"layer", "id:" + layer}, {"values", {{"size", 150}}}}}), "Change Size", &error)) << error;
        Access::drawFrame(*window);
        ASSERT_TRUE(timings().value("index_building", false)) << "a build after the edit";
        EXPECT_GT(Access::servedHits(*window, "glow"), 0u) << "the stale index answers mid-build";
        drain(frames, most);
        EXPECT_GE(timings().value("index_frames", 0), 10);
        EXPECT_LE(most, 1);
        const std::vector<std::string> spread = Access::indexRows(*window);
        // the same document in one pass
        PropertyIndex full;
        clearRecordedRows();
        collectIndex(window->designer(), window, full, false);
        std::vector<std::string> once;
        for (size_t i = 0; i < full.size(); i++) once.push_back(full.entry(i).rowId + "|" + full.entry(i).label);
        EXPECT_EQ(spread, once) << "the index built across frames is the one-pass index";
    });
}

// No system CJK font and a title in Japanese: the window says so once, naming the setting, and
// says nothing with a CJK font found or with no CJK text. Red control: the notice never drawn
// (state().notices empty) fails the first check.
TEST(TitleDesignerWindowDeathTest, CjkFontNotice) {
    IN_EDITOR("cjk", {
        TitleDesignerWindow* window = w.app->getTitleDesignerWindow();
        const fs::path file = newTitle(w, "cjk");
        ASSERT_TRUE(window->open(file, nullptr));
        const std::string layer = titleLayer(*window);
        ASSERT_FALSE(layer.empty());
        std::string error;
        ASSERT_TRUE(window->runOps(ta::Json::array({{{"op", "set_text"}, {"layer", "id:" + layer}, {"text", "\xE6\xBF\x80\xE7\x86\xB1 JACKPOT"}}}), "Set Text", &error)) << error;
        auto notices = [&]() { return window->state().value("notices", ta::Json::array()); };
        Access::setSystemCjk(*window, []() { return 0; });
        Access::drawFrame(*window);
        EXPECT_EQ(notices(), ta::Json::array({"cjk_font_missing"})) << "no CJK font and Japanese text";
        Access::setSystemCjk(*window, []() { return -1; });
        Access::drawFrame(*window);
        EXPECT_TRUE(notices().empty()) << "not while the search runs";
        Access::setSystemCjk(*window, []() { return 1; });
        Access::drawFrame(*window);
        EXPECT_TRUE(notices().empty()) << "a CJK font was found";
        Access::setSystemCjk(*window, []() { return 0; });
        ASSERT_TRUE(window->runOps(ta::Json::array({{{"op", "set_text"}, {"layer", "id:" + layer}, {"text", "JACKPOT"}}}), "Set Text", &error)) << error;
        Access::drawFrame(*window);
        EXPECT_TRUE(notices().empty()) << "no CJK text";
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
