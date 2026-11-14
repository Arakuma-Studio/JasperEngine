// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Painting in the Sprite Normal Map window (sprite normals Phase 5 spec 5 and 7.3) through its
// scripted API and SpriteNormalsPaintWindowTestAccess (the friend the window declares): strokes
// and brush footprints, mirror X, the wand, the stroke undo stack, opening a painted map (and one
// of another size), Alt-click sampling, Save writing the paint chunk (no MCP server: the write is
// ProjectFileWrites'). Every case runs in a child process with the editor's App and its project set
// up for good (the SpriteNormalsWindowTest pattern): a 32x32 shaded disk hero.png and a 128x32
// 4-frame walk.png drawn by the Sprite "Walker".

#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

#include "App.h"
#include "AppSettings.h"
#include "Backend.h"
#include "JasperEngine.h"
#include "Platform.h"
#include "Project.h"
#include "ScriptDataTestSupport.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "mcp/McpServer.h"
#include "spritenormals/SpriteNormalsPaint.h"
#include "spritenormals/SpriteNormalsRecipe.h"
#include "spritenormals/SpriteNormalsSession.h"
#include "texsynth/SynthImage.h"
#include "window/SpriteNormalsWindow.h"

#include "imgui.h"
#include "SymlinkSupport.h"

#include "support/PosixCompat.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <tuple>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace sn = jasperengine::editor::spritenormals;
namespace ts = jasperengine::editor::texsynth;
namespace fs = std::filesystem;

namespace jasperengine::editor {

    struct SpriteNormalsPaintWindowTestAccess {
        using W = SpriteNormalsWindow;
        static void advance(W& w, double seconds) {
            w.clockOffset += seconds;
            w.tick();
        }
        static void tick(W& w) { w.tick(); }
        static bool busy(W& w) { return w.isRunning() || w.changedAt >= 0.0 || w.saveAll.active || !w.quickJobs.empty(); }
        static void holdConfirms(W& w, bool hold) { w.holdConfirms = hold; }
        static std::function<void()> lastConfirm(W& w) { return w.lastConfirm; }
        static size_t undoSteps(W& w) { return w.paintUndo.size(); }
        static void undoByteLimit(W& w, size_t bytes) { w.paintUndoByteLimit = bytes; }
        static const sn::Image& displayed(W& w) { return w.displayNormal(); }
        static sn::Image composed(W& w) { return w.composedNormal(); }
        static void refreshWarningsNow(W& w) { w.refreshWarnings(true); }
        static void setPaintOpen(W& w, bool open) { w.paintOpen = open; }
        static bool setSourcePath(W& w, const std::string& path, std::string& error) { return w.setSourcePath(path, error); }
        static bool setSourceEntity(W& w, uint32_t sceneId, Entity entity, std::string& error) { return w.setSourceEntity(sceneId, entity, error); }
        static sn::Image litFull(W& w) {
            w.renderLit();
            return w.litRgba;
        }
        static sn::Image litAfterStroke(W& w) {
            w.renderLitRect();
            return w.litRgba;
        }
        static void checkTarget(W& w) {
            w.paintStampCheckedAt = -1.0;
            w.checkPaintTarget();
        }
        static sn::PaintTopology topology(W& w) {
            const sn::PaintTopology* t = w.topologyFor(true);
            return t ? *t : sn::PaintTopology();
        }
        static void beginStroke(W& w) { w.beginStroke(); }
        static void strokeTo(W& w, int x, int y) { w.strokeTo(x, y); }
        static void endStroke(W& w) { w.endStroke(); }
        static void drawFrame(W& w) {
            ImGui::NewFrame();
            w.show();
            ImGui::EndFrame();
        }
    };

}

namespace {

    using Access = SpriteNormalsPaintWindowTestAccess;
    using Px = std::array<uint8_t, 4>;
    using Pixels = std::vector<std::array<int, 2>>;

    ts::Image disk(int w, int h, int cells = 1) {
        ts::Image image = ts::Image::make(w, h, 4, 8);
        const int cw = w / cells;
        for (int c = 0; c < cells; c++) {
            const float cx = c * cw + cw * 0.5f, cy = h * 0.5f, r = std::min(cw, h) * 0.42f;
            for (int y = 0; y < h; y++) {
                for (int x = c * cw; x < (c + 1) * cw; x++) {
                    uint8_t* p = image.pixel(x, y);
                    const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                    if (dx * dx + dy * dy > r * r) continue;
                    const float shade = std::clamp(0.65f - (dx + dy) / (4.0f * r), 0.15f, 1.0f);
                    p[0] = static_cast<uint8_t>(200 * shade + 40);
                    p[1] = static_cast<uint8_t>(120 * shade + 30);
                    p[2] = static_cast<uint8_t>(60 * shade + 20);
                    p[3] = 255;
                }
            }
        }
        return image;
    }

    void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    void writePng(const fs::path& path, const ts::Image& image) {
        std::vector<uint8_t> png;
        ASSERT_TRUE(ts::encodePng(image, png));
        writeBytes(path, png);
    }

    struct World {
        fs::path dir;
        Project* p = nullptr;
        uint32_t sceneId = 0;
        Entity walker = NULL_ENTITY;
        Scene* sceneObject() const { return p->getScene(sceneId)->scene; }
        fs::path file(const std::string& rel) const { return dir / fs::u8path(rel); }
    };

