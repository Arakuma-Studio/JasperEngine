// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Sprite Normal Map window (sprite normals Phase 4 spec 9.1) through its scripted API (what
// its buttons, the Project window, the Inspector and MCP focus_window call) and
// SpriteNormalsWindowTestAccess, the friend the window declares for its internals. Saving goes
// through the session's writeOutputsUndoable (ProjectFileWrites on the project history; no MCP
// server: IN_EDITOR never starts one, IN_EDITOR_WITH_MCP does), so every case runs in a child
// process with the editor's App, its Platform and its project set up for good (the
// SpriteNormalsSession death test's pattern): a project written by the test with a 32x32
// shaded disk hero.png, a 128x32 4-frame walk.png, a 3-frame blink.gif (a minimal GIF writer
// below), a 2D scene with the Sprites "Hero" (hero.png) and "Walker" (walk.png, 4 frames) and a
// Light 2D of height 0. The workspace round trip runs in process (Stream only).

#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

#include "App.h"
#include "AppSettings.h"
#include "Backend.h"
#include "JasperEngine.h"
#include "Platform.h"
#include "Project.h"
#include "ScriptDataTestSupport.h"
#include "Stream.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/ActionComponent.h"
#include "component/Light2DComponent.h"
#include "component/MeshComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "mcp/McpContext.h"
#include "mcp/McpServer.h"
#include "mcp/tools/W4Shared.h"
#include "spritenormals/SpriteNormalsRecipe.h"
#include "spritenormals/SpriteNormalsSession.h"
#include "texsynth/SynthImage.h"
#include "window/SpriteNormalsWindow.h"

#include "imgui.h"
#include "SymlinkSupport.h"

#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace sn = jasperengine::editor::spritenormals;
namespace ts = jasperengine::editor::texsynth;
namespace fs = std::filesystem;

namespace jasperengine::editor {

    // the window's internals the tests read and drive
    struct SpriteNormalsWindowTestAccess {
        using W = SpriteNormalsWindow;
        // the debounce clock moves on, then one tick
        static void advance(W& w, double seconds) {
            w.clockOffset += seconds;
            w.tick();
        }
        static void tick(W& w) { w.tick(); }
        static std::shared_ptr<sn::SpriteNormalsRun> worker(W& w) { return w.run; }
        static bool debouncePending(W& w) { return w.changedAt >= 0.0; }
        static bool busy(W& w) { return w.isRunning() || w.changedAt >= 0.0 || w.saveAll.active || !w.quickJobs.empty(); }
        static const sn::Image& lit(W& w) {
            w.renderLit();
            return w.litRgba;
        }
        static sn::LightSetup lightSetup(W& w) { return w.lightSetup(); }
        static bool gpuValid(W& w) { return w.colorGpu.valid() || w.normalGpu.valid() || w.litGpu.valid(); }
        static void light(W& w, float& x, float& y, float& height) {
            w.ensureLight();
            x = w.light.x;
            y = w.light.y;
            height = w.light.height;
        }
        static float& split(W& w) { return w.splitPosition; }
        static void refreshWarningsNow(W& w) { w.refreshWarnings(true); }
        static void holdConfirms(W& w, bool hold) { w.holdConfirms = hold; }
        static std::function<void()> lastConfirm(W& w) { return w.lastConfirm; }
        static void requestSave(W& w) { w.requestSave(); }
        static void setAdvancedOpen(W& w, bool open) { w.advancedOpen = open; }
        static std::shared_ptr<sn::SpriteNormalsRun> shown(W& w) { return w.shown(); }
        static bool setSource(W& w, const std::string& path, std::string& error) { return w.setSourcePath(path, error); }
        static size_t quickJobs(W& w) { return w.quickJobs.size(); }
        // the jobs' workers to their end, without a tick (nothing written yet)
        static void waitWorkers(W& w) { w.waitJobWorkers(); }
        static const std::vector<sn::Problem>& problems(W& w) { return w.items[w.current].problems; }
        static double orbitTime(W& w) { return w.orbitTime; }
        static bool itemHasResult(W& w, size_t i) { return i < w.items.size() && w.items[i].result != nullptr; }
        // one headless ImGui frame of show()
        static void drawFrame(W& w) {
            ImGui::NewFrame();
            w.show();
            ImGui::EndFrame();
        }
    };

}

namespace {

    using Access = SpriteNormalsWindowTestAccess;

    // The editor's Platform, counting the calls of getAssetPath (what the session's checks read
    // through System::instance()) made off the main thread
    class CountingPlatform : public editor::Platform {
    public:
        explicit CountingPlatform(Project* project): editor::Platform(project), mainThread(std::this_thread::get_id()) {}
        std::string getAssetPath() override {
            if (std::this_thread::get_id() != mainThread) offMainThread++;
            return editor::Platform::getAssetPath();
        }
        std::thread::id mainThread;
        std::atomic<int> offMainThread{0};
    };

    // ---- images

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
                    // painted shading: brighter up-left
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

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // A GIF with a 256-colour table, index 0 transparent, LZW kept at 9 bits by a clear code every
    // 250 codes (valid, never compressed); frames of `size` x `size`, the given delays (1/100 s)
    std::vector<uint8_t> gif(int size, const std::vector<int>& delays) {
        std::vector<uint8_t> out = {'G', 'I', 'F', '8', '9', 'a'};
        auto u16 = [&](int v) {
            out.push_back(static_cast<uint8_t>(v & 0xff));
            out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
        };
        u16(size);
        u16(size);
        out.push_back(0xF7);   // global table, 8 bits, 256 entries
        out.push_back(0);
        out.push_back(0);
        for (int i = 0; i < 256; i++) {
            out.push_back(static_cast<uint8_t>(i));
            out.push_back(static_cast<uint8_t>(255 - i));
            out.push_back(static_cast<uint8_t>((i * 7) & 0xff));
        }
        // NETSCAPE2.0: loop forever
        const uint8_t loop[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00};
        out.insert(out.end(), std::begin(loop), std::end(loop));
        for (size_t f = 0; f < delays.size(); f++) {
            const uint8_t gce[] = {0x21, 0xF9, 0x04, static_cast<uint8_t>((2 << 2) | 1), static_cast<uint8_t>(delays[f] & 0xff),
                                   static_cast<uint8_t>(delays[f] >> 8), 0x00, 0x00};
            out.insert(out.end(), std::begin(gce), std::end(gce));
            out.push_back(0x2C);
            u16(0);
            u16(0);
            u16(size);
            u16(size);
            out.push_back(0);
            out.push_back(8);   // LZW minimum code size
            std::vector<int> codes;
            codes.push_back(256);
            int run = 0;
            for (int y = 0; y < size; y++) {
                for (int x = 0; x < size; x++) {
                    const float dx = x + 0.5f - size * 0.5f, dy = y + 0.5f - size * 0.5f;
                    const float r = size * (0.3f + 0.05f * static_cast<float>(f));
                    codes.push_back(dx * dx + dy * dy <= r * r ? 40 + static_cast<int>(f) * 60 + x : 0);
                    if (++run == 250) {
                        codes.push_back(256);
                        run = 0;
                    }
                }
            }
            codes.push_back(257);
            std::vector<uint8_t> bytes;
            uint32_t acc = 0;
            int bits = 0;
            for (int code : codes) {
                acc |= static_cast<uint32_t>(code) << bits;
                bits += 9;
                while (bits >= 8) {
                    bytes.push_back(static_cast<uint8_t>(acc & 0xff));
                    acc >>= 8;
                    bits -= 8;
                }
            }
            if (bits > 0) bytes.push_back(static_cast<uint8_t>(acc & 0xff));
            for (size_t i = 0; i < bytes.size(); i += 255) {
                const size_t n = std::min<size_t>(255, bytes.size() - i);
                out.push_back(static_cast<uint8_t>(n));
                out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(i), bytes.begin() + static_cast<std::ptrdiff_t>(i + n));
            }
            out.push_back(0);
        }
        out.push_back(0x3B);
        return out;
    }

    // ---- the editor in a child process

    struct World {
        fs::path dir;
        Project* p = nullptr;
        uint32_t sceneId = 0;
        Entity hero = NULL_ENTITY;
        Entity walker = NULL_ENTITY;
        Entity light = NULL_ENTITY;
        editor::Platform* platform = nullptr;
        CommandHistory* scene() const { return CommandHandle::get(sceneId); }
        CommandHistory* projectHistory() const { return p->getProjectCommandHistory(); }
        Scene* sceneObject() const { return p->getScene(sceneId)->scene; }
        Material& material(Entity e) const { return sceneObject()->getComponent<MeshComponent>(e).submeshes[0].material; }
        fs::path file(const std::string& rel) const { return dir / fs::u8path(rel); }
    };

    Entity create(World& w, const std::string& name, EntityCreationType type) {
        auto* cmd = new CreateEntityCmd(w.p, w.sceneId, name, type);
        cmd->setQuiet(true);
        w.scene()->addCommandNoMerge(cmd);
        return cmd->getEntity();
    }

    Entity sprite(World& w, const std::string& name, const std::string& texture, int width, int height) {
        const Entity e = create(w, name, EntityCreationType::SPRITE);
        Scene* scene = w.sceneObject();
        Texture& base = scene->getComponent<MeshComponent>(e).submeshes[0].material.baseColorTexture;
        base = Texture(texture);
        base.setMinFilter(TextureFilter::NEAREST);
        base.setMagFilter(TextureFilter::NEAREST);
        base.setWrapU(TextureWrap::CLAMP_TO_EDGE);
        base.setWrapV(TextureWrap::CLAMP_TO_EDGE);
        SpriteComponent& s = scene->getComponent<SpriteComponent>(e);
        s.width = static_cast<unsigned>(width);
        s.height = static_cast<unsigned>(height);
        return e;
    }

    void runInEditor(const std::string& name, const std::function<void(World&)>& body, bool mcpServer = false) {
        const fs::path root = fs::temp_directory_path() / ("jasper-sn-window-" + name + "-" + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root / "config");
        setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
        World w;
        w.dir = root / "project";
        fs::create_directories(w.dir / ".jasperengine");
        std::ofstream(w.dir / "project.yaml") << "name: snwindow\n";
        writePng(w.dir / "assets/hero.png", disk(32, 32));
        writePng(w.dir / "assets/walk.png", disk(128, 32, 4));
        writeBytes(w.dir / "assets/blink.gif", gif(16, {10, 20, 30}));

        AppSettings::initialize();
        App& app = Backend::getApp();
        w.p = app.getProject();
        static editor::Platform* platform = nullptr;
        platform = new editor::Platform(w.p);
        // the test binary's engine is TestMain's (a second Engine::systemInit would restart its jobs
        // and logger): this System is swapped in
        jasperengine::SystemTestAccess::swap(platform);
        w.platform = platform;
        Engine::pauseGameEvents(true);
        Engine::setAsyncLoading(false);
        if (!w.p->loadProject(w.dir, false)) std::exit(90);
        Engine::setAsyncLoading(false);
        if (mcpServer) mcp::McpServer::get().start(app);
        else if (mcp::McpServer::get().isStarted()) std::exit(91);

        w.p->createNewScene("Normals", SceneType::SCENE_2D);
        w.sceneId = w.p->getSelectedSceneId();
        w.hero = sprite(w, "Hero", "assets/hero.png", 32, 32);
        w.walker = sprite(w, "Walker", "assets/walk.png", 32, 32);
        SpriteComponent& walk = w.sceneObject()->getComponent<SpriteComponent>(w.walker);
        for (int i = 0; i < 4; i++) {
            walk.framesRect[i].name = "frame_" + std::to_string(i);
            walk.framesRect[i].rect = Rect(static_cast<float>(i * 32), 0.0f, 32.0f, 32.0f);
        }
        walk.numFramesRect = 4;
        w.light = create(w, "Light 2D", EntityCreationType::POINT_LIGHT_2D);
        w.sceneObject()->getComponent<Light2DComponent>(w.light).height = 0.0f;
        w.p->clearSelectedEntities(w.sceneId);

        // the child's failures go to its stderr (what EXPECT_EXIT shows), and decide its exit code
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

// ... with the MCP server started
#define IN_EDITOR_WITH_MCP(name, ...)                                           \
    do {                                                                        \
        auto inEditor = [&](World& w) __VA_ARGS__;                              \
        GTEST_FLAG_SET(death_test_style, "threadsafe"); EXPECT_EXIT(runInEditor(name, inEditor, true), ::testing::ExitedWithCode(0), ""); \
    } while (0)

    // the window's runs (and Save All, Shift-click jobs) to their end; the debounce clock moved past
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

    const SpriteNormalsWindow::WarningRow* row(const SpriteNormalsWindow& window, const std::string& kind) {
        for (const SpriteNormalsWindow::WarningRow& r : window.getWarnings()) {
            if (r.kind == kind) return &r;
        }
        return nullptr;
    }

    double luminance(const sn::Image& image, int x0, int x1) {
        double sum = 0;
        int n = 0;
        for (int y = 0; y < image.height; y++) {
            for (int x = x0; x < x1; x++) {
                const uint8_t* p = image.pixel(x, y);
                if (p[3] == 0) continue;
                sum += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
                n++;
            }
        }
        return n ? sum / n : 0.0;
    }

}

