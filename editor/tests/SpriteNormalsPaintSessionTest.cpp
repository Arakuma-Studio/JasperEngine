// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The paint layer in the session (spritenormals/SpriteNormalsSession, Phase 5 spec 3 and 7.2):
// paint_mode keep / replace / clear across regeneration, the size and layout rules, previews
// compositing the existing file's paint, the chunk and the recipe's "paint", the undoable write
// putting the previous paint back, metrics on the composite, regenerating from a recipe.
// Runs a headless project, as SpriteNormalsSessionTest does.

#include <gtest/gtest.h>

#include "App.h"
#include "AppSettings.h"
#include "Backend.h"
#include "JasperEngine.h"
#include "Platform.h"
#include "Project.h"
#include "ScriptDataTestSupport.h"
#include "command/CommandHistory.h"
#include "mcp/McpServer.h"
#include "spritenormals/SpriteNormalsPaint.h"
#include "spritenormals/SpriteNormalsSession.h"
#include "texsynth/SynthesisRecipe.h"
#include "texsynth/SynthesisSession.h"

#include "json.hpp"

#include "support/PosixCompat.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace sn = jasperengine::editor::spritenormals;
namespace ts = jasperengine::editor::texsynth;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
        std::unique_ptr<editor::Platform> platform;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path base = fs::temp_directory_path() / ("jasper-spritenormals-paint-test-" + std::to_string(::getpid()));
            fs::remove_all(base);
            fs::create_directories(base / "config");
            setenv("XDG_CONFIG_HOME", (base / "config").c_str(), 1);
            out.dir = base / "project";
            fs::create_directories(out.dir / ".jasperengine");
            std::ofstream(out.dir / "project.yaml") << "name: spritenormalspaint\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            out.platform.reset(new editor::Platform(out.project.get()));
            // the editor's Platform as System::instance() for this suite's tests (the binary's engine
            // is TestMain's: a second Engine::systemInit would restart its jobs and logger)
            headlesssystem::use(out.platform.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            Engine::setAsyncLoading(false);
            return out;
        }();
        headlesssystem::use(h.platform.get());
        return h;
    }

    const fs::path& root() { return headless().dir; }

    std::string folder(const std::string& name) {
        fs::remove_all(root() / "assets" / name);
        fs::create_directories(root() / "assets" / name);
        return "assets/" + name;
    }

    void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // shaded balls on transparency, one per `cell` x `cell` frame
    sn::Image balls(int w, int h, int cell, float r) {
        sn::Image image = sn::Image::make(w, h, 4, 8);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const float cx = (x % cell) + 0.5f - cell * 0.5f, cy = (y % cell) + 0.5f - cell * 0.5f;
                if (cx * cx + cy * cy > r * r) continue;
                uint8_t* p = image.pixel(x, y);
                const uint8_t shade = static_cast<uint8_t>(120 + 60 * (-cx - cy) / (2.0f * r));
                p[0] = shade;
                p[1] = static_cast<uint8_t>(shade / 2);
                p[2] = 60;
                p[3] = 255;
            }
        }
        return image;
    }

    void writePng(const std::string& rel, const sn::Image& image) {
        std::vector<uint8_t> png;
        ASSERT_TRUE(ts::encodePng(image, png));
        writeBytes(root() / fs::u8path(rel), png);
    }

    std::vector<sn::PaintOp> ops(const char* text) {
        std::vector<sn::PaintOp> out;
        std::vector<sn::Problem> problems;
        EXPECT_TRUE(sn::paintOpsFromJson(json::parse(text), out, problems)) << (problems.empty() ? "" : problems[0].message);
        return out;
    }

    sn::Spec spec(const std::string& source, const char* paint = nullptr, sn::PaintMode mode = sn::PaintMode::Keep) {
        sn::Spec s;
        s.source = source;
        s.settings = sn::presetSettings(sn::Preset::Default);
        s.overwrite = true;
        if (paint) s.paint = ops(paint);
        s.paintMode = mode;
        return s;
    }

    std::optional<sn::Problem> findKind(const std::vector<sn::Problem>& problems, const std::string& kind) {
        for (const sn::Problem& p : problems) {
            if (p.kind == kind) return p;
        }
        return std::nullopt;
    }

    struct Outcome {
        sn::Prepared prepared;
        sn::Result result;
        std::vector<sn::Problem> problems;
        bool ok = false;
    };

    Outcome run(const sn::Spec& s) {
        Outcome r;
        r.ok = sn::prepare(root(), s, nullptr, r.prepared, r.problems);
        if (!r.ok) return r;
        r.result = sn::generatePrepared(r.prepared);
        r.ok = r.result.status == sn::Status::Ok;
        return r;
    }

    // prepare, generate with paint, encode, write (no history)
    Outcome write(const sn::Spec& s) {
        Outcome r = run(s);
        EXPECT_TRUE(r.ok) << (r.problems.empty() ? (r.result.errors.empty() ? "" : r.result.errors[0].message) : r.problems[0].message);
        if (!r.ok) return r;
        std::vector<sn::OutputFile> files;
        std::string error, failed;
        EXPECT_TRUE(sn::encodeOutputs(r.prepared, r.result, files, error)) << error;
        EXPECT_TRUE(sn::writeOutputsDirect(files, failed, error)) << error;
        return r;
    }

    std::array<uint8_t, 4> at(const sn::Image& image, int x, int y) {
        const uint8_t* p = image.pixel(x, y);
        return {p[0], p[1], p[2], p[3]};
    }

    using Px = std::array<uint8_t, 4>;

}