    void runInEditor(const std::string& name, const std::function<void(World&)>& body) {
        const fs::path root = fs::temp_directory_path() / ("jasper-sn-paint-" + name + "-" + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root / "config");
        setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
        World w;
        w.dir = root / "project";
        fs::create_directories(w.dir / ".jasperengine");
        std::ofstream(w.dir / "project.yaml") << "name: snpaint\n";
        writePng(w.dir / "assets/hero.png", disk(32, 32));
        writePng(w.dir / "assets/walk.png", disk(128, 32, 4));
        writePng(w.dir / "assets/wide.png", disk(48, 32));

        AppSettings::initialize();
        App& app = Backend::getApp();
        w.p = app.getProject();
        static editor::Platform* platform = nullptr;
        platform = new editor::Platform(w.p);
        // the test binary's engine is TestMain's (a second Engine::systemInit would restart its jobs
        // and logger): this System is swapped in
        jasperengine::SystemTestAccess::swap(platform);
        Engine::pauseGameEvents(true);
        Engine::setAsyncLoading(false);
        if (!w.p->loadProject(w.dir, false)) std::exit(90);
        Engine::setAsyncLoading(false);
        // no MCP server: the window's Save writes through ProjectFileWrites without it
        if (mcp::McpServer::get().isStarted()) std::exit(91);

        w.p->createNewScene("Normals", SceneType::SCENE_2D);
        w.sceneId = w.p->getSelectedSceneId();
        auto* cmd = new CreateEntityCmd(w.p, w.sceneId, "Walker", EntityCreationType::SPRITE);
        cmd->setQuiet(true);
        CommandHandle::get(w.sceneId)->addCommandNoMerge(cmd);
        w.walker = cmd->getEntity();
        w.sceneObject()->getComponent<MeshComponent>(w.walker).submeshes[0].material.baseColorTexture = Texture("assets/walk.png");
        SpriteComponent& walk = w.sceneObject()->getComponent<SpriteComponent>(w.walker);
        walk.width = walk.height = 32;
        for (int i = 0; i < 4; i++) {
            walk.framesRect[i].name = "frame_" + std::to_string(i);
            walk.framesRect[i].rect = Rect(static_cast<float>(i * 32), 0.0f, 32.0f, 32.0f);
        }
        walk.numFramesRect = 4;
        w.p->clearSelectedEntities(w.sceneId);

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
        // the forked editor ends without its static destructors: its jobs (thumbnails, imports)
        // may still read the App while it would be destroyed
        std::_Exit(failures ? 1 : 0);
    }

#define IN_EDITOR(name, ...)                                                    \
    do {                                                                        \
        auto inEditor = [&](World& w) __VA_ARGS__;                              \
        GTEST_FLAG_SET(death_test_style, "threadsafe"); EXPECT_EXIT(runInEditor(name, inEditor), ::testing::ExitedWithCode(0), ""); \
    } while (0)

    bool settle(SpriteNormalsWindow& window, int maxMs = 20000) {
        for (int i = 0; i < maxMs / 2; i++) {
            Access::advance(window, 0.2);
            if (!Access::busy(window)) {
                Access::tick(window);
                return true;
            }
            usleep(2000);
        }
        return false;
    }

    // a window with the source open and its first result shown
    std::unique_ptr<SpriteNormalsWindow> openWindow(World& w, const std::string& source) {
        auto window = std::make_unique<SpriteNormalsWindow>(w.p);
        std::string error;
        EXPECT_TRUE(window->openSources({w.file(source)}, error)) << error;
        EXPECT_TRUE(settle(*window));
        EXPECT_TRUE(window->hasResult());
        return window;
    }

    // the painted pixels of the overlay
    std::set<std::array<int, 2>> painted(const sn::PaintOverlay& o) {
        std::set<std::array<int, 2>> out;
        for (int y = 0; y < o.height; y++) {
            for (int x = 0; x < o.width; x++) {
                if (o.rgba[(static_cast<size_t>(y) * o.width + x) * 4 + 3] > 0) out.insert({x, y});
            }
        }
        return out;
    }

    Px at(const sn::PaintOverlay& o, int x, int y) {
        const uint8_t* p = &o.rgba[(static_cast<size_t>(y) * o.width + x) * 4];
        return {p[0], p[1], p[2], p[3]};
    }

    sn::PaintBrush brushOf(int size, sn::PaintBrush::Shape shape = sn::PaintBrush::Shape::Round, float strength = 1.0f) {
        sn::PaintBrush b;
        b.size = size;
        b.shape = shape;
        b.strength = strength;
        return b;
    }

    // a painted map at `output` written by the session (paint mode replace, `ops`)
    void writePaintedMap(World& w, const std::string& source, const std::string& output, const char* ops) {
        sn::Spec spec;
        spec.source = source;
        spec.settings = sn::presetSettings(sn::Preset::Default);
        spec.output.normal = output;
        spec.overwrite = true;
        spec.paintMode = sn::PaintMode::Replace;
        std::vector<sn::Problem> problems;
        ASSERT_TRUE(sn::paintOpsFromJson(nlohmann::json::parse(ops), spec.paint, problems));
        sn::Prepared prepared;
        ASSERT_TRUE(sn::prepare(w.dir, spec, nullptr, prepared, problems)) << (problems.empty() ? "" : problems[0].message);
        const sn::Result result = sn::generatePrepared(prepared);
        ASSERT_EQ(result.status, sn::Status::Ok);
        std::vector<sn::OutputFile> files;
        std::string error, failed;
        ASSERT_TRUE(sn::encodeOutputs(prepared, result, files, error)) << error;
        ASSERT_TRUE(sn::writeOutputsDirect(files, failed, error)) << error;
    }

    const SpriteNormalsWindow::WarningRow* row(const SpriteNormalsWindow& window, const std::string& kind) {
        for (const SpriteNormalsWindow::WarningRow& r : window.getWarnings()) {
            if (r.kind == kind) return &r;
        }
        return nullptr;
    }

}

// ---- 1. strokes and footprints