// ==================================================================== 1-2 sources

TEST(SpriteNormalsWindowDeathTest, OpenSourcesSingle) {
    IN_EDITOR("single", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
        EXPECT_TRUE(window.isOpen());
        EXPECT_FALSE(window.isBatch());
        EXPECT_EQ(window.currentSpec().source, "assets/hero.png");
        const std::vector<sn::PlannedOutput> outputs = window.plannedOutputs();
        ASSERT_EQ(outputs.size(), 1u);
        EXPECT_EQ(outputs[0].path, "assets/hero_normal.png");
        EXPECT_EQ(window.sourceSummary(), "32 x 32 PNG");
        EXPECT_EQ(window.framesSummary(), "One image (no frames)");
        EXPECT_EQ(window.currentSpec().frames.mode, sn::FramesSpec::Mode::Auto);
        EXPECT_FALSE(window.hasAssignTarget());
        // not an image: refused with the reason
        EXPECT_FALSE(window.openSources({w.file("project.yaml")}, error));
        EXPECT_NE(error.find("not a PNG"), std::string::npos) << error;
    });
}

TEST(SpriteNormalsWindowDeathTest, BatchSaveAllIsOneUndoStep) {
    IN_EDITOR("batch", {
        writePng(w.file("assets/a.png"), disk(16, 16));
        writePng(w.file("assets/b.png"), disk(24, 24));
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png"), w.file("assets/a.png"), w.file("assets/b.png")}, error)) << error;
        EXPECT_TRUE(window.isBatch());
        EXPECT_EQ(window.itemCount(), 3u);
        EXPECT_EQ(window.currentItem(), 0u);
        for (size_t i = 0; i < 3; i++) EXPECT_TRUE(window.isItemChecked(i));
        const size_t before = w.projectHistory()->position();
        std::vector<std::string> written;
        ASSERT_TRUE(window.save(false, written, error)) << error;
        ASSERT_TRUE(settle(window));
        for (const char* name : {"assets/hero_normal.png", "assets/a_normal.png", "assets/b_normal.png"}) {
            EXPECT_TRUE(fs::exists(w.file(name))) << name;
            EXPECT_TRUE(sn::readRecipeHead(w.file(name)).has_value()) << name;
        }
        EXPECT_EQ(w.projectHistory()->position(), before + 1) << "Save All is one project undo step";
        EXPECT_EQ(window.getStatus(), "Saved 3 files as one undo step.");
        ASSERT_TRUE(w.projectHistory()->undo());
        for (const char* name : {"assets/hero_normal.png", "assets/a_normal.png", "assets/b_normal.png"}) EXPECT_FALSE(fs::exists(w.file(name))) << name;
        // an unchecked item is not written; existing files need overwrite
        ASSERT_TRUE(w.projectHistory()->redo());
        window.setItemChecked(1, false);
        EXPECT_FALSE(window.save(false, written, error));
        EXPECT_NE(error.find("exist"), std::string::npos) << error;
    });
}

// ==================================================================== 3-5 settings and live runs

TEST(SpriteNormalsWindowDeathTest, PresetsAndOverrides) {
    IN_EDITOR("presets", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
        sn::Settings s = window.getSettings();
        s.softness = 3.0f;
        s.radius = 9.0f;
        s.alphaThreshold = 0.2f;
        window.setSettings(s);
        window.setPreset(sn::Preset::Crisp);
        const sn::Settings crisp = sn::presetSettings(sn::Preset::Crisp);
        EXPECT_EQ(window.getSettings().preset, sn::Preset::Crisp);
        EXPECT_FLOAT_EQ(window.getSettings().strength, crisp.strength);
        EXPECT_FLOAT_EQ(window.getSettings().detail, crisp.detail);
        EXPECT_FLOAT_EQ(window.getSettings().smoothing, 0.6f);
        EXPECT_FLOAT_EQ(window.getSettings().volume, crisp.volume);
        EXPECT_FLOAT_EQ(window.getSettings().softness, 1.15f);
        EXPECT_FALSE(window.getSettings().radius.has_value()) << "radius back to auto";
        EXPECT_FLOAT_EQ(window.getSettings().alphaThreshold, 0.2f) << "not a preset value";
        // an edit is an override, the preset stays lit; another preset clears it
        s = window.getSettings();
        s.detail = 5.0f;
        window.setSettings(s);
        EXPECT_EQ(window.getSettings().preset, sn::Preset::Crisp);
        EXPECT_FLOAT_EQ(window.getSettings().detail, 5.0f);
        window.setPreset(sn::Preset::Chunky);
        EXPECT_FLOAT_EQ(window.getSettings().detail, 4.0f);
        EXPECT_FLOAT_EQ(window.getSettings().volume, 2.0f);
        // the last settings are the project's (an Inspector Shift-click uses them)
        EXPECT_EQ(w.p->getSpriteNormalsSettings().settings.preset, sn::Preset::Chunky);
    });
}

// High-resolution presets (2026-10-30-sprite-normals-hires-design.md): a preset sets its profile,
// tile turns edges to wrap and leaving it turns them back, any preset starts with the current
// generator (a map made before keeps version 1 until then); the result says what auto resolved to
TEST(SpriteNormalsWindowDeathTest, HighResolutionPresetsProfileScaleAndVersion) {
    IN_EDITOR("hires-presets", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
        window.setPreset(sn::Preset::Painted);
        EXPECT_EQ(window.getSettings().profile, sn::Profile::Inflate);
        EXPECT_EQ(window.getSettings().edges, sn::Edges::Transparent);
        window.setPreset(sn::Preset::Props);
        EXPECT_EQ(window.getSettings().profile, sn::Profile::Dome);
        EXPECT_FLOAT_EQ(window.getSettings().volume, 0.7f);
        EXPECT_FLOAT_EQ(window.getSettings().softness, 1.0f);
        window.setPreset(sn::Preset::Tile);
        EXPECT_EQ(window.getSettings().edges, sn::Edges::Wrap);
        EXPECT_EQ(window.getSettings().profile, sn::Profile::Auto);
        window.setPreset(sn::Preset::Default);
        EXPECT_EQ(window.getSettings().edges, sn::Edges::Transparent) << "leaving tile";
        // edges set by hand survive a preset other than tile
        sn::Settings s = window.getSettings();
        s.edges = sn::Edges::Extend;
        s.resolutionScale = sn::ResolutionScale::Off;
        s.softEdge = sn::SoftEdge::Solid;
        s.version = 1;
        s.smoothing = 24.0f;   // within the new range
        window.setSettings(s);
        EXPECT_TRUE(sn::validateSettings(window.getSettings()).empty());
        window.setPreset(sn::Preset::Crisp);
        EXPECT_EQ(window.getSettings().edges, sn::Edges::Extend);
        EXPECT_EQ(window.getSettings().resolutionScale, sn::ResolutionScale::Off) << "not a preset value";
        EXPECT_EQ(window.getSettings().softEdge, sn::SoftEdge::Solid) << "not a preset value";
        EXPECT_EQ(window.getSettings().version, sn::kSettingsVersion) << "a preset starts with the current generator";
        // the stored settings keep the new values (project.yaml)
        EXPECT_EQ(w.p->getSpriteNormalsSettings().settings.resolutionScale, sn::ResolutionScale::Off);
        // the controls draw (Advanced open: profile, resolution scale, the version note; nine presets)
        ImGuiContext* context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400, 900);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        window.open();
        Access::setAdvancedOpen(window, true);
        s = window.getSettings();
        s.version = 1;
        s.profile = sn::Profile::Inflate;
        window.setSettings(s);
        ASSERT_TRUE(settle(window));
        Access::drawFrame(window);
        EXPECT_EQ(window.getSettings().version, 1) << "drawing changes nothing";
        ImGui::DestroyContext(context);
    });
}