// ---- 1. keep across regeneration

TEST(SpriteNormalsPaintSession, KeepSurvivesRegenerationWithOtherSettings) {
    ASSERT_TRUE(headless().ready);
    const std::string dir = folder("keep");
    writePng(dir + "/hero.png", balls(32, 32, 32, 13));
    const Outcome first = write(spec(dir + "/hero.png",
                                 R"([{"op":"flatten","where":{"rect":[8,8,6,6]}},
                                     {"op":"angle","where":{"pixels":[[16,16]]},"direction":{"azimuth_deg":180,"tilt_deg":45}},
                                     {"op":"angle","where":{"pixels":[[20,20]]},"direction":[0,1,1],"strength":0.5}])"));
    ASSERT_TRUE(first.ok);
    ASSERT_EQ(first.prepared.paint.stats.ops.size(), 3u);
    EXPECT_EQ(first.prepared.paint.stats.ops[0].selected, 36);
    EXPECT_EQ(first.prepared.paint.stats.painted, 38);
    const fs::path map = root() / dir / "hero_normal.png";
    std::string why;
    const std::optional<sn::PaintOverlay> stored = sn::readPaint(map, &why);
    ASSERT_TRUE(stored) << why;
    EXPECT_EQ(*stored, first.prepared.paint.overlay);
    const std::optional<json> recipe = sn::readRecipe(map);
    ASSERT_TRUE(recipe);
    EXPECT_EQ((*recipe)["paint"], (json{{"painted", 38}}));

    // regenerate with another strength and no ops: the paint is kept and composited again
    sn::Spec again = spec(dir + "/hero.png");
    again.settings.strength = 4.0f;
    const Outcome second = write(again);
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(second.prepared.paintKeptFrom, dir + "/hero_normal.png");
    const sn::Result plain = sn::generate(second.prepared.request);
    int painted = 0;
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            const uint8_t a = second.prepared.paint.overlay.rgba[(static_cast<size_t>(y) * 32 + x) * 4 + 3];
            if (a == 255) {
                const uint8_t* p = &second.prepared.paint.overlay.rgba[(static_cast<size_t>(y) * 32 + x) * 4];
                EXPECT_EQ(at(second.result.normal, x, y), (Px{p[0], p[1], p[2], 255})) << x << "," << y;
                painted++;
            } else if (a == 0) {
                EXPECT_EQ(at(second.result.normal, x, y), at(plain.normal, x, y)) << x << "," << y;
            }
        }
    }
    EXPECT_EQ(painted, 37);
    EXPECT_EQ(at(second.result.normal, 16, 16), (Px{37, 128, 218, 255}));
    EXPECT_EQ(at(second.result.normal, 10, 10), (Px{128, 128, 255, 255}));
    EXPECT_EQ(sn::readPaint(map).value_or(sn::PaintOverlay()), *stored) << "the chunk is written again";
    EXPECT_EQ(sn::readRecipe(map).value_or(json())["settings"]["strength"], 4);

    // planned outputs report the paint already there; the staleness is unaffected
    const std::vector<sn::PlannedOutput> planned = sn::plannedOutputs(root(), spec(dir + "/hero.png"), sn::SourceKind::Image);
    ASSERT_TRUE(planned.at(0).existingPaint);
    EXPECT_EQ(planned[0].existingPaint->painted, 38);
    EXPECT_EQ(planned[0].existingPaint->width, 32);
    EXPECT_FALSE(sn::plannedOutputs(root(), spec(dir + "/hero.png", nullptr, sn::PaintMode::Replace), sn::SourceKind::Image)[0].existingPaint);
    EXPECT_EQ(sn::checkStaleness(root(), map).state, sn::Staleness::State::Fresh);
}