TEST(SpriteNormalsPaintWindowDeathTest, StrokesTouchTheLineAndTheFootprintOnce) {
    IN_EDITOR("strokes", {
        auto window = openWindow(w, "assets/hero.png");
        EXPECT_FALSE(window->paintStroke({{16, 16}})) << "Navigate paints nothing";
        window->setPaintTool(sn::PaintTool::Flatten);
        window->setPaintBrush(brushOf(1));
        ASSERT_TRUE(window->paintStroke({{8, 8}, {14, 12}}));
        const Pixels line = sn::linePixels({8, 8}, {14, 12});
        EXPECT_EQ(painted(window->paintOverlay()), (std::set<std::array<int, 2>>(line.begin(), line.end())));
        EXPECT_EQ(at(window->paintOverlay(), 8, 8), (Px{128, 128, 255, 255}));
        window->clearPaint();

        // footprints: round and square 3 (3 x 3), round 4 (4 x 4 without corners), square 4
        for (auto [size, shape, count] : {std::tuple{3, sn::PaintBrush::Shape::Round, 9u}, std::tuple{3, sn::PaintBrush::Shape::Square, 9u},
                                          std::tuple{4, sn::PaintBrush::Shape::Round, 12u}, std::tuple{4, sn::PaintBrush::Shape::Square, 16u}}) {
            window->clearPaint();
            window->setPaintBrush(brushOf(size, shape));
            ASSERT_TRUE(window->paintStroke({{16, 16}}));
            EXPECT_EQ(window->paintedPixelCount(), count) << size;
        }
        window->setPaintBrush(brushOf(2));
        window->clearPaint();
        window->paintStroke({{16, 16}});
        EXPECT_EQ(painted(window->paintOverlay()), (std::set<std::array<int, 2>>{{15, 15}, {16, 15}, {15, 16}, {16, 16}})) << "even sizes up-left";

        // no double application: a stroke crossing itself at strength 0.5 covers 128 once
        window->clearPaint();
        window->setPaintBrush(brushOf(3, sn::PaintBrush::Shape::Square, 0.5f));
        window->paintStroke({{10, 16}, {20, 16}, {10, 16}, {20, 16}});
        for (const auto& p : painted(window->paintOverlay())) EXPECT_EQ(at(window->paintOverlay(), p[0], p[1])[3], 128);
        // Erase 0.5 in one stroke halves once (two strokes: twice)
        window->setPaintTool(sn::PaintTool::Erase);
        window->paintStroke({{12, 16}, {12, 16}, {13, 16}, {12, 16}});
        EXPECT_EQ(at(window->paintOverlay(), 12, 16)[3], 64);
        window->paintStroke({{12, 16}});
        EXPECT_EQ(at(window->paintOverlay(), 12, 16)[3], 32);
        // the shown normal is the composite; Show paint off shows the generation
        window->setPaintTool(sn::PaintTool::Flatten);
        window->setPaintBrush(brushOf(1));
        window->paintStroke({{16, 10}});
        const sn::Image& shown = Access::displayed(*window);
        EXPECT_EQ(Px({shown.pixel(16, 10)[0], shown.pixel(16, 10)[1], shown.pixel(16, 10)[2], shown.pixel(16, 10)[3]}), (Px{128, 128, 255, 255}));
        window->setShowPaint(false);
        EXPECT_NE(std::memcmp(Access::displayed(*window).pixel(16, 10), "\x80\x80\xff", 3), 0);
        window->setShowPaint(true);
    });
}

// the Paint foldout and the canvas overlays draw in every state (a headless ImGui frame)
TEST(SpriteNormalsPaintWindowDeathTest, EveryPaintStateDrawsAFrame) {
    IN_EDITOR("draw", {
        ImGuiContext* context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400, 900);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        SpriteNormalsWindow window(w.p);
        window.open();
        Access::setPaintOpen(window, true);
        Access::drawFrame(window);   // no source: the brushes say why they are off
        std::string error;
        ASSERT_TRUE(window.openForEntity(w.sceneId, w.walker, 0, false, error)) << error;
        ASSERT_TRUE(settle(window));
        Access::setPaintOpen(window, true);
        window.setHighlightPainted(true);
        for (sn::PaintTool tool : {sn::PaintTool::Angle, sn::PaintTool::Flatten, sn::PaintTool::Smooth, sn::PaintTool::Erase}) {
            window.setPaintTool(tool);
            for (bool wand : {false, true}) {
                sn::PaintBrush b = brushOf(3);
                b.fill = wand ? sn::PaintBrush::Fill::Wand : sn::PaintBrush::Fill::Brush;
                b.mirrorX = !wand;
                window.setPaintBrush(b);
                window.paintStroke({{20, 16}, {40, 16}});
                for (SpriteNormalsWindow::View v : {SpriteNormalsWindow::View::Color, SpriteNormalsWindow::View::Normal, SpriteNormalsWindow::View::Lit,
                                                    SpriteNormalsWindow::View::Split}) {
                    window.setView(v);
                    Access::drawFrame(window);
                }
            }
        }
        window.setFrame(1);
        Access::drawFrame(window);
        window.setShowPaint(false);
        Access::drawFrame(window);
        // unsaved paint: Discard (the close and project-switch prompts' choice) drops it
        window.setPaintTool(sn::PaintTool::Flatten);
        window.setPaintBrush(brushOf(2));
        // frame 1 is shown: a stroke stays in it
        const size_t before = window.paintedPixelCount();
        ASSERT_TRUE(window.paintStroke({{16, 16}}));
        EXPECT_EQ(window.paintedPixelCount(), before);
        ASSERT_TRUE(window.paintStroke({{48, 16}}));
        EXPECT_TRUE(window.hasUnsavedPaint());
        window.discardPaint();
        EXPECT_FALSE(window.hasUnsavedPaint());
        window.setOpen(false);
        Access::drawFrame(window);
        ImGui::DestroyContext(context);
    });
}

// ---- 2. mirror X