// Toon steps: the preset and the control's value change the live result (every opaque pixel one of
// the palette's), another preset turns them off, and a saved map's recipe brings them back
TEST(SpriteNormalsWindowDeathTest, ToonStepsChangeTheResultAndTheRecipeRestoresThem) {
    IN_EDITOR("toon", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
        ASSERT_TRUE(settle(window));
        ASSERT_TRUE(Access::shown(window));
        const sn::Image smooth = Access::shown(window)->result().normal;
        auto distinct = [](const sn::Image& image) {
            std::set<uint32_t> colours;
            for (size_t i = 0; i < image.pixelCount(); i++) {
                const uint8_t* p = image.data.data() + 4 * i;
                if (p[3] > 0) colours.insert((uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]);
            }
            return colours.size();
        };
        EXPECT_GT(distinct(smooth), 40u) << "smooth normals";

        window.setPreset(sn::Preset::Toon);
        EXPECT_EQ(window.getSettings().quantize, sn::presetSettings(sn::Preset::Toon).quantize);
        ASSERT_TRUE(settle(window));
        const sn::Image toon = Access::shown(window)->result().normal;
        EXPECT_NE(toon.data, smooth.data);
        const sn::Quantize& q = window.getSettings().quantize;
        EXPECT_LE(distinct(toon), static_cast<size_t>(1 + q.directions * q.tilts));

        // the control's value (what a click on "4" sets), with the Advanced values drawn
        sn::Settings s = window.getSettings();
        s.quantize.directions = 4;
        s.quantize.tilts = 1;
        window.setSettings(s);
        ASSERT_TRUE(settle(window));
        EXPECT_LE(distinct(Access::shown(window)->result().normal), 5u);
        {
            ImGuiContext* context = ImGui::CreateContext();
            ImGui::SetCurrentContext(context);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(1400, 900);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char* pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            window.open();
            Access::setAdvancedOpen(window, true);
            Access::drawFrame(window);
            s.quantize.directions = 12;   // no segmented button: the Advanced combo's
            window.setSettings(s);
            Access::drawFrame(window);
            ImGui::DestroyContext(context);
        }
        EXPECT_EQ(w.p->getSpriteNormalsSettings().settings.quantize.directions, 12) << "the project's last settings";

        // another preset: off
        window.setPreset(sn::Preset::Crisp);
        EXPECT_FALSE(window.getSettings().quantize.on());

        // saved with Toon steps on a non-toon preset, opened again from the recipe: the same steps and pixels
        s = window.getSettings();
        s.quantize.directions = 6;
        s.quantize.tilts = 3;
        s.quantize.flatDeg = 15.0f;
        s.quantize.rotationDeg = 30.0f;
        window.setSettings(s);
        window.setOutput("assets/maps/hero_toon.png");
        ASSERT_TRUE(settle(window));
        const sn::Image saved = Access::shown(window)->result().normal;
        std::vector<std::string> written;
        ASSERT_TRUE(window.save(false, written, error)) << error;
        ASSERT_TRUE(settle(window));
        const std::optional<nlohmann::json> recipe = sn::readRecipeHead(w.file("assets/maps/hero_toon.png"));
        ASSERT_TRUE(recipe.has_value());
        EXPECT_EQ((*recipe)["settings"]["quantize"].dump(), R"({"directions":6,"flat_deg":15,"rotation_deg":30,"tilts":3})");
        {
            SpriteNormalsWindow again(w.p);
            ASSERT_TRUE(again.openRecipe(w.file("assets/maps/hero_toon.png"), error)) << error;
            EXPECT_EQ(again.getSettings().preset, sn::Preset::Crisp);
            EXPECT_EQ(again.getSettings().quantize, s.quantize);
            ASSERT_TRUE(settle(again));
            EXPECT_EQ(Access::shown(again)->result().normal.data, saved.data) << "regenerating gives the same pixels";
        }
        ts::Image file;
        ASSERT_TRUE(ts::loadImage(w.file("assets/maps/hero_toon.png"), file, error)) << error;
        EXPECT_EQ(ts::toRgba8(file).data, saved.data);

        // the toon preset turned off: recorded as off, restored as off
        window.setPreset(sn::Preset::Toon);
        s = window.getSettings();
        s.quantize.directions = 0;
        window.setSettings(s);
        window.setOutput("assets/maps/hero_off.png");
        ASSERT_TRUE(settle(window));
        ASSERT_TRUE(window.save(false, written, error)) << error;
        ASSERT_TRUE(settle(window));
        SpriteNormalsWindow off(w.p);
        ASSERT_TRUE(off.openRecipe(w.file("assets/maps/hero_off.png"), error)) << error;
        EXPECT_EQ(off.getSettings().preset, sn::Preset::Toon);
        EXPECT_FALSE(off.getSettings().quantize.on());
    });
}

TEST(SpriteNormalsWindowDeathTest, DebounceStartsOneRun) {
    IN_EDITOR("debounce", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
        Access::tick(window);
        EXPECT_EQ(Access::worker(window), nullptr) << "no run before 150 ms";
        Access::advance(window, 0.1);
        EXPECT_EQ(Access::worker(window), nullptr);
        Access::advance(window, 0.06);
        ASSERT_NE(Access::worker(window), nullptr) << "the run starts 150 ms after the last change";
        ASSERT_TRUE(settle(window));
        EXPECT_TRUE(window.hasResult());
        EXPECT_FALSE(window.isStale());
        // two changes 0.1 s apart: one run, 150 ms after the second
        sn::Settings s = window.getSettings();
        s.strength = 2.0f;
        window.setSettings(s);
        Access::advance(window, 0.1);
        s.strength = 2.5f;
        window.setSettings(s);
        EXPECT_TRUE(window.isStale());
        Access::advance(window, 0.1);
        EXPECT_EQ(Access::worker(window), nullptr) << "the second change restarted the wait";
        Access::advance(window, 0.06);
        const std::shared_ptr<sn::SpriteNormalsRun> run = Access::worker(window);
        ASSERT_NE(run, nullptr);
        EXPECT_FLOAT_EQ(run->prepared().request.settings.strength, 2.5f);
        ASSERT_TRUE(settle(window));
        EXPECT_FALSE(window.isStale());
    });
}

TEST(SpriteNormalsWindowDeathTest, LargeSourceIsNotLive) {
    IN_EDITOR("large", {
        writePng(w.file("assets/big.png"), disk(1100, 1000));
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openSources({w.file("assets/big.png")}, error)) << error;
        Access::advance(window, 0.5);
        EXPECT_EQ(Access::worker(window), nullptr) << "over 1,048,576 pixels: no live run";
        EXPECT_FALSE(Access::debouncePending(window));
        ASSERT_TRUE(window.regenerate(error)) << error;
        EXPECT_NE(Access::worker(window), nullptr);
        ASSERT_TRUE(settle(window, 60000));
        EXPECT_TRUE(window.hasResult());
    });
}

// ==================================================================== 6-9 save

TEST(SpriteNormalsWindowDeathTest, SaveWritesOneStepAndAsksBeforeReplacing) {
    IN_EDITOR("save", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        std::vector<std::string> written;
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
        EXPECT_FALSE(window.save(false, written, error)) << "no result yet";
        EXPECT_EQ(error, "Nothing to save: wait for the preview.");
        ASSERT_TRUE(settle(window));
        const size_t before = w.projectHistory()->position();
        ASSERT_TRUE(window.save(false, written, error)) << error;
        ASSERT_EQ(written, std::vector<std::string>{"assets/hero_normal.png"});
        EXPECT_EQ(w.projectHistory()->position(), before + 1);
        EXPECT_EQ(window.getStatus(), "Saved hero_normal.png (Project undo).");
        const std::string first = readAll(w.file("assets/hero_normal.png"));
        EXPECT_TRUE(sn::readRecipe(w.file("assets/hero_normal.png")).has_value());
        // the shown result's bytes: the Normal view shows exactly what was written
        sn::Source decoded;
        std::vector<sn::Problem> problems;
        ASSERT_TRUE(sn::loadSource(w.file("assets/hero_normal.png"), decoded, problems));
        EXPECT_EQ(decoded.color.data, Access::shown(window)->result().normal.data);
        // again: refused without overwrite; the Replace Files? confirm overwrites
        EXPECT_FALSE(window.save(false, written, error));
        EXPECT_NE(error.find("already exists"), std::string::npos) << error;
        Access::refreshWarningsNow(window);
        ASSERT_NE(row(window, "output_exists"), nullptr);
        EXPECT_EQ(row(window, "output_exists")->text, "hero_normal.png exists: Save replaces it (undo puts it back).");
        sn::Settings s = window.getSettings();
        s.detail = 7.0f;
        window.setSettings(s);
        ASSERT_TRUE(settle(window));
        Access::holdConfirms(window, true);
        Access::requestSave(window);
        ASSERT_TRUE(Access::lastConfirm(window)) << "Save asks before replacing";
        EXPECT_EQ(readAll(w.file("assets/hero_normal.png")), first) << "nothing written before the answer";
        Access::lastConfirm(window)();
        const std::string second = readAll(w.file("assets/hero_normal.png"));
        EXPECT_NE(second, first);
        EXPECT_EQ(w.projectHistory()->position(), before + 2);
        ASSERT_TRUE(w.projectHistory()->undo());
        EXPECT_EQ(readAll(w.file("assets/hero_normal.png")), first) << "project undo puts the previous bytes back";
    });
}

TEST(SpriteNormalsWindowDeathTest, SaveAndAssignIsOneSceneStep) {
    IN_EDITOR("assign", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        std::vector<std::string> written;
        ASSERT_TRUE(window.openForEntity(w.sceneId, w.hero, 0, false, error)) << error;
        EXPECT_TRUE(window.getAssign()) << "opened from the Inspector: Assign on";
        ASSERT_TRUE(settle(window));
        const size_t sceneBefore = w.scene()->position();
        const size_t projectBefore = w.projectHistory()->position();
        ASSERT_TRUE(window.save(false, written, error)) << error;
        EXPECT_EQ(window.getStatus(), "Saved hero_normal.png (Project undo) and assigned to Hero (Scene undo).");
        const Texture& normal = w.material(w.hero).normalTexture;
        EXPECT_EQ(normal.getPath(), "assets/hero_normal.png");
        EXPECT_EQ(normal.getMinFilter(), TextureFilter::NEAREST);
        EXPECT_EQ(normal.getMagFilter(), TextureFilter::NEAREST);
        EXPECT_EQ(normal.getWrapU(), TextureWrap::CLAMP_TO_EDGE);
        EXPECT_EQ(w.scene()->position(), sceneBefore + 1);
        EXPECT_EQ(w.projectHistory()->position(), projectBefore + 1);
        // scene undo clears it; the file stays until the project undo
        ASSERT_TRUE(w.scene()->undo());
        EXPECT_TRUE(w.material(w.hero).normalTexture.getPath().empty());
        EXPECT_TRUE(fs::exists(w.file("assets/hero_normal.png")));
        ASSERT_TRUE(w.projectHistory()->undo());
        EXPECT_FALSE(fs::exists(w.file("assets/hero_normal.png")));
        // while playing, Save & Assign is refused
        ASSERT_TRUE(w.projectHistory()->redo());
        w.p->getScene(w.sceneId)->playState = ScenePlayState::PLAYING;
        EXPECT_FALSE(window.save(true, written, error));
        EXPECT_EQ(error, "Stop the scene to assign the normal map");
        w.p->getScene(w.sceneId)->playState = ScenePlayState::STOPPED;
    });
}

TEST(SpriteNormalsWindowDeathTest, UndoRouting) {
    IN_EDITOR("undo", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        std::vector<std::string> written;
        uint32_t sceneId = 0;
        EXPECT_FALSE(window.undoGoesToScene(sceneId)) << "nothing assigned yet: the project";
        ASSERT_TRUE(window.openForEntity(w.sceneId, w.hero, 0, false, error)) << error;
        ASSERT_TRUE(settle(window));
        ASSERT_TRUE(window.save(false, written, error)) << error;
        ASSERT_TRUE(window.undoGoesToScene(sceneId)) << "the assign is the scene's newest step";
        EXPECT_EQ(sceneId, w.sceneId);
        // undone: the next undo is the project's (the write); redo comes back to the scene
        ASSERT_TRUE(w.scene()->undo());
        EXPECT_FALSE(window.undoGoesToScene(sceneId));
        ASSERT_TRUE(window.redoGoesToScene(sceneId));
        ASSERT_TRUE(w.scene()->redo());
        EXPECT_TRUE(window.undoGoesToScene(sceneId));
        // a further scene edit elsewhere: the project again
        auto* move = new PropertyCmd<Vector3>(w.p, w.sceneId, w.walker, ComponentType::Transform, "position", Vector3(5, 5, 0));
        move->setNoMerge();
        w.scene()->addCommand(move);
        EXPECT_FALSE(window.undoGoesToScene(sceneId));
        EXPECT_FALSE(window.redoGoesToScene(sceneId));
    });
}