// ---- 2. replace and clear

TEST(SpriteNormalsPaintSession, ReplaceStartsOverClearDropsThePaint) {
    const std::string dir = folder("replace");
    writePng(dir + "/hero.png", balls(32, 32, 32, 13));
    const fs::path map = root() / dir / "hero_normal.png";
    write(spec(dir + "/hero.png", R"([{"op":"flatten","where":{"rect":[8,8,6,6]}}])"));
    // keep adds to it
    write(spec(dir + "/hero.png", R"([{"op":"flatten","where":{"pixels":[[16,16]]}}])"));
    EXPECT_EQ(sn::readPaint(map).value_or(sn::PaintOverlay()).paintedCount(), 37u);
    // replace starts from nothing
    const Outcome replaced = write(spec(dir + "/hero.png", R"([{"op":"flatten","where":{"pixels":[[16,16]]}}])", sn::PaintMode::Replace));
    EXPECT_TRUE(replaced.prepared.paintKeptFrom.empty());
    EXPECT_EQ(sn::readPaint(map).value_or(sn::PaintOverlay()).paintedCount(), 1u);
    // clear: no chunk, no recipe "paint", the generation alone
    const Outcome cleared = write(spec(dir + "/hero.png", nullptr, sn::PaintMode::Clear));
    std::string why;
    EXPECT_FALSE(sn::readPaint(map, &why).has_value());
    EXPECT_TRUE(why.empty()) << why;
    EXPECT_FALSE(sn::readRecipe(map).value_or(json()).contains("paint"));
    EXPECT_EQ(cleared.result.normal.data, sn::generate(cleared.prepared.request).normal.data);
    // clear with ops is refused
    const std::vector<sn::Problem> problems = sn::checkSpec(root(), spec(dir + "/hero.png", R"([{"op":"erase","where":{"frame":0}}])", sn::PaintMode::Clear));
    const std::optional<sn::Problem> refused = findKind(problems, "invalid_paint");
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->field, "paint");
    // checkSpec validates ops built in C++ too
    sn::Spec bad = spec(dir + "/hero.png", R"([{"op":"flatten","where":{"frame":0}}])");
    bad.paint[0].strength = 2.0f;
    const std::optional<sn::Problem> strength = findKind(sn::checkSpec(root(), bad), "invalid_paint");
    ASSERT_TRUE(strength);
    EXPECT_EQ(strength->field, "paint[0].strength");
    // a frame that is not one: prepare knows the cells
    Outcome frame = run(spec(dir + "/hero.png", R"([{"op":"flatten","where":{"frame":1}}])"));
    EXPECT_FALSE(frame.ok);
    ASSERT_TRUE(findKind(frame.problems, "invalid_paint"));
    EXPECT_EQ(findKind(frame.problems, "invalid_paint")->field, "paint[0].where.frame");
    // the fingerprint follows the paint
    EXPECT_NE(spec(dir + "/hero.png").fingerprint(), spec(dir + "/hero.png", R"([{"op":"flatten","where":{"frame":0}}])").fingerprint());
    EXPECT_NE(spec(dir + "/hero.png").fingerprint(), spec(dir + "/hero.png", nullptr, sn::PaintMode::Clear).fingerprint());
}

// ---- 3. size mismatch