TEST(SpriteNormalsPaintWindowDeathTest, MirrorXWithinTheCellMirrorsTheDirection) {
    IN_EDITOR("mirror", {
        auto window = std::make_unique<SpriteNormalsWindow>(w.p);
        std::string error;
        ASSERT_TRUE(window->openForEntity(w.sceneId, w.walker, 0, false, error)) << error;
        ASSERT_TRUE(settle(*window));
        ASSERT_TRUE(window->hasResult());
        window->setPaintTool(sn::PaintTool::Angle);
        sn::PaintBrush b = brushOf(1);
        b.mirrorX = true;
        b.direction = {1.0f, 0.0f, 1.0f};
        window->setPaintBrush(b);
        // frame 1 is x 32..63: (36, 16) mirrors to 32 + 32 - 1 - 4 = 59
        ASSERT_TRUE(window->paintStroke({{36, 16}}));
        EXPECT_EQ(painted(window->paintOverlay()), (std::set<std::array<int, 2>>{{36, 16}, {59, 16}}));
        EXPECT_EQ(at(window->paintOverlay(), 36, 16), (Px{218, 128, 218, 255}));
        EXPECT_EQ(at(window->paintOverlay(), 59, 16), (Px{37, 128, 218, 255}));
        EXPECT_EQ(Access::undoSteps(*window), 1u) << "the mirrored pixels join the stroke";
    });
}

// ---- 3. the wand

TEST(SpriteNormalsPaintWindowDeathTest, WandExactToleranceContiguousAndFrames) {
    IN_EDITOR("wand", {
        auto window = std::make_unique<SpriteNormalsWindow>(w.p);
        std::string error;
        ASSERT_TRUE(window->openForEntity(w.sceneId, w.walker, 0, false, error)) << error;
        ASSERT_TRUE(settle(*window));
        window->setPaintTool(sn::PaintTool::Flatten);
        auto fill = [&](float tolerance, bool contiguous, bool all) {
            window->clearPaint();
            sn::PaintBrush b;
            b.fill = sn::PaintBrush::Fill::Wand;
            b.wandTolerance = tolerance;
            b.wandContiguous = contiguous;
            b.wandAllFrames = all;
            window->setPaintBrush(b);
            EXPECT_TRUE(window->wandFill(16, 16));
            return window->paintedPixelCount();
        };
        const size_t exact = fill(0.0f, false, false);
        const size_t exactContiguous = fill(0.0f, true, false);
        const size_t tolerant = fill(0.08f, false, false);
        const size_t allFrames = fill(0.0f, false, true);
        EXPECT_GT(exact, 0u);
        EXPECT_LE(exactContiguous, exact);
        EXPECT_GT(tolerant, exact) << "similar shades join";
        EXPECT_EQ(allFrames, 4 * exact) << "the four frames are the same disk";
        // every pixel painted has the clicked colour (exact)
        fill(0.0f, false, true);
        ts::Image source;
        std::string why;
        ASSERT_TRUE(ts::loadImage(w.file("assets/walk.png"), source, why)) << why;
        for (const auto& p : painted(window->paintOverlay())) EXPECT_EQ(std::memcmp(source.pixel(p[0], p[1]), source.pixel(16, 16), 3), 0);
        EXPECT_GT(Access::undoSteps(*window), 0u);
        // a transparent pixel: nothing
        window->clearPaint();
        EXPECT_FALSE(window->wandFill(0, 0));
        EXPECT_EQ(window->paintedPixelCount(), 0u);
    });
}

// ---- 4. the stroke stack, Clear, unsaved paint across Save and undo

TEST(SpriteNormalsPaintWindowDeathTest, UndoRedoCapClearAndUnsavedPaint) {
    IN_EDITOR("undo", {
        auto window = openWindow(w, "assets/hero.png");
        window->setPaintTool(sn::PaintTool::Flatten);
        window->setPaintBrush(brushOf(1));
        EXPECT_FALSE(window->hasUnsavedPaint());
        window->paintStroke({{10, 16}});
        window->paintStroke({{11, 16}});
        EXPECT_TRUE(window->hasUnsavedPaint());
        EXPECT_EQ(window->paintedPixelCount(), 2u);
        EXPECT_TRUE(window->undoPaint());
        EXPECT_EQ(window->paintedPixelCount(), 1u);
        EXPECT_TRUE(window->canRedoPaint());
        EXPECT_TRUE(window->redoPaint());
        EXPECT_EQ(window->paintedPixelCount(), 2u);
        // Clear is one step
        window->clearPaint();
        EXPECT_EQ(window->paintedPixelCount(), 0u);
        EXPECT_TRUE(window->undoPaint());
        EXPECT_EQ(window->paintedPixelCount(), 2u);

        // Save writes the chunk: the paint is saved, the stack stays
        std::vector<std::string> written;
        std::string error;
        ASSERT_TRUE(window->save(true, written, error)) << error;
        EXPECT_FALSE(window->hasUnsavedPaint());
        const fs::path map = w.file("assets/hero_normal.png");
        EXPECT_EQ(sn::readPaint(map).value_or(sn::PaintOverlay()).paintedCount(), 2u);
        EXPECT_EQ(sn::readRecipe(map).value_or(nlohmann::json())["paint"]["painted"], 2);
        // the file's normal at a painted pixel is the composite
        ts::Image decoded;
        ASSERT_TRUE(ts::loadImage(map, decoded, error)) << error;
        EXPECT_EQ(std::memcmp(decoded.pixel(10, 16), "\x80\x80\xff", 3), 0);
        EXPECT_TRUE(window->canUndoPaint());
        EXPECT_TRUE(window->undoPaint());
        EXPECT_TRUE(window->hasUnsavedPaint());
        EXPECT_TRUE(window->redoPaint());
        EXPECT_FALSE(window->hasUnsavedPaint());
        // the project's undo of the Save removes the map: the strokes are unsaved again, and said so
        EXPECT_TRUE(w.p->getProjectCommandHistory()->undo());
        EXPECT_FALSE(fs::exists(map));
        Access::checkTarget(*window);
        EXPECT_TRUE(window->hasUnsavedPaint());
        EXPECT_NE(window->getStatus().find("hero_normal.png is gone"), std::string::npos) << window->getStatus();
        // redo puts the saved file back: saved again
        EXPECT_TRUE(w.p->getProjectCommandHistory()->redo());
        Access::checkTarget(*window);
        EXPECT_FALSE(window->hasUnsavedPaint());
        // a stroke redo is dropped when another history's step is undone (the App does it)
        window->paintStroke({{20, 20}});
        EXPECT_TRUE(window->undoPaint());
        EXPECT_TRUE(window->canRedoPaint());
        window->dropPaintRedo();
        EXPECT_FALSE(window->canRedoPaint());

        // the cap: 256 steps, the oldest dropped
        for (int i = 0; i < 260; i++) window->paintStroke({{8 + i % 16, 8 + (i / 16) % 16}});
        EXPECT_EQ(Access::undoSteps(*window), 256u);

        // discarding goes back to the saved paint
        window->discardPaint();
        EXPECT_FALSE(window->hasUnsavedPaint());
        EXPECT_FALSE(window->canUndoPaint());
    });
}