TEST(SpriteNormalsWindowDeathTest, GifSaveAndAssignMakesTheAnimation) {
    IN_EDITOR("gif", {
        const Entity blinker = sprite(w, "Blinker", "assets/hero.png", 16, 16);
        SpriteNormalsWindow window(w.p);
        std::string error;
        std::vector<std::string> written;
        ASSERT_TRUE(window.openSources({w.file("assets/blink.gif")}, error)) << error;
        EXPECT_EQ(window.sourceSummary().rfind("3-frame GIF, 16 x 16", 0), 0u) << window.sourceSummary();
        EXPECT_EQ(window.framesSummary(), "3 frames from the GIF");
        window.setAssignTarget(w.sceneId, blinker, 0);
        window.setAssign(true);
        ASSERT_TRUE(settle(window));
        ASSERT_TRUE(window.hasResult());
        const size_t sceneBefore = w.scene()->position();
        ASSERT_TRUE(window.save(false, written, error)) << error;
        EXPECT_TRUE(fs::exists(w.file("assets/blink_sheet.png")));
        EXPECT_TRUE(fs::exists(w.file("assets/blink_sheet_normal.png")));
        EXPECT_EQ(w.material(blinker).baseColorTexture.getPath(), "assets/blink_sheet.png");
        EXPECT_EQ(w.material(blinker).normalTexture.getPath(), "assets/blink_sheet_normal.png");
        EXPECT_EQ(w.sceneObject()->getComponent<SpriteComponent>(blinker).numFramesRect, 3u);
        EXPECT_EQ(w.scene()->position(), sceneBefore + 1) << "one scene undo step";
        // the Sprite Animation with the GIF's delays
        Scene* scene = w.sceneObject();
        auto animations = scene->getComponentArray<SpriteAnimationComponent>();
        bool found = false;
        for (size_t i = 0; animations && i < animations->size(); i++) {
            const Entity e = animations->getEntity(i);
            if (scene->getComponent<ActionComponent>(e).target != blinker) continue;
            const SpriteAnimationComponent& anim = scene->getComponent<SpriteAnimationComponent>(e);
            ASSERT_EQ(anim.framesTimeSize, 3u);
            EXPECT_EQ(anim.framesTime[0], 100);
            EXPECT_EQ(anim.framesTime[1], 200);
            EXPECT_EQ(anim.framesTime[2], 300);
            found = true;
        }
        EXPECT_TRUE(found);
        ASSERT_TRUE(w.scene()->undo());
        EXPECT_EQ(w.material(blinker).baseColorTexture.getPath(), "assets/hero.png");
    });
}

// ==================================================================== 10-13 entry points

TEST(SpriteNormalsWindowDeathTest, OpenForEntity) {
    IN_EDITOR("entity", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openForEntity(w.sceneId, w.walker, 0, false, error)) << error;
        EXPECT_TRUE(window.isOpen());
        EXPECT_TRUE(window.getAssign());
        EXPECT_EQ(window.framesSummary(), "4 frames 32 x 32 from Sprite \"Walker\"");
        EXPECT_EQ(window.sourceSummary(), "128 x 32 PNG, 4 frames 32 x 32 (from Sprite \"Walker\")");
        ASSERT_TRUE(settle(window));
        ASSERT_TRUE(window.hasResult());
        EXPECT_EQ(Access::shown(window)->prepared().request.cells.size(), 4u);
        window.setOpen(false);
        // now: written and assigned with the window closed
        SpriteNormalsWindow quick(w.p);
        ASSERT_TRUE(quick.openForEntity(w.sceneId, w.walker, 0, true, error)) << error;
        EXPECT_FALSE(quick.isOpen());
        ASSERT_TRUE(settle(quick));
        EXPECT_TRUE(fs::exists(w.file("assets/walk_normal.png")));
        EXPECT_EQ(w.material(w.walker).normalTexture.getPath(), "assets/walk_normal.png");
        EXPECT_FALSE(quick.isOpen());
    });
}

TEST(SpriteNormalsWindowDeathTest, ShiftClickRegeneratesOrOpensTheWindow) {
    IN_EDITOR("shift", {
        std::string error;
        std::vector<std::string> written;
        // Hero's map generated with Detailed, then the last settings changed to Soft
        {
            SpriteNormalsWindow window(w.p);
            ASSERT_TRUE(window.openForEntity(w.sceneId, w.hero, 0, false, error)) << error;
            window.setPreset(sn::Preset::Detailed);
            ASSERT_TRUE(settle(window));
            ASSERT_TRUE(window.save(false, written, error)) << error;
            window.setPreset(sn::Preset::Soft);
        }
        EXPECT_EQ(w.p->getSpriteNormalsSettings().settings.preset, sn::Preset::Soft);
        writeBytes(w.file("assets/other_normal.png"), {1, 2, 3});
        const std::string otherBefore = readAll(w.file("assets/other_normal.png"));
        // modify the source: the regeneration differs, with the recipe's settings (two disks: the
        // same image again would write the same bytes, a no-op step oe-port's history drops)
        writePng(w.file("assets/hero.png"), disk(32, 32, 2));
        SpriteNormalsWindow quick(w.p);
        const size_t before = w.projectHistory()->position();
        ASSERT_TRUE(quick.openForEntity(w.sceneId, w.hero, 0, true, error)) << error;
        EXPECT_FALSE(quick.isOpen());
        ASSERT_TRUE(settle(quick));
        EXPECT_EQ(w.projectHistory()->position(), before + 1);
        const std::optional<nlohmann::json> recipe = sn::readRecipeHead(w.file("assets/hero_normal.png"));
        ASSERT_TRUE(recipe.has_value());
        EXPECT_EQ((*recipe)["settings"]["preset"], "detailed") << "the recipe's settings, not the last ones";
        EXPECT_EQ(readAll(w.file("assets/other_normal.png")), otherBefore) << "only the file the recipe names";
        // a non-generated file at the default path: the window opens, nothing is written
        writeBytes(w.file("assets/walk_normal.png"), {9, 9, 9});
        SpriteNormalsWindow walker(w.p);
        ASSERT_TRUE(walker.openForEntity(w.sceneId, w.walker, 0, true, error)) << error;
        EXPECT_TRUE(walker.isOpen());
        ASSERT_TRUE(settle(walker));
        EXPECT_EQ(readAll(w.file("assets/walk_normal.png")), std::string("\x09\x09\x09"));
        ASSERT_FALSE(walker.getNotices().empty());
        EXPECT_NE(walker.getNotices()[0].find("was not generated"), std::string::npos) << walker.getNotices()[0];
        Access::holdConfirms(walker, true);
        Access::requestSave(walker);
        EXPECT_TRUE(Access::lastConfirm(walker)) << "Save asks to replace it";
        EXPECT_EQ(readAll(w.file("assets/walk_normal.png")), std::string("\x09\x09\x09"));
    });
}

TEST(SpriteNormalsWindowDeathTest, OpenRecipeRestores) {
    IN_EDITOR("recipe", {
        std::string error;
        std::vector<std::string> written;
        {
            SpriteNormalsWindow window(w.p);
            ASSERT_TRUE(window.openSources({w.file("assets/walk.png")}, error)) << error;
            sn::FramesSpec frames;
            frames.mode = sn::FramesSpec::Mode::Grid;
            frames.grid.tileWidth = 32;
            frames.grid.tileHeight = 32;
            window.setFrames(frames);
            sn::Settings s = sn::presetSettings(sn::Preset::Crisp);
            s.preset = sn::Preset::Crisp;
            s.strength = 3.0f;
            s.profile = sn::Profile::Dome;
            window.setSettings(s);
            window.setOutput("assets/maps/walk_n.png");
            ASSERT_TRUE(settle(window));
            ASSERT_TRUE(window.save(false, written, error)) << error;
        }
        SpriteNormalsWindow window(w.p);
        ASSERT_TRUE(window.openRecipe(w.file("assets/maps/walk_n.png"), error)) << error;
        EXPECT_EQ(window.getSettings().preset, sn::Preset::Crisp);
        EXPECT_FLOAT_EQ(window.getSettings().strength, 3.0f);
        EXPECT_EQ(window.getSettings().profile, sn::Profile::Dome);
        EXPECT_EQ(window.currentSpec().frames.cells.size(), 4u);
        EXPECT_EQ(window.currentSpec().output.normal, "assets/maps/walk_n.png");
        ASSERT_FALSE(window.getNotices().empty());
        EXPECT_NE(window.getNotices().back().find("its source is unchanged"), std::string::npos) << window.getNotices().back();
        writePng(w.file("assets/walk.png"), disk(128, 32, 2));
        ASSERT_TRUE(window.openRecipe(w.file("assets/maps/walk_n.png"), error)) << error;
        EXPECT_EQ(window.getNotices().back(), "walk.png changed since this normal map was generated.");
        // no recipe
        EXPECT_FALSE(window.openRecipe(w.file("assets/hero.png"), error));
    });
}

TEST(SpriteNormalsWindowDeathTest, RegenerateFromRecipe) {
    IN_EDITOR("regenerate", {
        std::string error;
        std::vector<std::string> written;
        {
            SpriteNormalsWindow window(w.p);
            ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
            ASSERT_TRUE(settle(window));
            ASSERT_TRUE(window.save(false, written, error)) << error;
        }
        const std::string first = readAll(w.file("assets/hero_normal.png"));
        writePng(w.file("assets/hero.png"), disk(32, 32, 1));
        // the same image re-encoded is not different: make it so
        ts::Image changed = disk(32, 32);
        changed.pixel(16, 16)[0] = 3;
        writePng(w.file("assets/hero.png"), changed);
        const size_t before = w.projectHistory()->position();
        ASSERT_TRUE(SpriteNormalsWindow::regenerateFromRecipe(w.p, w.file("assets/hero_normal.png"), written, error)) << error;
        EXPECT_EQ(written, std::vector<std::string>{"assets/hero_normal.png"});
        EXPECT_EQ(w.projectHistory()->position(), before + 1);
        EXPECT_NE(readAll(w.file("assets/hero_normal.png")), first);
        EXPECT_EQ(sn::checkStaleness(w.dir, w.file("assets/hero_normal.png")).state, sn::Staleness::State::Fresh);
        fs::remove(w.file("assets/hero.png"));
        EXPECT_FALSE(SpriteNormalsWindow::regenerateFromRecipe(w.p, w.file("assets/hero_normal.png"), written, error));
        EXPECT_NE(error.find("missing"), std::string::npos) << error;
    });
}

// ==================================================================== 14-17 preview

TEST(SpriteNormalsWindowDeathTest, Eyedropper) {
    IN_EDITOR("eyedropper", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
        ASSERT_TRUE(window.pickRegionColour(-1, 16, 16, error)) << error;
        ASSERT_EQ(window.getSettings().regions.size(), 1u) << "Alt-click with no region makes region 1";
        const ts::Image hero = disk(32, 32);
        const uint8_t* p = hero.pixel(16, 16);
        EXPECT_EQ(window.getSettings().regions[0].colors[0], (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]);
        EXPECT_FALSE(window.pickRegionColour(0, 0, 0, error));
        EXPECT_EQ(error, "That pixel is empty.");
        sn::Settings s = window.getSettings();
        s.regions[0].colors.clear();
        for (uint32_t i = 0; i < 16; i++) s.regions[0].colors.push_back(0x101010 + i);
        window.setSettings(s);
        EXPECT_FALSE(window.pickRegionColour(0, 10, 16, error));
        EXPECT_EQ(error, "At most 16 colours per region.");
    });
}