TEST(SpriteNormalsPaintSession, AResizedSourceRefusesKeptPaint) {
    const std::string dir = folder("resized");
    writePng(dir + "/hero.png", balls(32, 32, 32, 13));
    const fs::path map = root() / dir / "hero_normal.png";
    write(spec(dir + "/hero.png", R"([{"op":"flatten","where":{"rect":[8,8,6,6]}}])"));
    const std::string before = readAll(map);
    writePng(dir + "/hero.png", balls(48, 32, 32, 13));
    const Outcome refused = run(spec(dir + "/hero.png"));
    EXPECT_FALSE(refused.ok);
    const std::optional<sn::Problem> mismatch = findKind(refused.problems, "paint_size_mismatch");
    ASSERT_TRUE(mismatch);
    EXPECT_EQ(mismatch->message, "hero_normal.png has 36 painted pixels for a 32x32 image; the new output is 48x32.");
    EXPECT_EQ(mismatch->fix, "Pass paint_mode: \"clear\" to drop the paint, or restore the source size.");
    EXPECT_EQ(readAll(map), before) << "nothing written";
    // clear writes it without paint
    const Outcome cleared = write(spec(dir + "/hero.png", nullptr, sn::PaintMode::Clear));
    ASSERT_TRUE(cleared.ok);
    EXPECT_FALSE(sn::readPaint(map).has_value());
    EXPECT_EQ(cleared.result.normal.width, 48);
}

// ---- 4. layout changed, hidden pixels, unreadable chunks

TEST(SpriteNormalsPaintSession, LayoutChangesHiddenPixelsAndUnreadableChunksWarn) {
    const std::string dir = folder("layout");
    writePng(dir + "/sheet.png", balls(32, 32, 16, 6));
    sn::Spec grid = spec(dir + "/sheet.png", R"([{"op":"flatten","where":{"frame":3}}])");
    grid.frames.mode = sn::FramesSpec::Mode::Grid;
    grid.frames.grid.tileWidth = grid.frames.grid.tileHeight = 16;
    const Outcome first = write(grid);
    ASSERT_TRUE(first.ok);
    EXPECT_GT(first.prepared.paint.stats.ops.at(0).selected, 50);
    EXPECT_FALSE(findKind(first.result.warnings, "paint_layout_changed"));

    // same size, no frames now
    sn::Spec none = spec(dir + "/sheet.png");
    none.frames.mode = sn::FramesSpec::Mode::None;
    const Outcome second = run(none);
    ASSERT_TRUE(second.ok);
    const std::optional<sn::Problem> layout = findKind(second.result.warnings, "paint_layout_changed");
    ASSERT_TRUE(layout);
    EXPECT_NE(layout->message.find("4 cells then, 0 now"), std::string::npos) << layout->message;
    EXPECT_EQ(second.prepared.paint.overlay, first.prepared.paint.overlay) << "the overlay is kept";

    // a painted pixel that turned transparent is hidden
    sn::Image holes = balls(32, 32, 16, 6);
    const int cx = 24, cy = 24;
    holes.pixel(cx, cy)[3] = 0;
    holes.pixel(cx + 1, cy)[3] = 0;
    writePng(dir + "/sheet.png", holes);
    const Outcome hidden = run(grid);
    ASSERT_TRUE(hidden.ok);
    EXPECT_EQ(hidden.prepared.paint.stats.hidden, 2);
    const std::optional<sn::Problem> warning = findKind(hidden.result.warnings, "paint_hidden_pixels");
    ASSERT_TRUE(warning);
    EXPECT_EQ(warning->message.rfind("2 painted pixels", 0), 0u) << warning->message;
    EXPECT_EQ(at(hidden.result.normal, cx, cy), (Px{128, 128, 255, 0}));

    // an unreadable chunk: a warning, the paint starts empty and the next write drops it
    const fs::path map = root() / dir / "sheet_normal.png";
    std::vector<uint8_t> bytes;
    {
        const std::string all = readAll(map);
        bytes.assign(all.begin(), all.end());
    }
    std::string text;
    sn::Problem problem;
    sn::PaintOverlay garbage = sn::PaintOverlay::blank(32, 32);
    garbage.rgba[3] = 255;
    ASSERT_TRUE(sn::encodePaintText(garbage, text, problem));
    text.replace(text.find("\"version\":1"), 11, "\"version\":9");
    std::vector<uint8_t> broken;
    std::string error;
    ASSERT_TRUE(ts::insertRecipeChunk(bytes, text, broken, error, sn::kPaintKeyword)) << error;
    writeBytes(map, broken);
    const Outcome unreadable = write(grid);
    ASSERT_TRUE(unreadable.ok);
    const std::optional<sn::Problem> dropped = findKind(unreadable.result.warnings, "paint_unreadable");
    ASSERT_TRUE(dropped);
    EXPECT_NE(dropped->message.find("version 9"), std::string::npos) << dropped->message;
    EXPECT_EQ(unreadable.prepared.paint.stats.painted, static_cast<int>(unreadable.prepared.paint.stats.ops.at(0).changed));
    std::string why;
    EXPECT_TRUE(sn::readPaint(map, &why).has_value()) << "rewritten readable: " << why;
}