// ---- 5. opening a painted map, and one of another size

TEST(SpriteNormalsPaintWindowDeathTest, OpeningAPaintedMapLoadsItsPaintAndAMismatchBlocksSave) {
    IN_EDITOR("open", {
        writePaintedMap(w, "assets/hero.png", "assets/hero_normal.png", R"([{"op":"flatten","where":{"rect":[10,10,3,3]}}])");
        {
            auto window = openWindow(w, "assets/hero.png");
            EXPECT_EQ(window->paintedPixelCount(), 9u);
            EXPECT_FALSE(window->hasUnsavedPaint());
            EXPECT_FALSE(window->canUndoPaint());
            EXPECT_EQ(std::memcmp(Access::displayed(*window).pixel(11, 11), "\x80\x80\xff", 3), 0) << "composited over the new result";
            // Edit Normal Map... too
            std::string error;
            ASSERT_TRUE(window->openRecipe(w.file("assets/hero_normal.png"), error)) << error;
            EXPECT_EQ(window->paintedPixelCount(), 9u);
            ASSERT_TRUE(settle(*window));
            // another output path without a paint layer: Save As, the paint stays, unsaved for that file
            window->setOutput("assets/other_normal.png");
            EXPECT_EQ(window->paintedPixelCount(), 9u);
            EXPECT_TRUE(window->hasUnsavedPaint());
            std::vector<std::string> written;
            ASSERT_TRUE(window->save(true, written, error)) << error;
            EXPECT_EQ(sn::readPaint(w.file("assets/other_normal.png")).value_or(sn::PaintOverlay()).paintedCount(), 9u);
            EXPECT_FALSE(window->hasUnsavedPaint());
        }
        // a 48 x 32 map's paint at hero's default output: a fix row, Save waits for Clear Paint
        writePaintedMap(w, "assets/wide.png", "assets/hero_normal.png", R"([{"op":"flatten","where":{"rect":[20,10,3,3]}}])");
        auto window = openWindow(w, "assets/hero.png");
        EXPECT_TRUE(window->paintSizeMismatch());
        Access::refreshWarningsNow(*window);
        const SpriteNormalsWindow::WarningRow* mismatch = row(*window, "paint_size_mismatch");
        ASSERT_NE(mismatch, nullptr);
        EXPECT_TRUE(mismatch->error);
        EXPECT_EQ(mismatch->fixes, (std::vector<std::string>{"Clear Paint"}));
        EXPECT_NE(mismatch->text.find("48x32"), std::string::npos) << mismatch->text;
        std::vector<std::string> written;
        std::string error;
        EXPECT_FALSE(window->save(true, written, error));
        EXPECT_NE(error.find("Clear Paint"), std::string::npos) << error;
        window->setPaintTool(sn::PaintTool::Flatten);
        EXPECT_FALSE(window->paintStroke({{16, 16}})) << "painting waits too";
        ASSERT_TRUE(window->applyFix("paint_size_mismatch", "Clear Paint", error)) << error;
        EXPECT_FALSE(window->paintSizeMismatch());
        EXPECT_TRUE(window->paintStroke({{16, 16}}));
        ASSERT_TRUE(window->save(true, written, error)) << error;
        EXPECT_EQ(sn::readPaint(w.file("assets/hero_normal.png")).value_or(sn::PaintOverlay()).width, 32);
    });
}

// ---- 6. Alt-click sampling

TEST(SpriteNormalsPaintWindowDeathTest, SamplingSetsTheDirectionToTheCompositedNormal) {
    IN_EDITOR("sample", {
        auto window = openWindow(w, "assets/hero.png");
        EXPECT_FALSE(window->samplePaintDirection(0, 0)) << "a transparent pixel";
        EXPECT_EQ(window->getPaintTool(), sn::PaintTool::Navigate);
        // an unpainted pixel: the generation's normal there
        ASSERT_TRUE(window->samplePaintDirection(9, 20));
        EXPECT_EQ(window->getPaintTool(), sn::PaintTool::Angle);
        const sn::Image composed = Access::composed(*window);
        const std::array<float, 3> n = sn::decodeDirection(composed.pixel(9, 20));
        const std::array<float, 3> d = window->getPaintBrush().direction;
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int k = 0; k < 3; k++) EXPECT_NEAR(d[k], n[k] / length, 1e-5f);
        // a painted one: the painted direction (within the 8-bit encoding)
        sn::PaintBrush b = brushOf(1);
        b.direction = sn::directionFromAngles(200.0f, 50.0f);
        window->setPaintBrush(b);
        window->paintStroke({{16, 16}});
        sn::PaintBrush other = brushOf(1);
        window->setPaintBrush(other);
        ASSERT_TRUE(window->samplePaintDirection(16, 16));
        float az = 0, tilt = 0;
        sn::anglesOf(window->getPaintBrush().direction, az, tilt);
        EXPECT_NEAR(az, 200.0f, 1.5f);
        EXPECT_NEAR(tilt, 50.0f, 1.5f);
    });
}

// ---- the scripted ops and a batch