TEST(SpriteNormalsWindowDeathTest, LitPreviewFollowsTheLight) {
    IN_EDITOR("lit", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
        // flat paint: only the silhouette's rounding shades it
        sn::Settings s = window.getSettings();
        s.detail = 0.0f;
        window.setSettings(s);
        ASSERT_TRUE(settle(window));
        float x, y, height;
        Access::light(window, x, y, height);
        EXPECT_FLOAT_EQ(x, 0.0f) << "default: up-left of the centre by half the side";
        EXPECT_FLOAT_EQ(y, 0.0f);
        EXPECT_NEAR(height, 19.2f, 1e-3f);
        window.setLight(-20.0f, 16.0f, 20.0f);
        const sn::Image left = Access::lit(window);
        ASSERT_EQ(left.width, 32);
        EXPECT_GT(luminance(left, 0, 16), luminance(left, 16, 32) + 5.0);
        window.setLight(52.0f, 16.0f, 20.0f);
        const sn::Image right = Access::lit(window);
        EXPECT_GT(luminance(right, 16, 32), luminance(right, 0, 16) + 5.0);
        EXPECT_FALSE(Access::gpuValid(window)) << "no GPU objects headless";
    });
}

TEST(SpriteNormalsWindowDeathTest, PlayStepsTheGifFrames) {
    IN_EDITOR("play", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openSources({w.file("assets/blink.gif")}, error)) << error;
        ASSERT_TRUE(settle(window));
        ASSERT_TRUE(window.hasResult());
        window.setPlaying(true);
        EXPECT_EQ(window.getFrame(), 0);
        Access::advance(window, 0.05);
        EXPECT_EQ(window.getFrame(), 0);
        Access::advance(window, 0.05);
        EXPECT_EQ(window.getFrame(), 1) << "frame 0 lasts 100 ms";
        Access::advance(window, 0.2);
        EXPECT_EQ(window.getFrame(), 2) << "frame 1 lasts 200 ms";
        Access::advance(window, 0.3);
        EXPECT_EQ(window.getFrame(), 0) << "frame 2 lasts 300 ms, then it loops";
        window.setPlaying(false);
        window.setFrame(-1);
        EXPECT_EQ(window.getFrame(), -1);
    });
}

// ==================================================================== 15, 18-19 warnings, drawing, project switch

TEST(SpriteNormalsWindowDeathTest, WarningsAndFixes) {
    IN_EDITOR("warnings", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openForEntity(w.sceneId, w.hero, 0, false, error)) << error;
        ASSERT_TRUE(settle(window));
        Access::refreshWarningsNow(window);
        auto same = [&](const std::string& kind) {
            const SpriteNormalsWindow::WarningRow* r = row(window, kind);
            if (!r) return false;
            for (const sn::Problem& p : sn::sceneWarnings(w.sceneObject(), w.hero, window.getSettings(), 32, 32, {}, window.getAssign())) {
                if (p.kind == kind) return p.message == r->text;
            }
            return false;
        };
        ASSERT_NE(row(window, "no_light_height"), nullptr);
        EXPECT_TRUE(same("no_light_height")) << "Phase 2's message verbatim";
        size_t before = w.scene()->position();
        ASSERT_TRUE(window.applyFix("no_light_height", "Set Height 64", error)) << error;
        EXPECT_FLOAT_EQ(w.sceneObject()->getComponent<Light2DComponent>(w.light).height, 64.0f);
        EXPECT_EQ(w.scene()->position(), before + 1);
        EXPECT_EQ(row(window, "no_light_height"), nullptr);
        // no light: Create Light 2D (height 64)
        w.sceneObject()->destroyEntity(w.light);
        Access::refreshWarningsNow(window);
        ASSERT_NE(row(window, "no_light"), nullptr);
        EXPECT_TRUE(same("no_light"));
        before = w.scene()->position();
        ASSERT_TRUE(window.applyFix("no_light", "Create Light 2D", error)) << error;
        EXPECT_EQ(w.scene()->position(), before + 1);
        const std::vector<Entity> selected = w.p->getSelectedEntities(w.sceneId);
        ASSERT_EQ(selected.size(), 1u) << "the new light is selected";
        EXPECT_FLOAT_EQ(w.sceneObject()->getComponent<Light2DComponent>(selected[0]).height, 64.0f);
        EXPECT_EQ(row(window, "no_light"), nullptr);
        // directx with a target
        sn::Settings s = window.getSettings();
        s.directx = true;
        window.setSettings(s);
        Access::refreshWarningsNow(window);
        ASSERT_NE(row(window, "directx_in_engine"), nullptr);
        EXPECT_TRUE(same("directx_in_engine"));
        ASSERT_TRUE(window.applyFix("directx_in_engine", "Switch to OpenGL", error)) << error;
        EXPECT_FALSE(window.getSettings().directx);
        // filter mismatch: a normal texture sampled differently, without assigning
        Texture normal("assets/hero.png");
        normal.setMinFilter(TextureFilter::LINEAR);
        w.material(w.hero).normalTexture = normal;
        window.setAssign(false);
        Access::refreshWarningsNow(window);
        ASSERT_NE(row(window, "filter_mismatch"), nullptr);
        EXPECT_TRUE(same("filter_mismatch"));
        ASSERT_TRUE(window.applyFix("filter_mismatch", "Match Base Texture", error)) << error;
        EXPECT_EQ(w.material(w.hero).normalTexture.getMinFilter(), TextureFilter::NEAREST);
        // size mismatch: another image's map for Hero
        writePng(w.file("assets/wide.png"), disk(48, 32));
        SpriteNormalsWindow other(w.p);
        ASSERT_TRUE(other.openSources({w.file("assets/wide.png")}, error)) << error;
        other.setAssignTarget(w.sceneId, w.hero, 0);
        ASSERT_TRUE(settle(other));
        Access::refreshWarningsNow(other);
        const SpriteNormalsWindow::WarningRow* size = row(other, "size_mismatch");
        ASSERT_NE(size, nullptr);
        EXPECT_TRUE(size->error);
        ASSERT_TRUE(other.applyFix("size_mismatch", "Use Base Texture", error)) << error;
        EXPECT_TRUE(other.currentSpec().hasSprite());
        EXPECT_FALSE(other.applyFix("size_mismatch", "Nonsense", error));
    });
}

// S78: the Stones preset and Dark Gaps Are Edges, the boxy and gaps_unmatched rows with their
// fixes in the window's words, and the lit preview's Light Steps (the runtime's formula)
TEST(SpriteNormalsWindowDeathTest, StonesGapsBoxyAndLightSteps) {
    IN_EDITOR("stones", {
        // a crate (a 20 px box on transparency) and a flat opaque tile
        ts::Image crate = ts::Image::make(32, 32, 4, 8);
        ts::Image flat = ts::Image::make(32, 32, 4, 8);
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 32; x++) {
                uint8_t* c = crate.pixel(x, y);
                if (x >= 6 && x < 26 && y >= 6 && y < 26) c[0] = 160, c[1] = 110, c[2] = 60, c[3] = 255;
                uint8_t* f = flat.pixel(x, y);
                f[0] = 120, f[1] = 110, f[2] = 100, f[3] = 255;
            }
        }
        writePng(w.file("assets/crate.png"), crate);
        writePng(w.file("assets/flat.png"), flat);
        SpriteNormalsWindow window(w.p);
        std::string error;
        // boxy: the default preset rounds a box into a pillow; Use Bevel takes the offered radius
        ASSERT_TRUE(window.openSources({w.file("assets/crate.png")}, error)) << error;
        ASSERT_TRUE(settle(window));
        Access::refreshWarningsNow(window);
        const SpriteNormalsWindow::WarningRow* boxy = row(window, "boxy");
        ASSERT_NE(boxy, nullptr);
        EXPECT_EQ(boxy->fixes, (std::vector<std::string>{"Use Bevel (radius 1)", "Use Props"}));
        EXPECT_NE(boxy->text.find("Rounded profile"), std::string::npos) << boxy->text;
        ASSERT_TRUE(window.applyFix("boxy", "Use Bevel (radius 1)", error)) << error;
        EXPECT_EQ(window.getSettings().profile, sn::Profile::Bevel);
        ASSERT_TRUE(window.getSettings().radius.has_value());
        EXPECT_FLOAT_EQ(*window.getSettings().radius, 1.0f);
        ASSERT_TRUE(settle(window));
        Access::refreshWarningsNow(window);
        EXPECT_EQ(row(window, "boxy"), nullptr) << "bevel is no pillow";
        window.setPreset(sn::Preset::Default);
        ASSERT_TRUE(settle(window));
        Access::refreshWarningsNow(window);
        ASSERT_NE(row(window, "boxy"), nullptr);
        ASSERT_TRUE(window.applyFix("boxy", "Use Props", error)) << error;
        EXPECT_EQ(window.getSettings().preset, sn::Preset::Props);
        // the Stones preset: gaps 30%, dome, wrap; another preset turns gaps off and unwraps
        window.setPreset(sn::Preset::Stones);
        EXPECT_FLOAT_EQ(window.getSettings().gaps.share, 0.3f);
        EXPECT_EQ(window.getSettings().profile, sn::Profile::Dome);
        EXPECT_EQ(window.getSettings().edges, sn::Edges::Wrap);
        window.setPreset(sn::Preset::Crisp);
        EXPECT_FALSE(window.getSettings().gaps.on());
        EXPECT_EQ(window.getSettings().edges, sn::Edges::Transparent);
        // a flat tile has no dark gaps: the row offers to lower the amount or turn gaps off
        ASSERT_TRUE(window.openSources({w.file("assets/flat.png")}, error)) << error;
        window.setPreset(sn::Preset::Stones);
        ASSERT_TRUE(settle(window));
        Access::refreshWarningsNow(window);
        const SpriteNormalsWindow::WarningRow* gaps = row(window, "gaps_unmatched");
        ASSERT_NE(gaps, nullptr);
        EXPECT_EQ(gaps->fixes, (std::vector<std::string>{"Lower Gap Amount", "Turn Off Dark Gaps"}));
        EXPECT_NE(gaps->text.find("Dark Gaps Are Edges"), std::string::npos) << gaps->text;
        ASSERT_TRUE(window.applyFix("gaps_unmatched", "Lower Gap Amount", error)) << error;
        EXPECT_FLOAT_EQ(window.getSettings().gaps.share, 0.15f);
        ASSERT_TRUE(window.applyFix("gaps_unmatched", "Turn Off Dark Gaps", error)) << error;
        EXPECT_FALSE(window.getSettings().gaps.on());
        ASSERT_TRUE(settle(window));
        Access::refreshWarningsNow(window);
        EXPECT_EQ(row(window, "gaps_unmatched"), nullptr);
        // Light Steps: the scene's by default (its 0: smooth), clamped to Scene..16, the lit preview
        // is renderLit with them
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
        ASSERT_TRUE(settle(window));
        ASSERT_EQ(w.p->getSelectedSceneId(), w.sceneId);
        EXPECT_EQ(window.getLightSteps(), SpriteNormalsWindow::kSceneLightSteps);
        EXPECT_EQ(window.effectiveLightSteps(), 0);
        const sn::Image smooth = Access::lit(window);
        w.sceneObject()->setLight2DSteps(5);
        EXPECT_EQ(window.effectiveLightSteps(), 5) << "follows the scene";
        EXPECT_EQ(Access::lightSetup(window).lightSteps, 5);
        w.sceneObject()->setLight2DSteps(0);
        window.setLightSteps(-7);
        EXPECT_EQ(window.getLightSteps(), SpriteNormalsWindow::kSceneLightSteps);
        window.setLightSteps(99);
        EXPECT_EQ(window.getLightSteps(), 16);
        window.setLightSteps(3);
        const sn::LightSetup setup = Access::lightSetup(window);
        EXPECT_EQ(setup.lightSteps, 3);
        const sn::Image stepped = Access::lit(window);
        ASSERT_FALSE(stepped.empty());
        EXPECT_NE(stepped.data, smooth.data);
        const std::shared_ptr<sn::SpriteNormalsRun> shown = Access::shown(window);
        ASSERT_TRUE(shown);
        EXPECT_EQ(stepped.data, sn::renderLit(shown->result().normal, shown->prepared().source.color, setup).data);
        // the controls draw (ten presets, the gap rows, the Light Steps combo)
        ImGuiContext* context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400, 900);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        window.open();
        window.setPreset(sn::Preset::Stones);
        window.setView(SpriteNormalsWindow::View::Lit);
        ASSERT_TRUE(settle(window));
        Access::drawFrame(window);
        EXPECT_FLOAT_EQ(window.getSettings().gaps.share, 0.3f) << "drawing changes nothing";
        EXPECT_EQ(window.getLightSteps(), 3);
        ImGui::DestroyContext(context);
    });
}