// ---- 5. a preview composites the file the default output would write

TEST(SpriteNormalsPaintSession, PreviewKeepsTheExistingFilesPaint) {
    const std::string dir = folder("preview");
    writePng(dir + "/hero.png", balls(32, 32, 32, 13));
    write(spec(dir + "/hero.png", R"([{"op":"angle","where":{"rect":[10,10,4,4]},"direction":{"azimuth_deg":0,"tilt_deg":45}}])"));
    sn::Spec preview = spec(dir + "/hero.png");   // no output given: the default names
    const Outcome r = run(preview);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.prepared.paintKeptFrom, dir + "/hero_normal.png");
    EXPECT_EQ(at(r.result.normal, 11, 11), (Px{218, 128, 218, 255}));
    EXPECT_EQ(r.prepared.paint.stats.painted, 16);
    EXPECT_TRUE(r.prepared.paint.stats.ops.empty());
}

// ---- 7. metrics on the composite

TEST(SpriteNormalsPaintSession, MetricsAreOfTheComposite) {
    const std::string dir = folder("metrics");
    writePng(dir + "/hero.png", balls(32, 32, 32, 13));
    const Outcome r = run(spec(dir + "/hero.png", R"([{"op":"flatten","where":{"rect":[0,0,32,32]}}])", sn::PaintMode::Replace));
    ASSERT_TRUE(r.ok);
    const sn::Metrics m = sn::measure(r.result.normal, r.prepared.request.cells, false, 0.03f, &r.prepared.paint.overlay);
    EXPECT_DOUBLE_EQ(m.flatPct, 100.0);
    EXPECT_DOUBLE_EQ(m.paintedPct, 100.0);
    EXPECT_EQ(m.paintedPixels, m.opaquePixels);
    const sn::Assessment a = sn::assess(r.prepared.request.settings, m, 0);
    EXPECT_NE(std::find(a.lines.begin(), a.lines.end(), "Painted: " + std::to_string(m.opaquePixels) + " px (100.0%)"), a.lines.end());
}

// ---- 8. regenerating from the recipe keeps the paint

TEST(SpriteNormalsPaintSession, SpecFromRecipeRegeneratesWithThePaint) {
    const std::string dir = folder("recipe");
    writePng(dir + "/hero.png", balls(32, 32, 32, 13));
    const Outcome first = write(spec(dir + "/hero.png", R"([{"op":"flatten","where":{"rect":[8,8,6,6]}}])"));
    const fs::path map = root() / dir / "hero_normal.png";
    // the source changes (stale): regenerate from the recipe
    sn::Image changed = balls(32, 32, 32, 13);
    changed.pixel(16, 16)[0] = 10;
    writePng(dir + "/hero.png", changed);
    EXPECT_EQ(sn::checkStaleness(root(), map).state, sn::Staleness::State::Changed);
    sn::Spec back;
    std::vector<std::string> notes;
    std::string error;
    ASSERT_TRUE(sn::specFromRecipe(sn::readRecipe(map).value_or(json()), map, root(), back, notes, error)) << error;
    EXPECT_EQ(back.paintMode, sn::PaintMode::Keep);
    EXPECT_TRUE(back.paint.empty());
    const Outcome again = write(back);
    ASSERT_TRUE(again.ok);
    EXPECT_EQ(again.prepared.paint.overlay, first.prepared.paint.overlay);
    EXPECT_EQ(at(again.result.normal, 10, 10), (Px{128, 128, 255, 255}));
    EXPECT_EQ(sn::checkStaleness(root(), map).state, sn::Staleness::State::Fresh);
    EXPECT_EQ(sn::readPaint(map).value_or(sn::PaintOverlay()).paintedCount(), 36u);
}