TEST(SpriteNormalsPaintWindowDeathTest, ScriptedOpsAndBatchModeNeverPaints) {
    IN_EDITOR("ops", {
        auto window = openWindow(w, "assets/hero.png");
        std::vector<sn::PaintOp> ops;
        std::vector<sn::Problem> problems;
        ASSERT_TRUE(sn::paintOpsFromJson(nlohmann::json::parse(R"([{"op":"flatten","where":{"rect":[8,8,4,4]}}])"), ops, problems));
        EXPECT_TRUE(window->applyPaintOps(ops, &problems));
        EXPECT_EQ(window->paintedPixelCount(), 16u);
        EXPECT_EQ(Access::undoSteps(*window), 1u);
        ASSERT_TRUE(sn::paintOpsFromJson(nlohmann::json::parse(R"([{"op":"flatten","where":{"frame":3}}])"), ops, problems));
        EXPECT_FALSE(window->applyPaintOps(ops, &problems));
        ASSERT_FALSE(problems.empty());
        EXPECT_EQ(problems[0].kind, "invalid_paint");
        EXPECT_EQ(window->paintedPixelCount(), 16u);

        // a batch: the brushes are off (each map keeps its own paint when saved)
        std::string error;
        ASSERT_TRUE(window->openSources({w.file("assets/hero.png"), w.file("assets/walk.png")}, error)) << error;
        settle(*window);
        EXPECT_FALSE(window->paintDisabledReason().empty());
        window->setPaintTool(sn::PaintTool::Flatten);
        EXPECT_FALSE(window->paintStroke({{16, 16}}));
    });
}

// ---- the cost of a stroke on a large atlas (2048 x 2048): per mouse sample, the stroke and the
// composite shown (printed; a loose bound only)
TEST(SpriteNormalsPaintWindowDeathTest, StrokeSamplesOnALargeAtlas) {
    IN_EDITOR("bench", {
        writePng(w.file("assets/big.png"), disk(2048, 2048));
        auto window = std::make_unique<SpriteNormalsWindow>(w.p);
        std::string error;
        ASSERT_TRUE(window->openSources({w.file("assets/big.png")}, error)) << error;
        ASSERT_TRUE(window->regenerate(error)) << error;
        ASSERT_TRUE(settle(*window, 120000));
        ASSERT_TRUE(window->hasResult());
        Access::displayed(*window);
        for (sn::PaintTool tool : {sn::PaintTool::Flatten, sn::PaintTool::Smooth}) {
            window->setPaintTool(tool);
            sn::PaintBrush b = brushOf(8);
            b.smoothRadius = 2;
            window->setPaintBrush(b);
            Access::beginStroke(*window);
            double total = 0.0;
            const int samples = 20;
            for (int k = 0; k < samples; k++) {
                const auto t0 = std::chrono::steady_clock::now();
                Access::strokeTo(*window, 700 + k * 30, 900 + k * 10);
                Access::displayed(*window);
                total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }
            Access::endStroke(*window);
            std::fprintf(stderr, "[ bench ] 2048x2048 %s size 8: %.1f ms per stroke sample\n", tool == sn::PaintTool::Smooth ? "Smooth r2" : "Flatten", total / samples);
            EXPECT_LT(total / samples, 5000.0);
        }
        EXPECT_GT(window->paintedPixelCount(), 1000u);
        if (std::getenv("JASPER_SN_BENCH")) ADD_FAILURE() << "bench output";   // shows the child's stderr
    });
}

// ---- fix round 1: retargeting with unsaved strokes asks first; a target changed on disk

TEST(SpriteNormalsPaintWindowDeathTest, EveryRetargetAsksBeforeDroppingUnsavedStrokes) {
    IN_EDITOR("retarget", {
        writePaintedMap(w, "assets/walk.png", "assets/walk_normal.png", R"([{"op":"flatten","where":{"rect":[40,10,4,4]}}])");
        auto window = openWindow(w, "assets/hero.png");
        Access::holdConfirms(*window, true);
        window->setPaintTool(sn::PaintTool::Flatten);
        window->setPaintBrush(brushOf(2));
        std::string error;
        // each entry point: asked (nothing changed yet), then Discard does it
        const std::vector<std::pair<std::string, std::function<bool()>>> entries = {
            {"openSources", [&] { return window->openSources({w.file("assets/walk.png")}, error); }},
            {"openRecipe", [&] { return window->openRecipe(w.file("assets/walk_normal.png"), error); }},
            {"openForEntity", [&] { return window->openForEntity(w.sceneId, w.walker, 0, false, error); }},
            {"setSourcePath", [&] { return Access::setSourcePath(*window, "assets/walk.png", error); }},
            {"setSourceEntity", [&] { return Access::setSourceEntity(*window, w.sceneId, w.walker, error); }},
            {"output with paint", [&] {
                 window->setOutput("assets/walk_normal.png");
                 return true;
             }},
        };
        for (const auto& [name, call] : entries) {
            SCOPED_TRACE(name);
            std::string e;
            ASSERT_TRUE(window->openSources({w.file("assets/hero.png")}, e)) << e;   // nothing unsaved: no question
            ASSERT_TRUE(settle(*window));
            ASSERT_TRUE(window->paintStroke({{16, 16}}));
            ASSERT_TRUE(window->hasUnsavedPaint());
            const std::string before = window->currentSpec().fingerprint() + window->currentSpec().output.normal;
            const sn::PaintOverlay painted = window->paintOverlay();
            Access::holdConfirms(*window, true);
            ASSERT_TRUE(call()) << error;
            EXPECT_EQ(window->paintOverlay(), painted) << "the strokes are kept while asking";
            EXPECT_EQ(window->currentSpec().fingerprint() + window->currentSpec().output.normal, before) << "nothing retargeted yet";
            std::function<void()> discard = Access::lastConfirm(*window);
            ASSERT_TRUE(discard);
            discard();
            settle(*window);
            EXPECT_FALSE(window->hasUnsavedPaint());
            EXPECT_NE(window->currentSpec().fingerprint() + window->currentSpec().output.normal, before) << "retargeted after Discard";
            // walk's map has paint (16 px): loaded with it
            if (name != "setSourceEntity" && name != "openForEntity") { EXPECT_EQ(window->paintedPixelCount(), 16u); }
            // reset for the next entry
            window->setConfirmPaintDiscard(true);
        }
        // the Use Base Texture fix (a source the assign target does not draw)
        std::string e;
        ASSERT_TRUE(window->openSources({w.file("assets/hero.png")}, e)) << e;
        ASSERT_TRUE(settle(*window));
        window->setAssignTarget(w.sceneId, w.walker, 0);
        ASSERT_TRUE(window->paintStroke({{16, 16}}));
        ASSERT_TRUE(window->applyFix("size_mismatch", "Use Base Texture", e)) << e;
        EXPECT_FALSE(window->currentSpec().hasSprite()) << "asked first";
        Access::lastConfirm(*window)();
        EXPECT_TRUE(window->currentSpec().hasSprite());
        EXPECT_FALSE(window->hasUnsavedPaint());
        // the scripted path drops them without asking
        ASSERT_TRUE(window->openSources({w.file("assets/hero.png")}, e)) << e;
        ASSERT_TRUE(settle(*window));
        ASSERT_TRUE(window->paintStroke({{16, 16}}));
        window->setConfirmPaintDiscard(false);
        ASSERT_TRUE(window->openSources({w.file("assets/walk.png")}, e)) << e;
        EXPECT_EQ(window->currentSpec().source, "assets/walk.png");
        EXPECT_FALSE(window->hasUnsavedPaint());
    });
}