// S78 R14, R18: a light under half the sprite's larger side, raised to that side in one undo step
TEST(SpriteNormalsWindowDeathTest, LightLowRaisesTheLights) {
    IN_EDITOR("lightlow", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openForEntity(w.sceneId, w.hero, 0, false, error)) << error;
        ASSERT_TRUE(settle(window));
        // Hero is a 32 px Sprite: Height 64 is high enough; scaled 5 times it is 160 units, and 64 is low
        w.sceneObject()->getComponent<Light2DComponent>(w.light).height = 64.0f;
        Access::refreshWarningsNow(window);
        EXPECT_EQ(row(window, "light_low"), nullptr);
        w.sceneObject()->getComponent<Transform>(w.hero).worldScale = Vector3(5.0f, 5.0f, 1.0f);
        Access::refreshWarningsNow(window);
        const SpriteNormalsWindow::WarningRow* low = row(window, "light_low");
        ASSERT_NE(low, nullptr);
        EXPECT_NE(low->text.find("\"Light 2D\""), std::string::npos) << low->text;
        EXPECT_EQ(low->fixes, std::vector<std::string>{"Raise Light(s)"});
        const size_t before = w.scene()->position();
        ASSERT_TRUE(window.applyFix("light_low", "Raise Light(s)", error)) << error;
        EXPECT_FLOAT_EQ(w.sceneObject()->getComponent<Light2DComponent>(w.light).height, 160.0f);
        EXPECT_EQ(w.scene()->position(), before + 1) << "one undo step";
        EXPECT_EQ(row(window, "light_low"), nullptr);
        EXPECT_FALSE(window.applyFix("light_low", "Raise Light(s)", error)) << "nothing low any more";
        ASSERT_TRUE(w.scene()->undo());
        EXPECT_FLOAT_EQ(w.sceneObject()->getComponent<Light2DComponent>(w.light).height, 64.0f);
    });
}

TEST(SpriteNormalsWindowDeathTest, ProjectSwitchDuringARun) {
    IN_EDITOR("switch", {
        writePng(w.file("assets/big.png"), disk(1024, 1024));
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openSources({w.file("assets/big.png")}, error)) << error;
        ASSERT_TRUE(window.regenerate(error)) << error;
        const std::shared_ptr<sn::SpriteNormalsRun> run = Access::worker(window);
        ASSERT_NE(run, nullptr);
        window.resetForProjectSwitch();
        for (int i = 0; i < 5000 && !run->finished(); i++) usleep(2000);
        EXPECT_TRUE(run->finished());
        EXPECT_EQ(window.itemCount(), 0u);
        EXPECT_FALSE(window.hasResult());
        EXPECT_FALSE(window.hasAssignTarget());
        Access::tick(window);
        EXPECT_FALSE(fs::exists(w.file("assets/big_normal.png")));
    });
}

// ==================================================================== 20 workspace (in process)

TEST(SpriteNormalsWindowSettings, WorkspaceRoundTrip) {
    SpriteNormalsWindowSettings s;
    s.settings = sn::presetSettings(sn::Preset::Chunky);
    s.settings.preset = sn::Preset::Chunky;
    s.settings.detail = 6.5f;
    s.settings.radius = 7.0f;
    s.settings.edges = sn::Edges::Wrap;
    sn::Region region;
    region.colors = {0x112233, 0xffeedd};
    region.tolerance = 0.1f;
    region.lift = -3.0f;
    region.flat = true;
    s.settings.regions.push_back(region);
    s.view = 3;
    s.lightHeight = 48.0f;
    s.lightColour = Vector3(1.0f, 0.5f, 0.25f);
    s.ambient = 0.4f;
    s.orbit = true;
    s.grid = false;
    s.advancedOpen = true;
    s.autoRegenerate = true;
    const YAML::Node node = YAML::Load(YAML::Dump(Stream::encodeSpriteNormalsSettings(s)));
    const SpriteNormalsWindowSettings back = Stream::decodeSpriteNormalsSettings(node);
    EXPECT_EQ(sn::settingsToJson(back.settings), sn::settingsToJson(s.settings));
    EXPECT_EQ(back.view, 3);
    EXPECT_FLOAT_EQ(back.lightHeight, 48.0f);
    EXPECT_FLOAT_EQ(back.lightColour.y, 0.5f);
    EXPECT_FLOAT_EQ(back.ambient, 0.4f);
    EXPECT_TRUE(back.orbit);
    EXPECT_FALSE(back.grid);
    EXPECT_TRUE(back.advancedOpen);
    EXPECT_TRUE(back.autoRegenerate);

    // invalid values fall back to the defaults
    YAML::Node bad = YAML::Load("view: sideways\nambient: banana\norbit: maybe\n");
    const SpriteNormalsWindowSettings fallback = Stream::decodeSpriteNormalsSettings(bad);
    EXPECT_EQ(fallback.view, 2);
    EXPECT_FLOAT_EQ(fallback.ambient, 0.25f);
    EXPECT_FALSE(fallback.orbit);
    YAML::Node outOfRange = YAML::Load("settings: {preset: default, strength: 99}\n");
    EXPECT_THROW(Stream::decodeSpriteNormalsSettings(outOfRange), std::exception) << "the workspace reader warns and keeps the defaults";
}

// ==================================================================== fix round 1

TEST(SpriteNormalsWindowDeathTest, AnotherSourceDropsTheAssignTarget) {
    IN_EDITOR("target", {
        std::string error;
        std::vector<std::string> written;
        {
            SpriteNormalsWindow window(w.p);
            ASSERT_TRUE(window.openSources({w.file("assets/walk.png")}, error)) << error;
            ASSERT_TRUE(settle(window));
            ASSERT_TRUE(window.save(false, written, error)) << error;
        }
        SpriteNormalsWindow window(w.p);
        ASSERT_TRUE(window.openForEntity(w.sceneId, w.hero, 0, false, error)) << error;
        ASSERT_TRUE(window.getAssign());
        // the same image typed again keeps Hero as the target
        ASSERT_TRUE(Access::setSource(window, "assets/hero.png", error)) << error;
        EXPECT_TRUE(window.hasAssignTarget());
        // another image: Hero does not draw it, so no Save & Assign onto Hero
        ASSERT_TRUE(Access::setSource(window, "assets/walk.png", error)) << error;
        EXPECT_FALSE(window.hasAssignTarget());
        EXPECT_FALSE(window.getAssign());
        // Edit Normal Map on walk_normal.png after the wand on Hero: no target either
        ASSERT_TRUE(window.openForEntity(w.sceneId, w.hero, 0, false, error)) << error;
        ASSERT_TRUE(window.openRecipe(w.file("assets/walk_normal.png"), error)) << error;
        EXPECT_FALSE(window.hasAssignTarget());
        EXPECT_FALSE(window.getAssign());
        ASSERT_TRUE(settle(window));
        ASSERT_TRUE(window.save(true, written, error)) << error;
        EXPECT_TRUE(w.material(w.hero).normalTexture.getPath().empty()) << "Hero untouched";
    });
}

TEST(SpriteNormalsWindowDeathTest, StartRegenerateRunsOnTheWorker) {
    IN_EDITOR("startregen", {
        std::string error;
        std::vector<std::string> written;
        SpriteNormalsWindow window(w.p);
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
        ASSERT_TRUE(settle(window));
        ASSERT_TRUE(window.save(false, written, error)) << error;
        const std::string first = readAll(w.file("assets/hero_normal.png"));
        ts::Image changed = disk(32, 32);
        changed.pixel(16, 16)[0] = 3;
        writePng(w.file("assets/hero.png"), changed);
        const size_t before = w.projectHistory()->position();
        ASSERT_TRUE(window.startRegenerate(w.file("assets/hero_normal.png"), error)) << error;
        EXPECT_TRUE(window.hasPendingJobs());
        EXPECT_EQ(readAll(w.file("assets/hero_normal.png")), first) << "nothing written on the UI thread";
        ASSERT_TRUE(settle(window));
        EXPECT_FALSE(window.hasPendingJobs());
        EXPECT_NE(readAll(w.file("assets/hero_normal.png")), first);
        EXPECT_EQ(w.projectHistory()->position(), before + 1) << "one project undo step";
        EXPECT_EQ(sn::checkStaleness(w.dir, w.file("assets/hero_normal.png")).state, sn::Staleness::State::Fresh);
        fs::remove(w.file("assets/hero.png"));
        EXPECT_FALSE(window.startRegenerate(w.file("assets/hero_normal.png"), error));
        EXPECT_NE(error.find("missing"), std::string::npos) << error;
    });
}

TEST(SpriteNormalsWindowDeathTest, StoredSettingsSurviveAProjectSwitch) {
    IN_EDITOR("stored", {
        std::string error;
        SpriteNormalsWindow window(w.p);
        window.open();
        // the next project's stored settings, as its workspace restored them
        window.resetForProjectSwitch();
        SpriteNormalsWindowSettings stored;
        stored.settings = sn::presetSettings(sn::Preset::Chunky);
        stored.settings.preset = sn::Preset::Chunky;
        stored.settings.strength = 5.0f;
        stored.view = 3;
        w.p->getSpriteNormalsSettings() = stored;
        Access::tick(window);
        EXPECT_FLOAT_EQ(window.getSettings().strength, 5.0f) << "taken from the project";
        EXPECT_EQ(window.getView(), SpriteNormalsWindow::View::Split);
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
        window.setView(SpriteNormalsWindow::View::Lit);
        EXPECT_FLOAT_EQ(w.p->getSpriteNormalsSettings().settings.strength, 5.0f) << "never overwritten by the defaults";
        EXPECT_EQ(w.p->getSpriteNormalsSettings().settings.preset, sn::Preset::Chunky);
    });
}