// ---- the worker

TEST(SpriteNormalsPaintSession, TheRunCompositesThePaint) {
    const std::string dir = folder("worker");
    writePng(dir + "/hero.png", balls(32, 32, 32, 13));
    sn::Prepared prepared;
    std::vector<sn::Problem> problems;
    ASSERT_TRUE(sn::prepare(root(), spec(dir + "/hero.png", R"([{"op":"flatten","where":{"rect":[8,8,6,6]}}])", sn::PaintMode::Replace), nullptr,
                            prepared, problems));
    std::shared_ptr<sn::SpriteNormalsRun> worker = sn::SpriteNormalsRun::start(std::move(prepared), 2);
    for (int i = 0; i < 2000 && !worker->finished(); i++) usleep(5000);
    ASSERT_TRUE(worker->finished());
    ASSERT_EQ(worker->result().status, sn::Status::Ok);
    EXPECT_EQ(worker->prepared().paint.stats.painted, 36);
    EXPECT_EQ(at(worker->result().normal, 10, 10), (Px{128, 128, 255, 255}));
}

// ---- 6. undo of the write restores the previous file, paint included

namespace {

    void undoRestoresThePreviousPaint(const fs::path& dir) {
        App& app = Backend::getApp();
        Project* p = app.getProject();
        editor::Platform platform(p);
        // the test binary's engine is TestMain's (a second Engine::systemInit would restart its jobs
        // and logger): this System is swapped in
        jasperengine::SystemTestAccess::swap(&platform);
        if (!p->loadProject(dir, false)) std::exit(2);
        // the write needs no MCP server (ProjectFileWrites)
        if (mcp::McpServer::get().isStarted()) std::exit(12);
        std::vector<uint8_t> source;
        ts::encodePng(balls(32, 32, 32, 13), source);
        writeBytes(dir / "assets" / "undo" / "hero.png", source);
        CommandHistory* history = p->getProjectCommandHistory();
        auto undoable = [&](const char* paint) {
            sn::Spec s = spec("assets/undo/hero.png", paint);
            sn::Prepared prepared;
            std::vector<sn::Problem> problems;
            if (!sn::prepare(dir, s, nullptr, prepared, problems)) std::exit(3);
            const sn::Result result = sn::generatePrepared(prepared);
            std::vector<sn::OutputFile> files;
            std::string error;
            if (result.status != sn::Status::Ok || !sn::encodeOutputs(prepared, result, files, error)) std::exit(4);
            if (!sn::writeOutputsUndoable(p, files, error)) std::exit(5);
            return files.at(0);
        };
        const sn::OutputFile v1 = undoable(R"([{"op":"flatten","where":{"rect":[8,8,6,6]}}])");
        const sn::OutputFile v2 = undoable(R"([{"op":"erase","where":{"rect":[8,8,3,6]}}])");
        if (readAll(v2.absolute) != v2.bytes) std::exit(6);
        if (sn::readPaint(v2.absolute).value_or(sn::PaintOverlay()).paintedCount() != 18) std::exit(7);
        if (!history->undo()) std::exit(8);
        if (readAll(v1.absolute) != v1.bytes) std::exit(9);
        if (sn::readPaint(v1.absolute).value_or(sn::PaintOverlay()).paintedCount() != 36) std::exit(10);
        if (!history->redo() || readAll(v2.absolute) != v2.bytes) std::exit(11);
        std::fflush(stdout);
        // the forked editor ends without its static destructors: its jobs (thumbnails, imports)
        // may still read the App while it would be destroyed
        std::_Exit(0);
    }

}

TEST(SpriteNormalsPaintSessionDeathTest, UndoOfTheWriteRestoresThePreviousPaint) {
    const fs::path dir = fs::temp_directory_path() / ("jasper-spritenormals-paint-undo-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / ".jasperengine");
    std::ofstream(dir / "project.yaml") << "name: undo\n";
    GTEST_FLAG_SET(death_test_style, "threadsafe"); EXPECT_EXIT(undoRestoresThePreviousPaint(dir), ::testing::ExitedWithCode(0), "");
    fs::remove_all(dir);
}