TEST(SpriteNormalsPaintWindowDeathTest, ATargetRewrittenOnDiskIsTheNewSavedStateAndSaysSo) {
    IN_EDITOR("ondisk", {
        auto window = openWindow(w, "assets/hero.png");
        window->setPaintTool(sn::PaintTool::Flatten);
        window->setPaintBrush(brushOf(2));
        ASSERT_TRUE(window->paintStroke({{16, 16}}));
        std::vector<std::string> written;
        std::string error;
        ASSERT_TRUE(window->save(true, written, error)) << error;
        Access::checkTarget(*window);
        EXPECT_FALSE(window->hasUnsavedPaint()) << "our own Save is not a change on disk";
        ASSERT_TRUE(window->paintStroke({{10, 16}}));
        // another tool (a regeneration) writes the map with other paint
        writePaintedMap(w, "assets/hero.png", "assets/hero_normal.png", R"([{"op":"flatten","where":{"rect":[8,8,3,3]}}])");
        Access::checkTarget(*window);
        EXPECT_TRUE(window->hasUnsavedPaint());
        EXPECT_NE(window->getStatus().find("changed on disk"), std::string::npos) << window->getStatus();
        EXPECT_EQ(window->paintedPixelCount(), 8u) << "the window's strokes stay";
    });
}

TEST(SpriteNormalsPaintWindowDeathTest, StrokesRecompositeAndRelightOnlyTheirPixels) {
    IN_EDITOR("dirty", {
        auto window = openWindow(w, "assets/hero.png");
        EXPECT_FALSE(Access::litFull(*window).empty());
        // the Angle brush starts tilted (azimuth 135, tilt 40), not facing the viewer
        float az = 0, tilt = 0;
        sn::anglesOf(window->getPaintBrush().direction, az, tilt);
        EXPECT_NEAR(az, 135.0f, 0.01f);
        EXPECT_NEAR(tilt, 40.0f, 0.01f);
        window->setPaintTool(sn::PaintTool::Angle);
        sn::PaintBrush b = brushOf(3);
        b.direction = window->getPaintBrush().direction;
        window->setPaintBrush(b);
        Access::displayed(*window);
        ASSERT_TRUE(window->paintStroke({{10, 10}, {20, 14}}));
        window->setPaintTool(sn::PaintTool::Smooth);
        ASSERT_TRUE(window->paintStroke({{12, 20}, {22, 18}}));
        EXPECT_EQ(Access::displayed(*window).data, Access::composed(*window).data) << "incremental composite == full";
        const sn::Image partial = Access::litAfterStroke(*window);
        EXPECT_EQ(partial.data, Access::litFull(*window).data) << "the stroke's lit pixels == a full render";
        window->undoPaint();
        EXPECT_EQ(Access::displayed(*window).data, Access::composed(*window).data);
    });
}

// ---- fix round 2: caches follow the result, never a run's address; an outside rewrite without strokes

TEST(SpriteNormalsPaintWindowDeathTest, SmoothsTopologyFollowsEveryNewResult) {
    IN_EDITOR("topology", {
        auto window = openWindow(w, "assets/hero.png");
        window->setPaintTool(sn::PaintTool::Smooth);
        window->setPaintBrush(brushOf(3));
        ASSERT_TRUE(window->paintStroke({{16, 16}}));
        EXPECT_EQ(Access::topology(*window).owner, std::vector<int>(32 * 32, 0)) << "one whole-image cell";
        // frames changed, regenerated twice (a freed run's address may come back for the next one)
        sn::FramesSpec grid;
        grid.mode = sn::FramesSpec::Mode::Grid;
        grid.grid.tileWidth = grid.grid.tileHeight = 16;
        window->setFrames(grid);
        ASSERT_TRUE(settle(*window));
        std::string error;
        ASSERT_TRUE(window->regenerate(error)) << error;
        ASSERT_TRUE(settle(*window));
        ASSERT_TRUE(window->paintStroke({{8, 8}}));
        const sn::PaintTopology now = Access::topology(*window);
        const sn::Image source = disk(32, 32);
        sn::Image rgba = sn::Image::make(32, 32, 4, 8);
        rgba.data = source.data;
        const sn::PaintTopology expected = sn::paintTopology(rgba, {{0, 0, 16, 16}, {16, 0, 16, 16}, {0, 16, 16, 16}, {16, 16, 16, 16}}, 0.03f);
        EXPECT_EQ(now.owner, expected.owner) << "the new result's four cells";
        EXPECT_EQ(now.island, expected.island);
        EXPECT_EQ(Access::displayed(*window).data, Access::composed(*window).data);
    });
}