TEST(SpriteNormalsWindowDeathTest, OrbitStopsWithTheWindow) {
    IN_EDITOR("orbit", {
        std::string error;
        SpriteNormalsWindow window(w.p);
        window.setOrbit(true);
        Access::advance(window, 0.5);
        EXPECT_DOUBLE_EQ(Access::orbitTime(window), 0.0) << "no source, no result: nothing orbits";
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
        ASSERT_TRUE(settle(window));
        const double t = Access::orbitTime(window);
        EXPECT_GT(t, 0.0);
        window.setOpen(false);
        Access::advance(window, 0.5);
        EXPECT_DOUBLE_EQ(Access::orbitTime(window), t) << "closed: the orbit stands still";
    });
}

TEST(SpriteNormalsWindowDeathTest, SaveAllRegeneratesItemsChangedMeanwhile) {
    IN_EDITOR("saveallstale", {
        writePng(w.file("assets/a.png"), disk(16, 16));
        SpriteNormalsWindow window(w.p);
        std::string error;
        std::vector<std::string> written;
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png"), w.file("assets/a.png")}, error)) << error;
        ASSERT_TRUE(window.save(false, written, error)) << error;
        // a setting changes while Save All runs: what is written follows it
        sn::Settings s = window.getSettings();
        s.strength = 3.25f;
        window.setSettings(s);
        ASSERT_TRUE(settle(window));
        for (const char* name : {"assets/hero_normal.png", "assets/a_normal.png"}) {
            const std::optional<nlohmann::json> recipe = sn::readRecipeHead(w.file(name));
            ASSERT_TRUE(recipe.has_value()) << name;
            EXPECT_DOUBLE_EQ((*recipe)["settings"]["strength"].get<double>(), 3.25) << name;
        }
        // written: only the shown item keeps its decoded result
        EXPECT_FALSE(Access::itemHasResult(window, 1));
    });
}

TEST(SpriteNormalsWindowDeathTest, QuickJobChecksItsTargetAndIgnoresARepeat) {
    IN_EDITOR("quicktarget", {
        std::string error;
        SpriteNormalsWindow window(w.p);
        ASSERT_TRUE(window.openForEntity(w.sceneId, w.hero, 0, true, error)) << error;
        ASSERT_TRUE(window.openForEntity(w.sceneId, w.hero, 0, true, error)) << error;
        EXPECT_EQ(Access::quickJobs(window), 1u) << "a repeated Shift-click is ignored while the first runs";
        // the target draws another image by the time the run ends: nothing written or assigned
        w.material(w.hero).baseColorTexture = Texture("assets/walk.png");
        ASSERT_TRUE(settle(window));
        EXPECT_FALSE(fs::exists(w.file("assets/hero_normal.png")));
        EXPECT_TRUE(w.material(w.hero).normalTexture.getPath().empty());
    });
}

TEST(SpriteNormalsWindowDeathTest, UndoRoutingKnowsItsOwnStep) {
    IN_EDITOR("undoidentity", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        std::vector<std::string> written;
        uint32_t sceneId = 0;
        ASSERT_TRUE(window.openForEntity(w.sceneId, w.hero, 0, false, error)) << error;
        ASSERT_TRUE(settle(window));
        ASSERT_TRUE(window.save(false, written, error)) << error;
        ASSERT_TRUE(window.undoGoesToScene(sceneId));
        // undone, then another edit takes the same position: not the assign
        ASSERT_TRUE(w.scene()->undo());
        auto* move = new PropertyCmd<Vector3>(w.p, w.sceneId, w.walker, ComponentType::Transform, "position", Vector3(7, 7, 0));
        move->setNoMerge();
        w.scene()->addCommand(move);
        EXPECT_FALSE(window.undoGoesToScene(sceneId)) << "the newest step is the move, at the assign's old position";
        EXPECT_FALSE(window.redoGoesToScene(sceneId));
    });
}

TEST(SpriteNormalsWindowDeathTest, ShiftClickUsesTheDefaultMapsRecipe) {
    IN_EDITOR("shiftrecipe", {
        std::string error;
        std::vector<std::string> written;
        {
            // hero_normal.png generated with Detailed, not assigned to Hero
            SpriteNormalsWindow window(w.p);
            ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
            window.setPreset(sn::Preset::Detailed);
            ASSERT_TRUE(settle(window));
            ASSERT_TRUE(window.save(false, written, error)) << error;
            window.setPreset(sn::Preset::Soft);
        }
        ASSERT_TRUE(w.material(w.hero).normalTexture.getPath().empty());
        SpriteNormalsWindow quick(w.p);
        ASSERT_TRUE(quick.openForEntity(w.sceneId, w.hero, 0, true, error)) << error;
        EXPECT_FALSE(quick.isOpen());
        ASSERT_TRUE(settle(quick));
        const std::optional<nlohmann::json> recipe = sn::readRecipeHead(w.file("assets/hero_normal.png"));
        ASSERT_TRUE(recipe.has_value());
        EXPECT_EQ((*recipe)["settings"]["preset"], "detailed") << "the map's own settings, not the last ones (soft)";
        EXPECT_EQ(w.material(w.hero).normalTexture.getPath(), "assets/hero_normal.png");
    });
}

// ==================================================================== fix round 2

TEST(SpriteNormalsWindowDeathTest, RegenerateWaitsForTheSceneToStop) {
    IN_EDITOR("regenplay", {
        std::string error;
        std::vector<std::string> written;
        SpriteNormalsWindow window(w.p);
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
        ASSERT_TRUE(settle(window));
        ASSERT_TRUE(window.save(false, written, error)) << error;
        const std::string first = readAll(w.file("assets/hero_normal.png"));
        ts::Image changed = disk(32, 32);
        changed.pixel(16, 16)[1] = 7;
        writePng(w.file("assets/hero.png"), changed);
        SceneProject* sp = w.p->getScene(w.sceneId);
        // refused while playing
        sp->playState = ScenePlayState::PLAYING;
        EXPECT_FALSE(window.startRegenerate(w.file("assets/hero_normal.png"), error));
        EXPECT_NE(error.find("stop the scene"), std::string::npos) << error;
        // started while stopped, the scene plays when it ends: it waits
        sp->playState = ScenePlayState::STOPPED;
        ASSERT_TRUE(window.startRegenerate(w.file("assets/hero_normal.png"), error)) << error;
        Access::waitWorkers(window);
        sp->playState = ScenePlayState::PLAYING;
        for (int i = 0; i < 10; i++) Access::advance(window, 0.1);
        EXPECT_EQ(readAll(w.file("assets/hero_normal.png")), first) << "nothing written during play";
        SpriteNormalsWindow::RegenerateOutcome outcome = window.regenerateOutcome(w.file("assets/hero_normal.png"));
        EXPECT_TRUE(outcome.known);
        EXPECT_FALSE(outcome.finished);
        EXPECT_NE(outcome.message.find("once the scene stops"), std::string::npos) << outcome.message;
        sp->playState = ScenePlayState::STOPPED;
        ASSERT_TRUE(settle(window));
        EXPECT_NE(readAll(w.file("assets/hero_normal.png")), first);
        outcome = window.regenerateOutcome(w.file("assets/hero_normal.png"));
        EXPECT_TRUE(outcome.finished && outcome.ok);
        EXPECT_EQ(outcome.message, "Regenerated hero_normal.png.");
    });
}

TEST(SpriteNormalsWindowDeathTest, RegenerateOutcomesAndSameNamesInTwoFolders) {
    IN_EDITOR("regenpaths", {
        std::string error;
        std::vector<std::string> written;
        for (const char* dir : {"assets/a", "assets/b"}) {
            writePng(w.file(std::string(dir) + "/hero.png"), disk(32, 32));
            SpriteNormalsWindow maker(w.p);
            ASSERT_TRUE(maker.openSources({w.file(std::string(dir) + "/hero.png")}, error)) << error;
            ASSERT_TRUE(settle(maker));
            ASSERT_TRUE(maker.save(false, written, error)) << error;
        }
        SpriteNormalsWindow window(w.p);
        ASSERT_TRUE(window.startRegenerate(w.file("assets/a/hero_normal.png"), error)) << error;
        ASSERT_TRUE(window.startRegenerate(w.file("assets/b/hero_normal.png"), error)) << error;
        EXPECT_EQ(Access::quickJobs(window), 2u) << "same file name, another folder: its own job";
        ASSERT_TRUE(window.startRegenerate(w.file("assets/a/hero_normal.png"), error)) << error;
        EXPECT_EQ(Access::quickJobs(window), 2u) << "the same file again: ignored while it runs";
        // b's write fails (a folder took its place): its outcome says so, a's says written
        Access::waitWorkers(window);
        fs::remove(w.file("assets/b/hero_normal.png"));
        fs::create_directories(w.file("assets/b/hero_normal.png"));
        ASSERT_TRUE(settle(window));
        const SpriteNormalsWindow::RegenerateOutcome a = window.regenerateOutcome(w.file("assets/a/hero_normal.png"));
        const SpriteNormalsWindow::RegenerateOutcome b = window.regenerateOutcome(w.file("assets/b/hero_normal.png"));
        EXPECT_TRUE(a.finished && a.ok) << a.message;
        EXPECT_TRUE(b.finished);
        EXPECT_FALSE(b.ok);
        EXPECT_EQ(b.message.rfind("Can't regenerate hero_normal.png", 0), 0u) << b.message;
    });
}

TEST(SpriteNormalsWindowDeathTest, UndoRoutingSurvivesUndoEditCycles) {
    IN_EDITOR("undoserial", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        std::vector<std::string> written;
        uint32_t sceneId = 0;
        ASSERT_TRUE(window.openForEntity(w.sceneId, w.hero, 0, false, error)) << error;
        ASSERT_TRUE(settle(window));
        ASSERT_TRUE(window.save(false, written, error)) << error;
        ASSERT_TRUE(window.undoGoesToScene(sceneId));
        ASSERT_TRUE(w.scene()->undo());
        // undo / edit / undo / edit: freed steps' memory may come back; the assign never does
        for (int i = 0; i < 20; i++) {
            auto* move = new PropertyCmd<Vector3>(w.p, w.sceneId, w.walker, ComponentType::Transform, "position", Vector3(float(i), 1, 0));
            move->setNoMerge();
            w.scene()->addCommand(move);
            EXPECT_FALSE(window.undoGoesToScene(sceneId)) << i;
            ASSERT_TRUE(w.scene()->undo());
            EXPECT_FALSE(window.redoGoesToScene(sceneId)) << "the redoable step is the move, not the assign: " << i;
        }
    });
}

TEST(SpriteNormalsWindowDeathTest, ProblemsFollowTheGameObject) {
    IN_EDITOR("sceneproblems", {
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openForEntity(w.sceneId, w.walker, 0, false, error)) << error;
        Access::tick(window);
        EXPECT_TRUE(Access::problems(window).empty());
        // re-sliced with the same number of frames, now overlapping: found on the next change
        SpriteComponent& walk = w.sceneObject()->getComponent<SpriteComponent>(w.walker);
        walk.framesRect[1].rect = Rect(16.0f, 0.0f, 32.0f, 32.0f);
        sn::Settings s = window.getSettings();
        s.strength = 9.0f;   // out of range: a settings problem too
        window.setSettings(s);
        const std::vector<sn::Problem>& problems = Access::problems(window);
        ASSERT_GE(problems.size(), 2u);
        EXPECT_EQ(problems[0].kind, "invalid_settings") << "settings problems first, as checkSpec lists them";
        bool overlap = false;
        for (const sn::Problem& p : problems) overlap = overlap || p.kind == "frames_mismatch";
        EXPECT_TRUE(overlap);
        // the texture swapped: the check runs again (and its order is kept)
        w.material(w.walker).baseColorTexture = Texture("assets/hero.png");
        s.strength = 9.5f;
        window.setSettings(s);
        EXPECT_EQ(Access::problems(window)[0].kind, "invalid_settings");
    });
}