TEST(SpriteNormalsPaintWindowDeathTest, AnOutsideRewriteWithoutStrokesIsAdopted) {
    IN_EDITOR("adopt", {
        writePaintedMap(w, "assets/hero.png", "assets/hero_normal.png", R"([{"op":"flatten","where":{"rect":[10,10,3,3]}}])");
        auto window = openWindow(w, "assets/hero.png");
        ASSERT_EQ(window->paintedPixelCount(), 9u);
        // what generate_sprite_normals {paint_mode: "clear", output, overwrite} does: an undoable write without paint
        sn::Spec spec;
        spec.source = "assets/hero.png";
        spec.settings = sn::presetSettings(sn::Preset::Default);
        spec.overwrite = true;
        spec.paintMode = sn::PaintMode::Clear;
        sn::Prepared prepared;
        std::vector<sn::Problem> problems;
        ASSERT_TRUE(sn::prepare(w.dir, spec, nullptr, prepared, problems));
        const sn::Result result = sn::generatePrepared(prepared);
        std::vector<sn::OutputFile> files;
        std::string error;
        ASSERT_TRUE(sn::encodeOutputs(prepared, result, files, error)) << error;
        ASSERT_TRUE(sn::writeOutputsUndoable(w.p, files, error)) << error;
        Access::checkTarget(*window);
        EXPECT_EQ(window->paintedPixelCount(), 0u) << "no strokes here: the file's (empty) paint is taken";
        EXPECT_FALSE(window->hasUnsavedPaint());
        // its undo brings the painted file back: taken again
        EXPECT_TRUE(w.p->getProjectCommandHistory()->undo());
        Access::checkTarget(*window);
        EXPECT_EQ(window->paintedPixelCount(), 9u);
        EXPECT_FALSE(window->hasUnsavedPaint());
    });
}

// ---- integration: a stroke whose undo patch the cap dropped still counts as a stroke

TEST(SpriteNormalsPaintWindowDeathTest, StrokesEvictedFromTheUndoCapStillKeepTheOverlay) {
    IN_EDITOR("evicted", {
        auto window = openWindow(w, "assets/hero.png");
        window->setPaintTool(sn::PaintTool::Flatten);
        window->setPaintBrush(brushOf(2));
        // every patch is over the cap: the stack is empty after each stroke, the strokes are not
        Access::undoByteLimit(*window, 1);
        ASSERT_TRUE(window->paintStroke({{16, 16}}));
        EXPECT_EQ(Access::undoSteps(*window), 0u);
        EXPECT_FALSE(window->canUndoPaint());
        ASSERT_EQ(window->paintedPixelCount(), 4u);
        EXPECT_TRUE(window->hasUnsavedPaint());
        // another tool writes the map with other paint: the saved state, the window's strokes stay
        writePaintedMap(w, "assets/hero.png", "assets/hero_normal.png", R"([{"op":"flatten","where":{"rect":[8,8,3,3]}}])");
        Access::checkTarget(*window);
        EXPECT_EQ(window->paintedPixelCount(), 4u) << "the strokes are not replaced by the file's paint";
        EXPECT_TRUE(window->hasUnsavedPaint());
        EXPECT_NE(window->getStatus().find("changed on disk"), std::string::npos) << window->getStatus();
        // discarding takes the file's paint, and a later rewrite is adopted again
        window->discardPaint();
        EXPECT_EQ(window->paintedPixelCount(), 9u);
        writePaintedMap(w, "assets/hero.png", "assets/hero_normal.png", R"([{"op":"flatten","where":{"rect":[14,14,2,2]}}])");
        Access::checkTarget(*window);
        EXPECT_EQ(window->paintedPixelCount(), 4u);
        EXPECT_FALSE(window->hasUnsavedPaint());
    });
}

// ---- the paint Save: no MCP server, and never through a link out of the assets

TEST(SpriteNormalsPaintWindowDeathTest, PaintSaveNeedsNoServerAndStaysInsideTheAssets) {
    SKIP_WITHOUT_SYMLINKS();
    IN_EDITOR("paintlinks", {
        ASSERT_FALSE(mcp::McpServer::get().isStarted());
        const fs::path outside = w.dir.parent_path() / "outside";
        fs::create_directories(outside);
        fs::create_directory_symlink(outside, w.file("assets/out"));
        auto window = openWindow(w, "assets/hero.png");
        window->setPaintTool(sn::PaintTool::Flatten);
        window->setPaintBrush(brushOf(2));
        window->paintStroke({{10, 16}});
        ASSERT_TRUE(window->hasUnsavedPaint());
        // the paint lives in the normal map: a map through the link is refused, nothing written
        window->setOutput("assets/out/hero_normal.png");
        std::vector<std::string> written;
        std::string error;
        const size_t before = w.p->getProjectCommandHistory()->position();
        EXPECT_FALSE(window->save(true, written, error));
        EXPECT_NE(error.find("symbolic link"), std::string::npos) << error;
        EXPECT_FALSE(fs::exists(outside / "hero_normal.png"));
        EXPECT_EQ(w.p->getProjectCommandHistory()->position(), before);
        EXPECT_TRUE(window->hasUnsavedPaint());
        // inside the assets: one project step with the paint chunk, undo and redo
        window->setOutput("");
        ASSERT_TRUE(window->save(true, written, error)) << error;
        const fs::path map = w.file("assets/hero_normal.png");
        EXPECT_EQ(w.p->getProjectCommandHistory()->position(), before + 1);
        EXPECT_FALSE(window->hasUnsavedPaint());
        EXPECT_GT(sn::readPaint(map).value_or(sn::PaintOverlay()).paintedCount(), 0u);
        ASSERT_TRUE(w.p->getProjectCommandHistory()->undo());
        EXPECT_FALSE(fs::exists(map));
        ASSERT_TRUE(w.p->getProjectCommandHistory()->redo());
        EXPECT_GT(sn::readPaint(map).value_or(sn::PaintOverlay()).paintedCount(), 0u);
    });
}