TEST(SpriteNormalsWindowDeathTest, ALargeItemsManualResultSurvivesARowSwitch) {
    IN_EDITOR("batchkeep", {
        writePng(w.file("assets/big.png"), disk(1100, 1000));
        SpriteNormalsWindow window(w.p);
        std::string error;
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png"), w.file("assets/big.png")}, error)) << error;
        ASSERT_TRUE(settle(window));
        window.selectItem(1);
        ASSERT_TRUE(window.regenerate(error)) << error;
        ASSERT_TRUE(settle(window, 60000));
        ASSERT_TRUE(window.hasResult());
        window.selectItem(0);
        ASSERT_TRUE(settle(window));
        EXPECT_TRUE(Access::itemHasResult(window, 1)) << "not saved, not live: kept";
        window.selectItem(1);
        EXPECT_TRUE(window.hasResult()) << "shown again at once, no second Generate";
        EXPECT_EQ(Access::worker(window), nullptr);
    });
}

// ==================================================================== fix round 3

TEST(SpriteNormalsWindowDeathTest, JobWorkersNeverTouchTheSystem) {
    IN_EDITOR("nosystem", {
        std::string error;
        std::vector<std::string> written;
        {
            SpriteNormalsWindow maker(w.p);
            ASSERT_TRUE(maker.openSources({w.file("assets/hero.png")}, error)) << error;
            ASSERT_TRUE(settle(maker));
            ASSERT_TRUE(maker.save(false, written, error)) << error;
        }
        ts::Image changed = disk(32, 32);
        changed.pixel(16, 16)[2] = 9;
        writePng(w.file("assets/hero.png"), changed);
        const std::string before = readAll(w.file("assets/hero_normal.png"));
        SpriteNormalsWindow window(w.p);
        // the System is one that notes every asset-path read made off this thread (the project's
        // assets directory the main thread may be changing): the workers make none, the checks
        // run here
        // the test binary's engine is TestMain's (a second Engine::systemInit would restart its jobs
        // and logger): this System is swapped in
        CountingPlatform counting(w.p);
        jasperengine::SystemTestAccess::swap(&counting);
        ASSERT_TRUE(window.startRegenerate(w.file("assets/hero_normal.png"), error)) << error;
        EXPECT_EQ(window.regenerateOutcome(w.file("assets/hero_normal.png")).message, "Regenerating hero_normal.png...");
        ASSERT_TRUE(window.openForEntity(w.sceneId, w.walker, 0, true, error)) << error;
        Access::waitWorkers(window);
        EXPECT_EQ(counting.offMainThread.load(), 0) << "a worker read the System's asset path";
        ASSERT_TRUE(settle(window));
        EXPECT_EQ(counting.offMainThread.load(), 0);
        jasperengine::SystemTestAccess::swap(w.platform);
        EXPECT_NE(readAll(w.file("assets/hero_normal.png")), before);
        EXPECT_TRUE(window.regenerateOutcome(w.file("assets/hero_normal.png")).ok);
        EXPECT_EQ(w.material(w.walker).normalTexture.getPath(), "assets/walk_normal.png");
        // a project switch forgets the outcomes
        window.resetForProjectSwitch();
        EXPECT_FALSE(window.regenerateOutcome(w.file("assets/hero_normal.png")).known);
    });
}

TEST(SpriteNormalsWindowDeathTest, ShiftClickNeverWritesWhileAnySceneplays) {
    IN_EDITOR("anyplaying", {
        std::string error;
        const uint32_t normals = w.sceneId;
        // another scene of the project (a bare entry: only its play state matters here)
        SceneProject extra;
        extra.id = normals + 1000;
        extra.name = "Other";
        w.p->getScenes().push_back(extra);
        auto other = [&]() -> SceneProject* { return w.p->getScene(normals + 1000); };
        SpriteNormalsWindow window(w.p);
        // another scene plays: refused at once
        other()->playState = ScenePlayState::PLAYING;
        EXPECT_FALSE(window.openForEntity(normals, w.hero, 0, true, error));
        other()->playState = ScenePlayState::STOPPED;
        // started while stopped, another scene plays when it ends: nothing written or assigned
        ASSERT_TRUE(window.openForEntity(normals, w.hero, 0, true, error)) << error;
        Access::waitWorkers(window);
        other()->playState = ScenePlayState::PLAYING;
        ASSERT_TRUE(settle(window));
        EXPECT_FALSE(fs::exists(w.file("assets/hero_normal.png")));
        EXPECT_TRUE(w.material(w.hero).normalTexture.getPath().empty());
        other()->playState = ScenePlayState::STOPPED;
        w.p->getScenes().pop_back();
    });
}

// ==================================================================== Save without the MCP server

TEST(SpriteNormalsWindowDeathTest, SaveNeedsNoMcpServerAndRefreshesTheEditor) {
    IN_EDITOR("nomcp", {
        ASSERT_FALSE(mcp::McpServer::get().isStarted());
        SpriteNormalsWindow window(w.p);
        std::string error;
        std::vector<std::string> written;
        ASSERT_TRUE(window.openForEntity(w.sceneId, w.hero, 0, false, error)) << error;
        ASSERT_TRUE(settle(window));
        // Save & Assign: the file, one project step
        const size_t before = w.projectHistory()->position();
        ASSERT_TRUE(window.save(false, written, error)) << error;
        const fs::path normal = w.file("assets/hero_normal.png");
        ASSERT_TRUE(fs::exists(normal));
        EXPECT_EQ(w.projectHistory()->position(), before + 1);
        Texture& texture = w.material(w.hero).normalTexture;
        ASSERT_EQ(texture.getPath(), "assets/hero_normal.png");
        texture.load();
        ASSERT_TRUE(texture.hasData());
        // project undo: the file goes, the texture reading it is released (ProjectFileWrites::refresh)
        ASSERT_TRUE(w.projectHistory()->undo());
        EXPECT_FALSE(fs::exists(normal));
        EXPECT_FALSE(texture.hasData()) << "the texture reading the removed file is released";
        // redo: back
        ASSERT_TRUE(w.projectHistory()->redo());
        EXPECT_TRUE(fs::exists(normal));
        texture.load();
        ASSERT_TRUE(texture.hasData());
        // a replacing Save (no assign this time) releases it too: the next draw reads the new map
        window.setAssign(false);
        sn::Settings s = window.getSettings();
        s.detail = 7.0f;
        window.setSettings(s);
        ASSERT_TRUE(settle(window));
        const std::string first = readAll(normal);
        ASSERT_TRUE(window.save(true, written, error)) << error;
        EXPECT_NE(readAll(normal), first);
        EXPECT_FALSE(texture.hasData()) << "the texture reading the replaced file is released";
        EXPECT_EQ(texture.getPath(), "assets/hero_normal.png");
    });
}

TEST(SpriteNormalsWindowDeathTest, SaveWithTheMcpServerIsNoExternalChange) {
    IN_EDITOR_WITH_MCP("withmcp", {
        ASSERT_TRUE(mcp::McpServer::get().isStarted());
        mcp::McpContext& ctx = mcp::McpServer::get().context();
        auto listed = [](const nlohmann::json& sync, const char* list, const std::string& rel) {
            for (const nlohmann::json& p : sync["changed"][list]) {
                if (p == rel) return true;
            }
            return false;
        };
        mcp::w4::syncFiles(ctx, {}, true);   // the snapshot full syncs compare with
        SpriteNormalsWindow window(w.p);
        std::string error;
        std::vector<std::string> written;
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
        ASSERT_TRUE(settle(window));
        ASSERT_TRUE(window.save(false, written, error)) << error;
        // the window's write went through the server's file sync: not a change made behind its back
        nlohmann::json sync = mcp::w4::syncFiles(ctx, {}, true);
        EXPECT_FALSE(listed(sync, "added", "assets/hero_normal.png")) << sync.dump();
        ASSERT_TRUE(w.projectHistory()->undo());
        sync = mcp::w4::syncFiles(ctx, {}, true);
        EXPECT_FALSE(listed(sync, "removed", "assets/hero_normal.png")) << sync.dump();
        ASSERT_TRUE(w.projectHistory()->redo());
        sync = mcp::w4::syncFiles(ctx, {}, true);
        EXPECT_FALSE(listed(sync, "added", "assets/hero_normal.png")) << sync.dump();
        // a file written behind the editor's back is listed (the full sync does see changes)
        writePng(w.file("assets/elsewhere.png"), disk(8, 8));
        sync = mcp::w4::syncFiles(ctx, {}, true);
        EXPECT_TRUE(listed(sync, "added", "assets/elsewhere.png")) << sync.dump();
    });
}

// ==================================================================== outputs and symbolic links

TEST(SpriteNormalsWindowDeathTest, AnOutputThroughALinkOutOfTheAssetsIsRefused) {
    SKIP_WITHOUT_SYMLINKS();
    IN_EDITOR("symlink", {
        // assets/out is a link to a folder outside the project
        const fs::path outside = w.dir.parent_path() / "outside";
        fs::create_directories(outside);
        fs::create_directory_symlink(outside, w.file("assets/out"));
        SpriteNormalsWindow window(w.p);
        std::string error;
        std::vector<std::string> written;
        ASSERT_TRUE(window.openSources({w.file("assets/hero.png")}, error)) << error;
        ASSERT_TRUE(settle(window));
        ASSERT_TRUE(window.hasResult());
        window.setOutput("assets/out/hero_normal.png");
        bool refused = false;
        for (const sn::Problem& p : Access::problems(window)) {
            if (p.kind == "output_invalid" && p.message.find("symbolic link") != std::string::npos) refused = true;
        }
        EXPECT_TRUE(refused) << "the window lists the problem";
        const size_t before = w.projectHistory()->position();
        EXPECT_FALSE(window.save(false, written, error));
        EXPECT_NE(error.find("resolves outside the project's assets directory"), std::string::npos) << error;
        EXPECT_FALSE(fs::exists(outside / "hero_normal.png")) << "nothing written through the link";
        EXPECT_EQ(w.projectHistory()->position(), before);
        // a link to a folder of the assets is fine
        fs::create_directories(w.file("assets/real"));
        fs::create_directory_symlink(w.file("assets/real"), w.file("assets/alias"));
        window.setOutput("assets/alias/hero_normal.png");
        ASSERT_TRUE(window.save(false, written, error)) << error;
        EXPECT_TRUE(fs::exists(w.file("assets/real/hero_normal.png")));
        // an output that is the source through a link: refused (never replaced, even with overwrite)
        fs::create_symlink("hero.png", w.file("assets/hero_link.png"));
        window.setOutput("assets/hero_link.png");
        const std::string source = readAll(w.file("assets/hero.png"));
        EXPECT_FALSE(window.save(true, written, error));
        EXPECT_NE(error.find("would replace the source"), std::string::npos) << error;
        EXPECT_EQ(readAll(w.file("assets/hero.png")), source);
    });
}
