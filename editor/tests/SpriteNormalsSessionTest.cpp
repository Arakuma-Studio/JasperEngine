// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor side of the sprite normal generator (spritenormals/SpriteNormalsSession, Phase 2
// spec 8): output naming, checkSpec's problem kinds, prepare + generate + encodeOutputs (the
// recipes and the frames JSON), all-or-nothing writes, the undoable write, staleness,
// recipes back to specs, the worker, and what a scene says (Sprite frames, lights, warnings).
// Runs a headless project, as VatEditorTest does.

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "Backend.h"
#include "JasperEngine.h"
#include "Platform.h"
#include "Project.h"
#include "ScriptDataTestSupport.h"
#include "App.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "component/Light2DComponent.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "mcp/McpServer.h"
#include "mcp/tools/SpriteNormalsSheetWrite.h"
#include "spritenormals/SpriteNormalsSession.h"
#include "spritenormals/SpriteNormalsWatcher.h"
#include "texsynth/SynthesisRecipe.h"
#include "texsynth/SynthesisSession.h"
#include "util/ProjectFileWrites.h"
#include "util/SafeWrite.h"
#include "SymlinkSupport.h"

#include "json.hpp"

#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "support/PosixCompat.h"

#include <csignal>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "util/FileUtils.h"

using namespace jasperengine;
using namespace jasperengine::editor;
namespace sn = jasperengine::editor::spritenormals;
namespace ts = jasperengine::editor::texsynth;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

    // ---- one headless project for this file

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
        std::unique_ptr<editor::Platform> platform;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-spritenormals-session-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            std::ofstream(out.dir / "project.yaml") << "name: spritenormals\n";
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
        // another test file's project may have taken the engine's Platform since
        headlesssystem::use(h.platform.get());
        return h;
    }

    Project* project() { return headless().project.get(); }
    const fs::path& root() { return headless().dir; }

    // a fresh folder under assets per test
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

    // A shaded ball on transparency: w x h, a disk of radius r per frame cell of `cell` pixels
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

    // walk.gif of the generator's fixtures (8 frames)
    void copyWalkGif(const std::string& rel) {
        const fs::path fixture = fs::path(JASPER_EDITOR_TEST_FIXTURES) / ".." / ".." / "spritenormals" / "tests" / "fixtures" / "inputs" / "walk.gif";
        fs::create_directories((root() / fs::u8path(rel)).parent_path());
        fs::copy_file(fixture, root() / fs::u8path(rel), fs::copy_options::overwrite_existing);
    }

    std::vector<std::string> kinds(const std::vector<sn::Problem>& problems) {
        std::vector<std::string> out;
        for (const sn::Problem& p : problems) out.push_back(p.kind);
        return out;
    }

    // a copy: the problems are usually a temporary
    std::optional<sn::Problem> findKind(const std::vector<sn::Problem>& problems, const std::string& kind) {
        for (const sn::Problem& p : problems) {
            if (p.kind == kind) return p;
        }
        return std::nullopt;
    }

    sn::Spec stillSpec(const std::string& source) {
        sn::Spec spec;
        spec.source = source;
        spec.settings = sn::presetSettings(sn::Preset::Default);
        return spec;
    }

    // prepare + generate + encodeOutputs
    std::vector<sn::OutputFile> generateFiles(const sn::Spec& spec, const Scene* scene = nullptr, sn::Prepared* keep = nullptr) {
        sn::Prepared prepared;
        std::vector<sn::Problem> problems;
        EXPECT_TRUE(sn::prepare(root(), spec, scene, prepared, problems)) << (problems.empty() ? "" : problems[0].message);
        const sn::Result result = sn::generate(prepared.request);
        EXPECT_EQ(result.status, sn::Status::Ok);
        std::vector<sn::OutputFile> files;
        std::string error;
        EXPECT_TRUE(sn::encodeOutputs(prepared, result, files, error)) << error;
        if (keep) *keep = prepared;
        return files;
    }

    const sn::OutputFile* role(const std::vector<sn::OutputFile>& files, const std::string& name) {
        for (const sn::OutputFile& f : files) {
            if (f.role == name) return &f;
        }
        return nullptr;
    }

    json recipeOf(const sn::OutputFile& file) {
        std::string error;
        const std::optional<json> recipe = sn::readRecipe(reinterpret_cast<const uint8_t*>(file.bytes.data()), file.bytes.size(), &error);
        EXPECT_TRUE(recipe) << file.path << ": " << error;
        return recipe.value_or(json());
    }

    uint32_t newScene() {
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("Sprites", SceneType::SCENE_2D);
        return p->getSelectedSceneId();
    }

    Scene* sceneOf(uint32_t sceneId) { return project()->getScene(sceneId)->scene; }

    Entity create(uint32_t sceneId, const std::string& name, EntityCreationType type) {
        auto* cmd = new CreateEntityCmd(project(), sceneId, name, type);
        cmd->setQuiet(true);
        CommandHandle::get(sceneId)->addCommandNoMerge(cmd);
        return cmd->getEntity();
    }

    // A Sprite drawing `texture` (project-relative = assets-relative with the default assets dir)
    Entity sprite(uint32_t sceneId, const std::string& texture) {
        const Entity e = create(sceneId, "Hero", EntityCreationType::SPRITE);
        sceneOf(sceneId)->getComponent<MeshComponent>(e).submeshes[0].material.baseColorTexture = Texture(texture);
        return e;
    }

}

// ---- naming

TEST(SpriteNormalsSession, DefaultOutputs) {
    auto paths = [](const std::vector<sn::PlannedOutput>& outputs) {
        std::vector<std::string> out;
        for (const sn::PlannedOutput& p : outputs) out.push_back(p.role + "=" + p.path);
        return out;
    };
    EXPECT_EQ(paths(sn::defaultOutputs("assets/hero.png", sn::SourceKind::Image, false, false)), (std::vector<std::string>{"normal=assets/hero_normal.png"}));
    EXPECT_EQ(paths(sn::defaultOutputs("assets/hero.png", sn::SourceKind::Image, true, true)),
              (std::vector<std::string>{"normal=assets/hero_normal.png", "height=assets/hero_height.png", "frames_json=assets/hero_normal.frames.json"}));
    EXPECT_EQ(paths(sn::defaultOutputs("assets/a/walk.gif", sn::SourceKind::Gif, false, false)),
              (std::vector<std::string>{"normal=assets/a/walk_sheet_normal.png", "atlas=assets/a/walk_sheet.png"}));
    EXPECT_EQ(paths(sn::defaultOutputs("spin.png", sn::SourceKind::Apng, true, true)),
              (std::vector<std::string>{"normal=spin_sheet_normal.png", "atlas=spin_sheet.png", "height=spin_sheet_height.png",
                                        "frames_json=spin_sheet.frames.json"}));
    // explicit paths win; the frames JSON follows the atlas (or normal) actually written
    sn::Spec spec = stillSpec("assets/hero.gif");
    spec.output.atlas = "assets/out/atlas.png";
    spec.output.framesJson = true;
    EXPECT_EQ(paths(sn::plannedOutputs(root(), spec, sn::SourceKind::Gif)),
              (std::vector<std::string>{"normal=assets/hero_sheet_normal.png", "atlas=assets/out/atlas.png", "frames_json=assets/out/atlas.frames.json"}));
    spec.output.framesJsonPath = "assets/f.json";
    spec.output.normal = "assets/n.png";
    EXPECT_EQ(paths(sn::plannedOutputs(root(), spec, sn::SourceKind::Image)), (std::vector<std::string>{"normal=assets/n.png", "frames_json=assets/f.json"}));
}

TEST(SpriteNormalsSession, ResolveProjectFile) {
    fs::path abs;
    std::string error;
    EXPECT_TRUE(sn::resolveProjectFile(root(), "assets/./x/../hero.png", abs, error)) << error;
    EXPECT_EQ(abs, (root() / "assets" / "hero.png").lexically_normal());
    EXPECT_EQ(sn::projectRelative(root(), abs), "assets/hero.png");
    EXPECT_FALSE(sn::resolveProjectFile(root(), "../hero.png", abs, error));
    EXPECT_NE(error.find("outside the project"), std::string::npos) << error;
    EXPECT_FALSE(sn::resolveProjectFile(root(), "/tmp/hero.png", abs, error));
    EXPECT_NE(error.find("absolute"), std::string::npos) << error;
    EXPECT_FALSE(sn::resolveProjectFile(root(), ".jasperengine/hero.png", abs, error));
    EXPECT_NE(error.find("hidden"), std::string::npos) << error;
    // outside a project: as given
    EXPECT_TRUE(sn::resolveProjectFile(fs::path(), "/tmp/x/hero.png", abs, error));
    EXPECT_EQ(abs, fs::path("/tmp/x/hero.png"));
}

// ---- checkSpec

TEST(SpriteNormalsSession, CheckSpecKinds) {
    const std::string dir = folder("check");
    writePng(dir + "/hero.png", balls(32, 32, 32, 12));
    copyWalkGif(dir + "/walk.gif");

    EXPECT_TRUE(sn::checkSpec(root(), stillSpec(dir + "/hero.png")).empty());

    // missing source, outside the project, in a hidden directory, no source at all
    std::optional<sn::Problem> p = findKind(sn::checkSpec(root(), stillSpec(dir + "/nope.png")), "invalid_source");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->field, "source");
    EXPECT_NE(p->message.find("does not exist"), std::string::npos);
    EXPECT_TRUE(findKind(sn::checkSpec(root(), stillSpec("../outside.png")), "invalid_source"));
    std::vector<uint8_t> png;
    ASSERT_TRUE(ts::encodePng(balls(8, 8, 8, 3), png));
    writeBytes(root() / ".hidden" / "x.png", png);
    p = findKind(sn::checkSpec(root(), stillSpec(".hidden/x.png")), "invalid_source");
    ASSERT_TRUE(p);
    EXPECT_NE(p->message.find("hidden"), std::string::npos) << p->message;
    EXPECT_TRUE(findKind(sn::checkSpec(root(), stillSpec("")), "invalid_source"));
    // not an image
    std::ofstream(root() / fs::u8path(dir) / "notes.png") << "text";
    EXPECT_TRUE(findKind(sn::checkSpec(root(), stillSpec(dir + "/notes.png")), "invalid_source"));

    // a grid on a GIF
    sn::Spec gif = stillSpec(dir + "/walk.gif");
    gif.frames.mode = sn::FramesSpec::Mode::Grid;
    gif.frames.grid.tileWidth = gif.frames.grid.tileHeight = 8;
    p = findKind(sn::checkSpec(root(), gif), "frames_mismatch");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->message, "The frames come from the GIF; leave frames auto.");
    // frames none on a GIF
    gif.frames.mode = sn::FramesSpec::Mode::None;
    p = findKind(sn::checkSpec(root(), gif), "frames_mismatch");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->message, "An animation's frames come from the file; leave frames auto.");
    // an empty grid on a still
    sn::Spec grid = stillSpec(dir + "/hero.png");
    grid.frames.mode = sn::FramesSpec::Mode::Grid;
    grid.frames.grid.tileWidth = grid.frames.grid.tileHeight = 64;
    EXPECT_TRUE(findKind(sn::checkSpec(root(), grid), "frames_mismatch"));
    // a cell outside the image
    sn::Spec cells = stillSpec(dir + "/hero.png");
    cells.frames.mode = sn::FramesSpec::Mode::Cells;
    cells.frames.cells = {{0, 0, 16, 16}, {20, 20, 16, 16}};
    p = findKind(sn::checkSpec(root(), cells), "frames_mismatch");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->field, "frames.cells[1]");

    // outputs: existing, colliding with the source, not .png, outside, the frames JSON not .json
    writePng(dir + "/hero_normal.png", balls(32, 32, 32, 12));
    p = findKind(sn::checkSpec(root(), stillSpec(dir + "/hero.png")), "output_exists");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->field, "output.normal");
    EXPECT_EQ(p->fix, "Pass overwrite, or another output path.");
    sn::Spec over = stillSpec(dir + "/hero.png");
    over.overwrite = true;
    EXPECT_TRUE(sn::checkSpec(root(), over).empty());
    sn::Spec collide = over;
    collide.output.normal = dir + "/hero.png";
    p = findKind(sn::checkSpec(root(), collide), "output_invalid");
    ASSERT_TRUE(p);
    EXPECT_NE(p->message.find("replace the source"), std::string::npos) << p->message;
    sn::Spec jpg = over;
    jpg.output.normal = dir + "/hero_normal.jpg";
    EXPECT_TRUE(findKind(sn::checkSpec(root(), jpg), "output_invalid"));
    sn::Spec outside = over;
    outside.output.normal = "../hero_normal.png";
    EXPECT_TRUE(findKind(sn::checkSpec(root(), outside), "output_invalid"));
    sn::Spec both = over;
    both.settings.height = true;
    both.output.height = dir + "/hero_normal.png";
    p = findKind(sn::checkSpec(root(), both), "output_invalid");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->field, "output.height");
    sn::Spec frames = over;
    frames.output.framesJson = true;
    frames.output.framesJsonPath = dir + "/frames.txt";
    EXPECT_TRUE(findKind(sn::checkSpec(root(), frames), "output_invalid"));
    sn::Spec latin = over;
    latin.output.normal = dir + "/h\xE9ro.png";
    p = findKind(sn::checkSpec(root(), latin), "output_invalid");
    ASSERT_TRUE(p);
    EXPECT_NE(p->message.find("not valid UTF-8"), std::string::npos);
    EXPECT_TRUE(ts::isValidUtf8(p->message));

    // settings out of range come from validateSettings, every one of them
    sn::Spec bad = over;
    bad.settings.strength = 9.0f;
    bad.settings.alphaThreshold = 2.0f;
    std::vector<sn::Problem> problems = sn::checkSpec(root(), bad);
    EXPECT_EQ(kinds(problems), (std::vector<std::string>{"invalid_settings", "invalid_settings"}));
}

// The trusted roots are resolved on the main thread when a root setting changes and published
// whole: workers read a snapshot (never a half-updated list, never resolving themselves)
TEST(SpriteNormalsSession, TrustedRootsAreAPublishedSnapshot) {
    namespace sw = jasperengine::editor::safewrite;
    std::error_code ec;
    fs::create_directories(project()->getProjectPath() / "assets", ec);
    const fs::path projectReal = sw::resolvedPath(project()->getProjectPath());
    const fs::path assetsReal = sw::resolvedPath(project()->getProjectPath() / "assets");
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; i++) {
        readers.emplace_back([&]() {
            while (!stop.load()) {
                const std::vector<fs::path> roots = project()->getTrustedRoots();
                // either configuration, whole: the project first, then (with "assets") the assets folder
                const bool whole = (roots.size() == 1 && roots[0] == projectReal) ||
                                   (roots.size() == 2 && roots[0] == projectReal && roots[1] == assetsReal);
                if (!whole) bad++;
            }
        });
    }
    for (int i = 0; i < 200; i++) {
        project()->setAssetsDir(i % 2 ? "." : "assets");
    }
    stop = true;
    for (std::thread& t : readers) t.join();
    project()->setAssetsDir(".");
    EXPECT_EQ(bad.load(), 0) << "a reader saw a partial list";
    EXPECT_EQ(project()->getTrustedRoots(), std::vector<fs::path>{projectReal});
}

TEST(SpriteNormalsSession, OutputsMustBeInsideTheAssetsDirectory) {
    const std::string dir = folder("assetsdir");
    writePng(dir + "/hero.png", balls(16, 16, 16, 6));
    // the project's assets directory is "assets" for this test (the Platform's asset path follows it)
    project()->setAssetsDir("assets");
    sn::Spec spec = stillSpec(dir + "/hero.png");
    EXPECT_TRUE(sn::checkSpec(root(), spec).empty()) << "the default output is beside the source, in assets/";
    spec.output.normal = "elsewhere/hero_normal.png";
    const std::vector<sn::Problem> problems = sn::checkSpec(root(), spec);
    std::optional<sn::Problem> p = findKind(problems, "output_invalid");
    project()->setAssetsDir(".");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->field, "output.normal");
    EXPECT_EQ(p->message, "elsewhere/hero_normal.png is outside the project's assets directory (assets).");
    // with the default assets directory "." the whole project is assets
    EXPECT_TRUE(sn::checkSpec(root(), spec).empty());
}

// ---- symbolic links: every output stays inside the assets directory once links are resolved

TEST(SpriteNormalsSession, OutputsThroughALinkOutOfTheAssetsAreRefused) {
    SKIP_WITHOUT_SYMLINKS();
    const std::string dir = folder("links");
    writePng(dir + "/hero.png", balls(16, 16, 16, 6));
    copyWalkGif(dir + "/walk.gif");
    const fs::path outside = root().parent_path() / "outside-links";
    fs::remove_all(outside);
    fs::create_directories(outside);
    const fs::path abs = root() / fs::u8path(dir);
    fs::create_directory_symlink(outside, abs / "out");
    project()->setAssetsDir("assets");
    auto refusal = [&](const sn::Spec& spec, const std::string& field) {
        for (const sn::Problem& p : sn::checkSpec(root(), spec)) {
            if (p.kind == "output_invalid" && p.field == field) return p.message;
        }
        return std::string();
    };
    // the normal map, the height map, the frames JSON and the atlas
    sn::Spec normal = stillSpec(dir + "/hero.png");
    normal.output.normal = dir + "/out/hero_normal.png";
    const std::string message = refusal(normal, "output.normal");
    EXPECT_EQ(message, dir + "/out/hero_normal.png resolves outside the project's assets directory (assets): a symbolic link on its way leads to " +
                           jasperengine::editor::FileUtils::pathToGenericUtf8((fs::weakly_canonical(outside) / "hero_normal.png")) + ".");
    sn::Spec height = stillSpec(dir + "/hero.png");
    height.settings.height = true;
    height.output.height = dir + "/out/hero_height.png";
    EXPECT_NE(refusal(height, "output.height").find("symbolic link"), std::string::npos);
    sn::Spec frames = stillSpec(dir + "/hero.png");
    frames.frames.mode = sn::FramesSpec::Mode::Grid;
    frames.frames.grid.tileWidth = frames.frames.grid.tileHeight = 8;
    frames.output.framesJson = true;
    frames.output.framesJsonPath = dir + "/out/hero.frames.json";
    EXPECT_NE(refusal(frames, "output.frames_json").find("symbolic link"), std::string::npos);
    sn::Spec atlas = stillSpec(dir + "/walk.gif");
    atlas.output.atlas = dir + "/out/walk_sheet.png";
    EXPECT_NE(refusal(atlas, "output.atlas").find("symbolic link"), std::string::npos);
    // the default outputs of a source in the linked folder too (its paint would be stored there)
    fs::copy_file(abs / "hero.png", outside / "art.png");
    sn::Spec linkedSource = stillSpec(dir + "/out/art.png");
    EXPECT_NE(refusal(linkedSource, "output.normal").find("symbolic link"), std::string::npos);
    // a link to a folder of the assets is fine
    fs::create_directories(abs / "real");
    fs::create_directory_symlink(abs / "real", abs / "inside");
    sn::Spec inside = stillSpec(dir + "/hero.png");
    inside.output.normal = dir + "/inside/hero_normal.png";
    EXPECT_TRUE(sn::checkSpec(root(), inside).empty());
    // a link into a hidden directory of the project, with the whole project as assets
    project()->setAssetsDir(".");
    fs::create_directories(root() / ".jasperengine" / "links");
    fs::create_directory_symlink(root() / ".jasperengine" / "links", abs / "hidden");
    sn::Spec hidden = stillSpec(dir + "/hero.png");
    hidden.output.normal = dir + "/hidden/hero_normal.png";
    EXPECT_NE(refusal(hidden, "output.normal").find("hidden directory .jasperengine"), std::string::npos);
    EXPECT_NE(refusal(normal, "output.normal").find("symbolic link"), std::string::npos) << "outside the project too";
    // the undoable write checks again where it writes (a link made since the check): nothing is written
    sn::OutputFile late;
    late.role = "normal";
    late.path = dir + "/out/late.png";
    late.absolute = abs / "out" / "late.png";
    late.bytes = "bytes";
    std::string error;
    sn::Problem lateProblem;
    const size_t before = project()->getProjectCommandHistory()->position();
    EXPECT_FALSE(sn::writeOutputsUndoable(project(), {late}, error, &lateProblem));
    EXPECT_NE(error.find("symbolic link"), std::string::npos) << error;
    EXPECT_EQ(lateProblem.kind, "output_invalid");
    EXPECT_EQ(lateProblem.field, "output.normal");
    EXPECT_EQ(lateProblem.message, error);
    EXPECT_EQ(lateProblem.fix, "Write it to a folder of the assets directory that is not a link to elsewhere.");
    EXPECT_FALSE(fs::exists(outside / "late.png"));
    EXPECT_EQ(project()->getProjectCommandHistory()->position(), before);
    fs::remove_all(abs);
    fs::remove_all(outside);
    fs::remove_all(root() / ".jasperengine" / "links");
}

TEST(SpriteNormalsSession, ResolvedPathFollowsDanglingLinksAndRefusesLoops) {
    SKIP_WITHOUT_SYMLINKS();
    const fs::path base = root().parent_path() / "resolve-links";
    fs::remove_all(base);
    fs::create_directories(base / "real");
    const fs::path real = fs::weakly_canonical(base);
    // a dangling file link, a dangling folder link, a chain of relative links ending in nothing
    fs::create_symlink(real / "nowhere" / "x.png", base / "file.png");
    fs::create_directory_symlink(real / "gone", base / "dir");
    fs::create_symlink("chain2", base / "chain1");
    fs::create_symlink("real/../missing.png", base / "chain2");
    EXPECT_EQ(sn::resolvedPath(base / "file.png"), real / "nowhere" / "x.png");
    EXPECT_EQ(sn::resolvedPath(base / "dir" / "a" / "b.png"), real / "gone" / "a" / "b.png");
    EXPECT_EQ(sn::resolvedPath(base / "chain1"), real / "missing.png");
    EXPECT_EQ(sn::resolvedPath(base / "real" / "new.png"), real / "real" / "new.png") << "not there yet: lexical";
    EXPECT_FALSE(sn::isInsideResolved(base / "file.png", base / "real"));
    // a loop: nothing it could be inside of
    fs::create_symlink("loop2", base / "loop1");
    fs::create_symlink("loop1", base / "loop2");
    EXPECT_TRUE(sn::resolvedPath(base / "loop1" / "x.png").empty());
    EXPECT_FALSE(sn::isInsideResolved(base / "loop1" / "x.png", base));
    fs::remove_all(base);
}

TEST(SpriteNormalsSession, OutputsThroughADanglingLinkAreRefused) {
    SKIP_WITHOUT_SYMLINKS();
    const std::string dir = folder("dangling");
    writePng(dir + "/hero.png", balls(16, 16, 16, 6));
    copyWalkGif(dir + "/walk.gif");
    const fs::path abs = root() / fs::u8path(dir);
    const fs::path outside = root().parent_path() / "outside-dangling";
    fs::remove_all(outside);
    // file links to files that don't exist (yet) outside the project, and a folder link to a missing folder
    for (const char* name : {"hero_normal.png", "hero_height.png", "hero.frames.json", "walk_sheet.png"}) {
        fs::create_symlink(outside / name, abs / name);
    }
    fs::create_directory_symlink(outside / "folder", abs / "gone");
    project()->setAssetsDir("assets");
    auto refused = [&](const sn::Spec& spec, const std::string& field) {
        for (const sn::Problem& p : sn::checkSpec(root(), spec)) {
            if (p.kind == "output_invalid" && p.field == field && p.message.find("symbolic link") != std::string::npos) return true;
        }
        return false;
    };
    sn::Spec normal = stillSpec(dir + "/hero.png");
    normal.overwrite = true;
    EXPECT_TRUE(refused(normal, "output.normal")) << "the default normal map is a dangling link";
    sn::Spec height = normal;
    height.settings.height = true;
    height.output.normal = dir + "/n.png";
    EXPECT_TRUE(refused(height, "output.height"));
    sn::Spec frames = normal;
    frames.output.normal = dir + "/n.png";
    frames.frames.mode = sn::FramesSpec::Mode::Grid;
    frames.frames.grid.tileWidth = frames.frames.grid.tileHeight = 8;
    frames.output.framesJson = true;
    frames.output.framesJsonPath = dir + "/hero.frames.json";
    EXPECT_TRUE(refused(frames, "output.frames_json"));
    sn::Spec atlas = stillSpec(dir + "/walk.gif");
    atlas.overwrite = true;
    atlas.output.normal = dir + "/walk_n.png";
    EXPECT_TRUE(refused(atlas, "output.atlas"));
    sn::Spec folderLink = stillSpec(dir + "/hero.png");
    folderLink.output.normal = dir + "/gone/hero_normal.png";
    EXPECT_TRUE(refused(folderLink, "output.normal"));
    // the write-time check too, and nothing appears outside
    sn::OutputFile late;
    late.role = "normal";
    late.path = dir + "/hero_normal.png";
    late.absolute = abs / "hero_normal.png";
    late.bytes = "bytes";
    sn::Problem refusal;
    std::string error;
    EXPECT_FALSE(sn::writeOutputsUndoable(project(), {late}, error, &refusal));
    EXPECT_EQ(refusal.kind, "output_invalid");
    EXPECT_FALSE(sn::outputsContained(project(), {late}, refusal));
    EXPECT_FALSE(fs::exists(outside));
    project()->setAssetsDir(".");
    fs::remove_all(abs);
}

TEST(SpriteNormalsSession, RedoAndUndoNeverGoThroughALinkMadeInBetween) {
    SKIP_WITHOUT_SYMLINKS();
    const std::string dir = folder("redo");
    const fs::path abs = root() / fs::u8path(dir);
    fs::create_directories(abs / "sub");
    const fs::path outside = root().parent_path() / "outside-redo";
    fs::remove_all(outside);
    fs::create_directories(outside);
    sn::OutputFile file;
    file.role = "normal";
    file.path = dir + "/sub/x_normal.png";
    file.absolute = abs / "sub" / "x_normal.png";
    file.bytes = "first";
    CommandHistory* history = project()->getProjectCommandHistory();
    std::string error;
    ASSERT_TRUE(sn::writeOutputsUndoable(project(), {file}, error)) << error;
    ASSERT_TRUE(history->undo());
    EXPECT_FALSE(fs::exists(file.absolute));
    // the folder becomes a link out of the assets: the redo is refused (nothing written through
    // it) and the history stays where it is
    fs::remove_all(abs / "sub");
    fs::create_directory_symlink(outside, abs / "sub");
    const size_t undone = history->position();
    EXPECT_FALSE(history->redo()) << "a refused redo is not taken";
    EXPECT_EQ(history->position(), undone);
    EXPECT_TRUE(history->canRedo()) << "still there for when the link is gone";
    EXPECT_FALSE(fs::exists(outside / "x_normal.png")) << "the redo was not applied";
    // back to a folder: the redo goes through
    fs::remove(abs / "sub");
    fs::create_directories(abs / "sub");
    ASSERT_TRUE(history->redo());
    EXPECT_EQ(history->position(), undone + 1);
    EXPECT_EQ(readAll(file.absolute), "first");
    // a link again, a file of theirs behind it: the undo is refused (removes nothing through
    // it) and the history keeps its place, saying why and how to fix it
    fs::remove_all(abs / "sub");
    fs::create_directory_symlink(outside, abs / "sub");
    std::ofstream(outside / "x_normal.png") << "theirs";
    const size_t done = history->position();
    EXPECT_FALSE(history->undo()) << "a refused undo is not taken";
    EXPECT_EQ(history->position(), done);
    EXPECT_EQ(readAll(outside / "x_normal.png"), "theirs") << "the undo was not applied";
    EXPECT_NE(history->lastRefusal().find(dir + "/sub/x_normal.png"), std::string::npos) << history->lastRefusal();
    EXPECT_NE(history->lastRefusal().find("Remove the link, then undo again."), std::string::npos) << history->lastRefusal();
    EXPECT_EQ(history->undoRefusedLastTry(), history->lastRefusal()) << "the Edit menu's note";
    // a loop undoing to a mark stops at the refused step instead of spinning
    int tries = 0;
    while (history->position() > 0 && history->canUndo() && history->undo()) tries++;
    EXPECT_EQ(tries, 0);
    EXPECT_EQ(history->position(), done);
    // the link gone: the undo goes through
    fs::remove(abs / "sub");
    fs::create_directories(abs / "sub");
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(history->position(), done - 1);
    EXPECT_TRUE(history->lastRefusal().empty());
    EXPECT_TRUE(history->undoRefusedLastTry().empty());
    EXPECT_FALSE(fs::exists(file.absolute));
    fs::remove_all(abs);
    fs::remove_all(outside);
}

// The undoable write closes the gap between its check and its write: a folder on the way made a
// link after the guard passed is never followed (WriteFilesCmd::writeBeneath, util/SafeWrite.h)
TEST(SpriteNormalsSession, ALinkPlantedAfterTheCheckIsNotFollowed) {
    SKIP_WITHOUT_SYMLINKS();
    const std::string dir = folder("toctou");
    const fs::path abs = root() / fs::u8path(dir);
    fs::create_directories(abs / "sub");
    const fs::path outside = root().parent_path() / "outside-toctou";
    fs::remove_all(outside);
    fs::create_directories(outside);
    // the guard passes, then the folder becomes a link out of the project (as a racing process would)
    bool planted = false;
    auto plant = [&](std::string&) {
        if (!planted) {
            planted = true;
            fs::remove_all(abs / "sub");
            fs::create_directory_symlink(outside, abs / "sub");
        }
        return true;
    };
    std::string error;
    const size_t before = project()->getProjectCommandHistory()->position();
    EXPECT_FALSE(ProjectFileWrites::writeUndoable(project(), {{abs / "sub" / "x.png", "bytes"}}, error, nullptr, plant,
                                                  sn::outputsRoot(project())));
    EXPECT_NE(error.find("symbolic link"), std::string::npos) << error;
    EXPECT_FALSE(fs::exists(outside / "x.png")) << "nothing written through the link";
    EXPECT_EQ(project()->getProjectCommandHistory()->position(), before) << "no step";
    // the direct writer (the CLI, the watcher) too
    std::string failed;
    sn::OutputFile out{"normal", dir + "/sub/y.png", abs / "sub" / "y.png", "bytes"};
    EXPECT_FALSE(sn::writeOutputsDirect({out}, failed, error, sn::outputsRoot(project())));
    EXPECT_FALSE(fs::exists(outside / "y.png"));
    fs::remove_all(abs);
    fs::remove_all(outside);
}

TEST(SpriteNormalsSession, TheSheetReplacesALinkInsteadOfWritingThroughIt) {
    SKIP_WITHOUT_SYMLINKS();
    const std::string dir = folder("sheetlink");
    const fs::path abs = root() / fs::u8path(dir);
    const fs::path outside = root().parent_path() / "outside-sheet";
    fs::remove_all(outside);
    fs::create_directories(outside);
    fs::create_symlink(outside / "sheet.png", abs / "sheet.png");
    {
        mcp::SheetWrite sheet;
        std::string error;
        ASSERT_TRUE(sheet.write(root(), abs / "sheet.png", {1, 2, 3}, error)) << error;
        sheet.commit();
    }
    EXPECT_FALSE(fs::exists(outside / "sheet.png")) << "not written through the dangling link";
    EXPECT_FALSE(fs::is_symlink(abs / "sheet.png"));
    EXPECT_EQ(readAll(abs / "sheet.png"), std::string("\x01\x02\x03"));
    // a rollback of a replaced sheet puts its bytes back; nothing temporary is left
    {
        mcp::SheetWrite sheet;
        std::string error;
        ASSERT_TRUE(sheet.write(root(), abs / "sheet.png", {9}, error)) << error;
    }
    EXPECT_EQ(readAll(abs / "sheet.png"), std::string("\x01\x02\x03"));
    EXPECT_EQ(std::distance(fs::directory_iterator(abs), fs::directory_iterator()), 1);
    fs::remove_all(abs);
    fs::remove_all(outside);
}

TEST(SpriteNormalsSession, AnOutputThatIsTheSourceThroughALinkIsRefused) {
    SKIP_WITHOUT_SYMLINKS();
    const std::string dir = folder("alias");
    writePng(dir + "/hero.png", balls(16, 16, 16, 6));
    const fs::path abs = root() / fs::u8path(dir);
    fs::create_symlink("hero.png", abs / "hero_link.png");
    fs::create_directory_symlink(abs, abs.parent_path() / "alias_dir");
    sn::Spec spec = stillSpec(dir + "/hero.png");
    spec.overwrite = true;
    for (const std::string& out : {dir + "/hero_link.png", std::string("assets/alias_dir/hero.png")}) {
        spec.output.normal = out;
        std::optional<sn::Problem> p = findKind(sn::checkSpec(root(), spec), "output_invalid");
        ASSERT_TRUE(p) << out;
        EXPECT_NE(p->message.find("would replace the source"), std::string::npos) << out << ": " << p->message;
    }
    // two outputs that are one file through a link
    spec.output.normal = dir + "/hero_n.png";
    spec.settings.height = true;
    spec.output.height = "assets/alias_dir/hero_n.png";
    std::optional<sn::Problem> p = findKind(sn::checkSpec(root(), spec), "output_invalid");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->field, "output.height");
    EXPECT_NE(p->message.find("Two outputs"), std::string::npos) << p->message;
    fs::remove(abs.parent_path() / "alias_dir");
    fs::remove_all(abs);
}

TEST(SpriteNormalsSession, ALinkedAssetsDirectoryOrProjectKeepsWorking) {
    SKIP_WITHOUT_SYMLINKS();
    const std::string dir = folder("linkedroot");
    writePng(dir + "/hero.png", balls(16, 16, 16, 6));
    // the assets directory is a link to a folder outside the project
    const fs::path real = root().parent_path() / "real-assets";
    fs::remove_all(real);
    fs::create_directories(real / "art");
    fs::copy_file(root() / fs::u8path(dir) / "hero.png", real / "art" / "hero.png");
    fs::remove(root() / "linked");
    fs::create_directory_symlink(real, root() / "linked");
    project()->setAssetsDir("linked");
    sn::Spec spec = stillSpec("linked/art/hero.png");
    EXPECT_TRUE(sn::checkSpec(root(), spec).empty()) << "the default output, inside the linked assets directory";
    spec.output.normal = "assets/elsewhere.png";
    EXPECT_TRUE(findKind(sn::checkSpec(root(), spec), "output_invalid")) << "outside the assets directory, as before";
    // its undoable write goes through
    spec.output.normal.clear();
    std::vector<sn::OutputFile> files = generateFiles(spec);
    ASSERT_EQ(files.size(), 1u);
    std::string error;
    ASSERT_TRUE(sn::writeOutputsUndoable(project(), files, error)) << error;
    EXPECT_TRUE(fs::is_regular_file(real / "art" / "hero_normal.png"));
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_FALSE(fs::exists(real / "art" / "hero_normal.png"));
    // the assets directory is a trusted root; a link BELOW it leading further out is refused
    const fs::path further = root().parent_path() / "further-out";
    fs::remove_all(further);
    fs::create_directories(further);
    fs::create_directory_symlink(further, real / "out");
    sn::Spec escape = stillSpec("linked/art/hero.png");
    escape.output.normal = "linked/out/hero_normal.png";
    std::optional<sn::Problem> refused = findKind(sn::checkSpec(root(), escape), "output_invalid");
    ASSERT_TRUE(refused) << "a link below the linked assets directory, out of it";
    EXPECT_NE(refused->message.find("symbolic link"), std::string::npos) << refused->message;
    sn::OutputFile late;
    late.role = "normal";
    late.path = "linked/out/late.png";
    late.absolute = root() / "linked" / "out" / "late.png";
    late.bytes = "bytes";
    EXPECT_FALSE(sn::writeOutputsUndoable(project(), {late}, error));
    EXPECT_FALSE(fs::exists(further / "late.png"));
    fs::remove(real / "out");
    fs::remove_all(further);
    project()->setAssetsDir(".");
    // the project reached through a link
    const fs::path via = root().parent_path() / "via-link";
    fs::remove(via);
    fs::create_directory_symlink(root(), via);
    sn::Spec through = stillSpec(dir + "/hero.png");
    EXPECT_TRUE(sn::checkSpec(via, through).empty());
    project()->setAssetsDir("assets");
    EXPECT_TRUE(sn::checkSpec(via, through).empty()) << "assets/ named through the project's link";
    project()->setAssetsDir(".");
    fs::remove(via);
    fs::remove(root() / "linked");
    fs::remove_all(real);
}

// ---- prepare, generate, encode

TEST(SpriteNormalsSession, StillPrepareGenerateEncode) {
    const std::string dir = folder("still");
    writePng(dir + "/hero.png", balls(32, 32, 32, 12));
    sn::Spec spec = stillSpec(dir + "/hero.png");
    spec.settings.height = true;
    spec.output.framesJson = true;
    sn::Prepared prepared;
    const std::vector<sn::OutputFile> files = generateFiles(spec, nullptr, &prepared);
    EXPECT_EQ(prepared.framesFrom, sn::FramesFrom::Single);
    EXPECT_TRUE(prepared.request.cells.empty());
    EXPECT_FALSE(prepared.request.sequence);
    ASSERT_EQ(files.size(), 3u);
    EXPECT_EQ(files[0].role, "normal");
    EXPECT_EQ(files[0].path, dir + "/hero_normal.png");
    EXPECT_EQ(files[0].absolute, root() / "assets" / "still" / "hero_normal.png");
    const json normal = recipeOf(files[0]);
    EXPECT_EQ(normal["role"], "normal");
    EXPECT_EQ(normal["source"]["path"], dir + "/hero.png");
    EXPECT_EQ(normal["source"]["kind"], "image");
    EXPECT_EQ(normal["source"]["sha1"], ts::sha1Hex(reinterpret_cast<const uint8_t*>(readAll(root() / fs::u8path(dir) / "hero.png").data()),
                                                    readAll(root() / fs::u8path(dir) / "hero.png").size()));
    EXPECT_EQ(normal["outputs"]["normal"], dir + "/hero_normal.png");
    EXPECT_EQ(normal["outputs"]["height"], dir + "/hero_height.png");
    EXPECT_TRUE(normal["outputs"]["atlas"].is_null());
    EXPECT_EQ(normal["radius_resolved"], "per-island");
    EXPECT_TRUE(normal.contains("height_range_px"));
    const json height = recipeOf(*role(files, "height"));
    EXPECT_EQ(height["role"], "height");
    // the frames JSON of a still: no animation
    const json frames = json::parse(role(files, "frames_json")->bytes);
    EXPECT_EQ(role(files, "frames_json")->path, dir + "/hero_normal.frames.json");
    std::vector<std::string> keys;
    for (auto it = frames.begin(); it != frames.end(); ++it) keys.push_back(it.key());
    EXPECT_EQ(keys, (std::vector<std::string>{"animation", "height", "settings", "source", "version", "width"}));
    EXPECT_EQ(frames["version"], 1);
    EXPECT_EQ(frames["width"], 32);
    EXPECT_TRUE(frames["animation"].is_null());
    EXPECT_EQ(frames["source"], dir + "/hero.png");
    EXPECT_EQ(frames["settings"]["height"], true);
}

TEST(SpriteNormalsSession, GifPrepareGenerateEncode) {
    const std::string dir = folder("gif");
    copyWalkGif(dir + "/walk.gif");
    sn::Spec spec = stillSpec(dir + "/walk.gif");
    spec.output.framesJson = true;
    sn::Prepared prepared;
    const std::vector<sn::OutputFile> files = generateFiles(spec, nullptr, &prepared);
    EXPECT_EQ(prepared.source.kind, sn::SourceKind::Gif);
    EXPECT_EQ(prepared.framesFrom, sn::FramesFrom::Gif);
    EXPECT_EQ(prepared.request.cells.size(), 8u);
    EXPECT_TRUE(prepared.request.sequence);
    ASSERT_TRUE(role(files, "normal") && role(files, "atlas") && role(files, "frames_json"));
    EXPECT_EQ(role(files, "normal")->path, dir + "/walk_sheet_normal.png");
    EXPECT_EQ(role(files, "atlas")->path, dir + "/walk_sheet.png");
    const json normal = recipeOf(*role(files, "normal"));
    EXPECT_EQ(normal["source"]["kind"], "gif");
    EXPECT_EQ(normal["frames"]["from"], "gif");
    EXPECT_EQ(normal["frames"]["cells"].size(), 8u);
    EXPECT_EQ(normal["frames"]["columns"], 3);
    EXPECT_EQ(normal["frames"]["delays_ms"].size(), 8u);
    EXPECT_TRUE(normal["radius_resolved"].is_number()) << "a sequence has one radius";
    EXPECT_EQ(normal["outputs"]["atlas"], dir + "/walk_sheet.png");
    const json atlas = recipeOf(*role(files, "atlas"));
    EXPECT_EQ(atlas["role"], "atlas");
    // the atlas PNG is the decoded colour atlas
    ts::Image decoded;
    std::string error;
    const std::string& bytes = role(files, "atlas")->bytes;
    ASSERT_TRUE(ts::loadImageFromMemory(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), decoded, error)) << error;
    EXPECT_EQ(ts::toRgba8(decoded).data, prepared.source.color.data);
    // the frames JSON: upstream's schema plus version, rows, loop, frame_ms, source, settings
    const json frames = json::parse(role(files, "frames_json")->bytes);
    EXPECT_EQ(role(files, "frames_json")->path, dir + "/walk_sheet.frames.json");
    const json& a = frames["animation"];
    std::vector<std::string> keys;
    for (auto it = a.begin(); it != a.end(); ++it) keys.push_back(it.key());
    EXPECT_EQ(keys, (std::vector<std::string>{"columns", "delays_ms", "frame_count", "frame_height", "frame_ms", "frame_width", "loop", "rows"}));
    EXPECT_EQ(a["frame_count"], 8);
    EXPECT_EQ(a["columns"], 3);
    EXPECT_EQ(a["rows"], 3);
    EXPECT_EQ(a["frame_width"], prepared.source.frameWidth);
    EXPECT_EQ(a["delays_ms"].size(), 8u);
    ASSERT_EQ(a["frame_ms"].size(), 8u);
    EXPECT_TRUE(a["frame_ms"][0].is_number_float());
    EXPECT_TRUE(a.contains("loop"));
    EXPECT_EQ(frames["source"], dir + "/walk.gif");
    EXPECT_TRUE(frames["settings"].is_object());
}

// ---- writes

TEST(SpriteNormalsSession, WriteOutputsDirectIsAllOrNothing) {
    const std::string dir = folder("direct");
    writePng(dir + "/hero.png", balls(32, 32, 32, 12));
    std::vector<sn::OutputFile> files = generateFiles(stillSpec(dir + "/hero.png"));
    ASSERT_EQ(files.size(), 1u);
    // a second file under a path component that is a file: nothing is written
    std::ofstream(root() / fs::u8path(dir) / "blocker") << "a file";
    sn::OutputFile blocked = files[0];
    blocked.path = dir + "/blocker/x.png";
    blocked.absolute = root() / fs::u8path(dir) / "blocker" / "x.png";
    std::vector<sn::OutputFile> set = {files[0], blocked};
    std::string failed, error;
    EXPECT_FALSE(sn::writeOutputsDirect(set, failed, error));
    EXPECT_NE(failed.find("blocker"), std::string::npos) << failed;
    EXPECT_NE(error.find("nothing was written"), std::string::npos) << error;
    EXPECT_FALSE(fs::exists(files[0].absolute));
    // the set alone goes through
    ASSERT_TRUE(sn::writeOutputsDirect(files, failed, error)) << error;
    EXPECT_EQ(readAll(files[0].absolute), files[0].bytes);
}

TEST(SpriteNormalsSession, WriteOutputsUndoableNeedsAProject) {
    // no editor project: refused with a message, never a crash
    std::string error;
    EXPECT_FALSE(sn::writeOutputsUndoable(nullptr, {}, error));
    EXPECT_NE(error.find("writeOutputsDirect"), std::string::npos) << error;
}

namespace {

    // In a child process (the editor's App is set up for good; no MCP server): the set is one
    // step of the project history, undo removes all of it, redo puts it back
    void undoableWriteInTheEditor(const fs::path& dir) {
        App& app = Backend::getApp();
        Project* p = app.getProject();
        editor::Platform platform(p);
        // the test binary's engine is TestMain's (a second Engine::systemInit would restart its jobs
        // and logger): this System is swapped in
        jasperengine::SystemTestAccess::swap(&platform);
        if (!p->loadProject(dir, false)) std::exit(2);
        // no MCP server: the undoable write needs none (ProjectFileWrites)
        if (mcp::McpServer::get().isStarted()) std::exit(20);
        writeBytes(dir / "assets" / "undo" / "hero.png", [] {
            std::vector<uint8_t> png;
            ts::encodePng(balls(32, 32, 32, 12), png);
            return png;
        }());
        sn::Spec spec = stillSpec("assets/undo/hero.png");
        spec.settings.height = true;
        sn::Prepared prepared;
        std::vector<sn::Problem> problems;
        if (!sn::prepare(dir, spec, nullptr, prepared, problems)) std::exit(3);
        const sn::Result result = sn::generate(prepared.request);
        std::vector<sn::OutputFile> files;
        std::string error;
        if (result.status != sn::Status::Ok || !sn::encodeOutputs(prepared, result, files, error) || files.size() != 2) std::exit(4);
        CommandHistory* history = p->getProjectCommandHistory();
        const size_t before = history->position();
        if (!sn::writeOutputsUndoable(p, files, error)) std::exit(5);
        if (history->position() != before + 1) std::exit(6);
        for (const sn::OutputFile& f : files) {
            if (readAll(f.absolute) != f.bytes) std::exit(7);
        }
        if (!history->undo()) std::exit(8);
        for (const sn::OutputFile& f : files) {
            if (fs::exists(f.absolute)) std::exit(9);
        }
        if (!history->redo() || readAll(files[1].absolute) != files[1].bytes) std::exit(10);
        std::fflush(stdout);
        // the forked editor ends without its static destructors: its jobs (thumbnails, imports)
        // may still read the App while it would be destroyed
        std::_Exit(0);
    }

}

namespace {

    // the App's watcher, ticked until its walk ends dormant (no generated map) or not; false on time out
    bool settleWatcher(SpriteNormalsWatcher& watcher, bool dormant) {
        for (int i = 0; i < 2000; i++) {
            watcher.tick();
            if (watcher.dormant() == dormant && i > 2) return true;
            usleep(5000);
        }
        return false;
    }

    // In a child process (the App with its watcher, no MCP server): the undoable write, its undo
    // and its redo each wake the watcher (writeOutputsUndoable, and the write command's refresh:
    // ProjectFileWrites::refresh)
    void watcherWakesInTheEditor(const fs::path& dir) {
        App& app = Backend::getApp();
        Project* p = app.getProject();
        editor::Platform platform(p);
        // the test binary's engine is TestMain's (a second Engine::systemInit would restart its jobs
        // and logger): this System is swapped in
        jasperengine::SystemTestAccess::swap(&platform);
        if (!p->loadProject(dir, false)) std::exit(2);
        // no MCP server: the undoable write needs none (ProjectFileWrites)
        if (mcp::McpServer::get().isStarted()) std::exit(20);
        SpriteNormalsWatcher* watcher = app.getSpriteNormalsWatcher();
        if (!watcher || SpriteNormalsWatcher::active() != watcher) std::exit(3);
        writeBytes(dir / "assets" / "wake" / "hero.png", [] {
            std::vector<uint8_t> png;
            ts::encodePng(balls(32, 32, 32, 12), png);
            return png;
        }());
        if (!settleWatcher(*watcher, true)) std::exit(4);
        sn::Prepared prepared;
        std::vector<sn::Problem> problems;
        if (!sn::prepare(dir, stillSpec("assets/wake/hero.png"), nullptr, prepared, problems)) std::exit(5);
        const sn::Result result = sn::generate(prepared.request);
        std::vector<sn::OutputFile> files;
        std::string error;
        if (result.status != sn::Status::Ok || !sn::encodeOutputs(prepared, result, files, error)) std::exit(6);
        // the first generation wakes it
        if (!sn::writeOutputsUndoable(p, files, error)) std::exit(7);
        if (watcher->dormant()) std::exit(8);
        // its walk indexes the map (not dormant)
        std::string indexedSource, indexedRole;
        bool first = false;
        for (int i = 0; i < 2000 && !first; i++) {
            watcher->tick();
            first = watcher->generatedInfo(files[0].absolute, indexedSource, indexedRole);
            if (!first) usleep(5000);
        }
        if (!first || watcher->dormant()) std::exit(9);
        // undone: the map is gone, the next walk ends dormant
        CommandHistory* history = p->getProjectCommandHistory();
        if (!history->undo()) std::exit(10);
        if (!settleWatcher(*watcher, true)) std::exit(11);
        // redone: the write command's refresh wakes it at once
        if (!history->redo()) std::exit(12);
        if (watcher->dormant()) std::exit(13);
        // and its next walk indexes the map again
        std::string source, role;
        bool indexed = false;
        for (int i = 0; i < 2000 && !indexed; i++) {
            watcher->tick();
            indexed = watcher->generatedInfo(files[0].absolute, source, role);
            if (!indexed) usleep(5000);
        }
        if (!indexed || watcher->dormant()) std::exit(15);
        std::fflush(stdout);
        // the forked editor ends without its static destructors: its jobs (thumbnails, imports)
        // may still read the App while it would be destroyed
        std::_Exit(0);
    }

}

TEST(SpriteNormalsSessionDeathTest, TheWatcherWakesOnTheUndoableWriteItsUndoAndRedo) {
    const fs::path dir = fs::temp_directory_path() / ("jasper-spritenormals-wake-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / ".jasperengine");
    std::ofstream(dir / "project.yaml") << "name: wake\n";
    GTEST_FLAG_SET(death_test_style, "threadsafe"); EXPECT_EXIT(watcherWakesInTheEditor(dir), ::testing::ExitedWithCode(0), "");
    fs::remove_all(dir);
}

TEST(SpriteNormalsSessionDeathTest, WriteOutputsUndoableIsOneProjectStep) {
    const fs::path dir = fs::temp_directory_path() / ("jasper-spritenormals-undo-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / ".jasperengine");
    std::ofstream(dir / "project.yaml") << "name: undo\n";
    GTEST_FLAG_SET(death_test_style, "threadsafe"); EXPECT_EXIT(undoableWriteInTheEditor(dir), ::testing::ExitedWithCode(0), "");
    fs::remove_all(dir);
}

// ---- staleness and recipes

TEST(SpriteNormalsSession, StalenessFreshChangedMissing) {
    const std::string dir = folder("stale");
    writePng(dir + "/hero.png", balls(32, 32, 32, 12));
    std::vector<sn::OutputFile> files = generateFiles(stillSpec(dir + "/hero.png"));
    std::string failed, error;
    ASSERT_TRUE(sn::writeOutputsDirect(files, failed, error)) << error;
    const fs::path map = files[0].absolute;

    sn::Staleness s = sn::checkStaleness(root(), map);
    EXPECT_EQ(s.state, sn::Staleness::State::Fresh) << s.message;
    EXPECT_EQ(s.sourcePath, dir + "/hero.png");
    EXPECT_EQ(s.sourceAbsolute, root() / "assets" / "stale" / "hero.png");
    writePng(dir + "/hero.png", balls(32, 32, 32, 10));
    s = sn::checkStaleness(root(), map);
    EXPECT_EQ(s.state, sn::Staleness::State::Changed);
    EXPECT_EQ(s.message, "hero.png changed since this map was made.");
    EXPECT_STREQ(sn::toString(s.state), "changed");
    fs::remove(root() / fs::u8path(dir) / "hero.png");
    s = sn::checkStaleness(root(), map);
    EXPECT_EQ(s.state, sn::Staleness::State::Missing);
    EXPECT_STREQ(sn::toString(s.state), "missing");

    // no recipe, a garbled one
    writePng(dir + "/plain.png", balls(8, 8, 8, 3));
    EXPECT_EQ(sn::checkStaleness(root(), root() / fs::u8path(dir) / "plain.png").state, sn::Staleness::State::NoRecipe);
    std::vector<uint8_t> png, garbled;
    ASSERT_TRUE(ts::encodePng(balls(8, 8, 8, 3), png));
    ASSERT_TRUE(ts::insertRecipeChunk(png, R"({"format":"jasperengine.spritenormals","version":1,"source":{"path":5}})", garbled, error, sn::kRecipeKeyword));
    writeBytes(root() / fs::u8path(dir) / "garbled.png", garbled);
    s = sn::checkStaleness(root(), root() / fs::u8path(dir) / "garbled.png");
    EXPECT_EQ(s.state, sn::Staleness::State::Invalid);
    EXPECT_NE(s.message.find("source.path is a number"), std::string::npos) << s.message;
}

TEST(SpriteNormalsSession, SpecFromRecipeRoundTrip) {
    const std::string dir = folder("roundtrip");
    writePng(dir + "/sheet.png", balls(64, 32, 32, 12));
    // non-default settings, a grid of two frames
    sn::Spec spec = stillSpec(dir + "/sheet.png");
    spec.settings = sn::presetSettings(sn::Preset::Chunky);
    spec.settings.profile = sn::Profile::Dome;
    spec.settings.height = true;
    spec.settings.regions.push_back({{0x3c3c3cu}, 0.04f, 0.0f, 1.5f, false});
    spec.frames.mode = sn::FramesSpec::Mode::Grid;
    spec.frames.grid.columns = 2;
    spec.frames.grid.rows = 1;
    const std::vector<sn::OutputFile> files = generateFiles(spec);
    std::string failed, error;
    ASSERT_TRUE(sn::writeOutputsDirect(files, failed, error)) << error;

    const fs::path map = root() / fs::u8path(dir) / "sheet_normal.png";
    const std::optional<json> recipe = sn::readRecipe(map, &error);
    ASSERT_TRUE(recipe) << error;
    sn::Spec back;
    std::vector<std::string> notes;
    ASSERT_TRUE(sn::specFromRecipe(*recipe, map, root(), back, notes, error)) << error;
    EXPECT_TRUE(notes.empty());
    EXPECT_EQ(back.source, dir + "/sheet.png");
    EXPECT_TRUE(back.overwrite);
    EXPECT_EQ(back.output.normal, dir + "/sheet_normal.png");
    EXPECT_EQ(back.output.height, dir + "/sheet_height.png");
    EXPECT_EQ(sn::settingsToJson(back.settings), sn::settingsToJson(spec.settings));
    // the grid comes back as its cells, and regenerates the same bytes
    EXPECT_EQ(back.frames.mode, sn::FramesSpec::Mode::Cells);
    EXPECT_EQ(back.frames.cells.size(), 2u);
    const std::vector<sn::OutputFile> again = generateFiles(back);
    ASSERT_TRUE(role(again, "normal"));
    EXPECT_EQ(role(again, "normal")->bytes, role(files, "normal")->bytes);

    // frames auto: the same fingerprint after the trip
    sn::Spec plain = stillSpec(dir + "/sheet.png");
    plain.settings.strength = 2.25f;
    plain.overwrite = true;
    const std::vector<sn::OutputFile> plainFiles = generateFiles(plain);
    ASSERT_TRUE(sn::writeOutputsDirect(plainFiles, failed, error)) << error;
    ASSERT_TRUE(sn::specFromRecipe(*sn::readRecipe(map, &error), map, root(), back, notes, error)) << error;
    EXPECT_EQ(back.fingerprint(), plain.fingerprint());
    EXPECT_NE(back.fingerprint(), spec.fingerprint());

    // garbled or foreign recipes: errors, never exceptions
    bool ok = true;
    EXPECT_NO_THROW(ok = sn::specFromRecipe(json::parse(R"({"format":1,"mode":"tileable"})"), map, root(), back, notes, error));
    EXPECT_FALSE(ok);
    EXPECT_EQ(error, "not a sprite normals recipe");
    EXPECT_NO_THROW(ok = sn::specFromRecipe(json::parse(R"({"format":"jasperengine.spritenormals","version":1,"source":{"path":"a.png"},"settings":{"edges":"torus"}})"),
                                            map, root(), back, notes, error));
    EXPECT_FALSE(ok);
    EXPECT_NE(error.find("garbled"), std::string::npos) << error;
}

// A version 1 recipe (written before high-resolution support) regenerates with version 1 rules and
// its own profile; a version 2 recipe records profile and resolution_scale
TEST(SpriteNormalsSession, VersionOneRecipesKeepTheirRules) {
    std::vector<std::string> notes;
    std::string error;
    sn::Spec back;
    const fs::path map = root() / "assets" / "old_normal.png";
    const json old = json::parse(R"({"format":"jasperengine.spritenormals","version":1,"source":{"path":"assets/old.png"},)"
                                 R"("settings":{"preset":"default","strength":1.5,"detail":3,"smoothing":1,"volume":1.15,"radius":"auto",)"
                                 R"("softness":1.15,"alpha_threshold":0.03,"edges":"transparent","ignore_outline":false,"regions":[],)"
                                 R"("directx":false,"height":false}})");
    ASSERT_TRUE(sn::specFromRecipe(old, map, root(), back, notes, error)) << error;
    EXPECT_EQ(back.settings.version, 1);
    EXPECT_EQ(back.settings.profile, sn::Profile::Rounded) << "a version 1 recipe without a profile is rounded";
    EXPECT_EQ(sn::settingsToJson(back.settings)["version"], 1);
    json v2 = old;
    v2["version"] = 2;
    v2["settings"]["profile"] = "auto";
    v2["settings"]["resolution_scale"] = "off";
    ASSERT_TRUE(sn::specFromRecipe(v2, map, root(), back, notes, error)) << error;
    EXPECT_EQ(back.settings.version, 2);
    EXPECT_EQ(back.settings.profile, sn::Profile::Auto);
    EXPECT_EQ(back.settings.resolutionScale, sn::ResolutionScale::Off);
    EXPECT_FALSE(sn::settingsToJson(back.settings).contains("version"));
}

TEST(SpriteNormalsSession, FingerprintFollowsWhatChangesTheResult) {
    sn::Spec a = stillSpec("assets/hero.png");
    sn::Spec b = a;
    b.output.normal = "assets/elsewhere.png";
    b.overwrite = true;
    EXPECT_EQ(a.fingerprint(), b.fingerprint()) << "outputs do not change the result";
    b.settings.detail = 4.0f;
    EXPECT_NE(a.fingerprint(), b.fingerprint());
    b = a;
    b.frames.mode = sn::FramesSpec::Mode::None;
    EXPECT_NE(a.fingerprint(), b.fingerprint());
    b = a;
    b.source = "assets/caf\xE9.png";
    EXPECT_NO_THROW(b.fingerprint());
    EXPECT_NE(a.fingerprint(), b.fingerprint());
}

// ---- the worker

namespace {

    // tens of thousands of islands: seconds of work, cancel is checked before each
    sn::Prepared heavy() {
        sn::Prepared p;
        p.request.color = sn::Image::make(2048, 2048, 4, 8);
        for (int y = 0; y < 2048; y++) {
            for (int x = 0; x < 2048; x++) {
                if ((x % 8) < 4 && (y % 8) < 4) {
                    uint8_t* px = p.request.color.pixel(x, y);
                    px[0] = static_cast<uint8_t>(x);
                    px[1] = static_cast<uint8_t>(y);
                    px[2] = 90;
                    px[3] = 255;
                }
            }
        }
        p.request.settings = sn::presetSettings(sn::Preset::Default);
        p.request.threads = 1;
        return p;
    }

    void shutdownWhileRunning() {
        std::atomic<int> called{0};
        std::vector<std::shared_ptr<sn::SpriteNormalsRun>> runs;
        for (int i = 0; i < 3; i++) runs.push_back(sn::SpriteNormalsRun::start(heavy(), 1, [&called](sn::SpriteNormalsRun&) { called++; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const bool stopped = sn::SpriteNormalsRun::cancelAll(10.0);
        bool cancelled = true;
        for (const auto& run : runs) cancelled = cancelled && run->finished() && run->result().status == sn::Status::Cancelled;
        std::exit(stopped && cancelled && called == 0 ? 0 : 1);
    }

}

TEST(SpriteNormalsRun, FinishesCallsBackAndCancels) {
    const std::string dir = folder("run");
    writePng(dir + "/hero.png", balls(32, 32, 32, 12));
    sn::Prepared prepared;
    std::vector<sn::Problem> problems;
    ASSERT_TRUE(sn::prepare(root(), stillSpec(dir + "/hero.png"), nullptr, prepared, problems));
    std::atomic<int> calls{0};
    std::shared_ptr<sn::SpriteNormalsRun> run = sn::SpriteNormalsRun::start(prepared, 2, [&calls](sn::SpriteNormalsRun& done) {
        EXPECT_TRUE(done.finished());
        calls++;
    });
    EXPECT_EQ(run->generation(), ts::projectGeneration());
    for (int i = 0; i < 500 && calls == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(run->finished());
    EXPECT_EQ(run->result().status, sn::Status::Ok);
    EXPECT_EQ(run->result().normal.width, 32);
    EXPECT_EQ(run->prepared().sourcePath, dir + "/hero.png");
    EXPECT_EQ(run->prepared().request.threads, 2);

    // cancel: the run ends cancelled, still calling back
    std::shared_ptr<sn::SpriteNormalsRun> big = sn::SpriteNormalsRun::start(heavy(), 1, [&calls](sn::SpriteNormalsRun&) { calls++; });
    big->cancel();
    for (int i = 0; i < 500 && !big->finished(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(big->finished());
    EXPECT_EQ(big->result().status, sn::Status::Cancelled);
    for (int i = 0; i < 100 && calls < 2; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(calls, 2);
    // cancelRunning (a project switch) reaches every run
    std::shared_ptr<sn::SpriteNormalsRun> other = sn::SpriteNormalsRun::start(heavy(), 1);
    sn::SpriteNormalsRun::cancelRunning();
    for (int i = 0; i < 500 && !other->finished(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(other->result().status, sn::Status::Cancelled);
}

TEST(SpriteNormalsRun, AProjectSwitchMakesItsResultStale) {
    const std::string dir = folder("generation");
    writePng(dir + "/hero.png", balls(16, 16, 16, 6));
    sn::Prepared prepared;
    std::vector<sn::Problem> problems;
    ASSERT_TRUE(sn::prepare(root(), stillSpec(dir + "/hero.png"), nullptr, prepared, problems));
    std::shared_ptr<sn::SpriteNormalsRun> run = sn::SpriteNormalsRun::start(prepared);
    for (int i = 0; i < 500 && !run->finished(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(run->finished());
    EXPECT_EQ(run->generation(), ts::projectGeneration()) << "the writer may write it";
    ts::projectSwitching();
    EXPECT_NE(run->generation(), ts::projectGeneration()) << "the writer refuses it (project_changed)";
    EXPECT_STREQ(sn::kProjectChangedMessage, "Another project was opened while generating.");
}

TEST(SpriteNormalsRunDeathTest, ShutdownCancelsWaitsAndCallsNothingBack) {
    GTEST_FLAG_SET(death_test_style, "threadsafe"); EXPECT_EXIT(shutdownWhileRunning(), ::testing::ExitedWithCode(0), "");
}

// ---- the scene

TEST(SpriteNormalsScene, FramesFromSpriteAndSpriteSources) {
    const std::string dir = folder("sprite");
    writePng(dir + "/sheet.png", balls(64, 32, 32, 12));
    const uint32_t sceneId = newScene();
    const Entity hero = sprite(sceneId, dir + "/sheet.png");
    Scene* scene = sceneOf(sceneId);

    std::string texture, error;
    std::vector<sn::Cell> cells;
    ASSERT_TRUE(sn::framesFromSprite(scene, hero, 0, texture, cells, error)) << error;
    EXPECT_EQ(texture, dir + "/sheet.png");
    EXPECT_TRUE(cells.empty());
    // pixel frames, and normalized ones (fractions of the texture)
    SpriteComponent& sc = scene->getComponent<SpriteComponent>(hero);
    sc.framesRect[0] = {"a", Rect(0, 0, 32, 32)};
    sc.framesRect[1] = {"b", Rect(0.5f, 0.0f, 0.5f, 1.0f)};
    sc.numFramesRect = 2;
    ASSERT_TRUE(sn::framesFromSprite(scene, hero, 0, texture, cells, error)) << error;
    EXPECT_EQ(cells, (std::vector<sn::Cell>{{0, 0, 32, 32}, {32, 0, 32, 32}}));

    // not a sprite: no mesh
    const Entity empty = create(sceneId, "Empty", EntityCreationType::GAMEOBJECT);
    EXPECT_FALSE(sn::framesFromSprite(scene, empty, 0, texture, cells, error));
    EXPECT_NE(error.find("no mesh"), std::string::npos) << error;

    // a Sprite as the source: its texture, its frames (a sequence), the recipe names it
    sn::Spec spec;
    spec.sceneId = sceneId;
    spec.entity = hero;
    spec.settings = sn::presetSettings(sn::Preset::Crisp);
    EXPECT_TRUE(sn::checkSpec(root(), spec, scene).empty());
    EXPECT_TRUE(findKind(sn::checkSpec(root(), spec, nullptr), "not_a_sprite")) << "its scene must be given";
    sn::Prepared prepared;
    const std::vector<sn::OutputFile> files = generateFiles(spec, scene, &prepared);
    EXPECT_EQ(prepared.sourcePath, dir + "/sheet.png");
    EXPECT_EQ(prepared.framesFrom, sn::FramesFrom::Sprite);
    EXPECT_TRUE(prepared.request.sequence);
    EXPECT_EQ(prepared.spriteEntityPath, "Hero");
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].path, dir + "/sheet_normal.png");
    const json recipe = recipeOf(files[0]);
    EXPECT_EQ(recipe["source"]["kind"], "sprite");
    EXPECT_EQ(recipe["source"]["entity"], "Hero");
    EXPECT_EQ(recipe["frames"]["from"], "sprite");
    // back to a spec: the texture is the source, with a note
    sn::Spec back;
    std::vector<std::string> notes;
    ASSERT_TRUE(sn::specFromRecipe(recipe, root() / fs::u8path(files[0].path), root(), back, notes, error)) << error;
    EXPECT_EQ(back.source, dir + "/sheet.png");
    EXPECT_FALSE(back.hasSprite());
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_NE(notes[0].find("Hero"), std::string::npos);
    EXPECT_EQ(back.frames.cells, prepared.request.cells);
    // frames none: the whole image
    spec.frames.mode = sn::FramesSpec::Mode::None;
    generateFiles(spec, scene, &prepared);
    EXPECT_EQ(prepared.framesFrom, sn::FramesFrom::Single);
    EXPECT_TRUE(prepared.request.cells.empty());
}

TEST(SpriteNormalsScene, LightCheck) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    scene->setAmbientLight2D(0.2f);
    sn::LightCheck c = sn::lightCheck(scene);
    EXPECT_EQ(c.lights, 0);
    EXPECT_FLOAT_EQ(c.ambientIntensity, 0.2f);
    const Entity light = create(sceneId, "Light", EntityCreationType::POINT_LIGHT_2D);
    Light2DComponent& l = scene->getComponent<Light2DComponent>(light);
    l.height = 0.0f;
    c = sn::lightCheck(scene);
    EXPECT_EQ(c.lights, 1);
    EXPECT_EQ(c.lightsWithHeight, 0);
    l.height = 64.0f;
    c = sn::lightCheck(scene);
    EXPECT_EQ(c.lightsWithHeight, 1);
    l.enabled = false;
    EXPECT_EQ(sn::lightCheck(scene).lights, 0) << "disabled lights do not count";
    l.enabled = true;
    scene->setAmbientLight2D(1.0f);
    EXPECT_FLOAT_EQ(sn::lightCheck(scene).ambientIntensity, 1.0f);
    EXPECT_EQ(sn::lightCheck(nullptr).lights, 0);
}

TEST(SpriteNormalsScene, SceneWarnings) {
    const std::string dir = folder("warnings");
    writePng(dir + "/hero.png", balls(32, 32, 32, 12));
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    scene->setAmbientLight2D(0.2f);
    const Entity hero = sprite(sceneId, dir + "/hero.png");
    const sn::Settings settings;
    const std::vector<sn::Delay> still;

    // no light at all; a light without height
    std::vector<sn::Problem> w = sn::sceneWarnings(scene, hero, settings, 32, 32, still, true);
    ASSERT_EQ(kinds(w), std::vector<std::string>{"no_light"});
    EXPECT_EQ(w[0].message, "This scene has no Light 2D.");
    EXPECT_EQ(w[0].fix, "Create a Light 2D (new lights get Height 64).");
    EXPECT_TRUE(w[0].field.empty());
    const Entity light = create(sceneId, "Light", EntityCreationType::POINT_LIGHT_2D);
    scene->getComponent<Light2DComponent>(light).height = 0.0f;
    w = sn::sceneWarnings(scene, hero, settings, 32, 32, still, true);
    ASSERT_EQ(kinds(w), std::vector<std::string>{"no_light_height"});
    EXPECT_EQ(w[0].message, "No Light 2D in this scene has a height above 0, so normal maps don't change the lighting.");
    EXPECT_EQ(w[0].fix, "Set a Light 2D's Height (64 is a good start).");
    scene->getComponent<Light2DComponent>(light).height = 64.0f;
    EXPECT_TRUE(sn::sceneWarnings(scene, hero, settings, 32, 32, still, true).empty());

    // ambient
    scene->setAmbientLight2D(0.8f);
    w = sn::sceneWarnings(scene, hero, settings, 32, 32, still, true);
    ASSERT_EQ(kinds(w), std::vector<std::string>{"ambient_high"});
    EXPECT_EQ(w[0].message, "The scene's 2D ambient intensity is 0.8, which washes out normal maps.");
    scene->setAmbientLight2D(0.79f);
    EXPECT_TRUE(sn::sceneWarnings(scene, hero, settings, 32, 32, still, true).empty());

    // size
    w = sn::sceneWarnings(scene, hero, settings, 16, 32, still, false);
    ASSERT_EQ(kinds(w), std::vector<std::string>{"size_mismatch"});
    EXPECT_EQ(w[0].message, "The sprite's texture is 32x32 but the normal map is 16x32; they must match pixel for pixel.");
    EXPECT_EQ(kinds(sn::sceneWarnings(scene, hero, settings, 16, 32, still, true)), std::vector<std::string>{"size_mismatch"});
    const std::vector<sn::Delay> two = {{80, 1}, {80, 1}};
    EXPECT_TRUE(sn::sceneWarnings(scene, hero, settings, 64, 32, two, true).empty()) << "applying an animation replaces the texture";

    // a Normal Texture sampled differently from the Base Texture
    MeshComponent& mesh = scene->getComponent<MeshComponent>(hero);
    mesh.submeshes[0].material.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
    mesh.submeshes[0].material.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
    EXPECT_TRUE(sn::sceneWarnings(scene, hero, settings, 32, 32, still, false).empty()) << "no Normal Texture yet";
    mesh.submeshes[0].material.normalTexture = Texture(dir + "/hero_normal.png");
    w = sn::sceneWarnings(scene, hero, settings, 32, 32, still, false);
    ASSERT_EQ(kinds(w), std::vector<std::string>{"filter_mismatch"});
    EXPECT_EQ(w[0].message, "The Normal Texture is filtered differently from the Base Texture (pixels will smear at edges).");
    EXPECT_TRUE(sn::sceneWarnings(scene, hero, settings, 32, 32, still, true).empty()) << "apply copies the filter and wrap";
    mesh.submeshes[0].material.normalTexture.setMinFilter(TextureFilter::NEAREST);
    mesh.submeshes[0].material.normalTexture.setMagFilter(TextureFilter::NEAREST);
    EXPECT_TRUE(sn::sceneWarnings(scene, hero, settings, 32, 32, still, false).empty());

    // frames, DirectX, a shader that does not sample the map
    w = sn::sceneWarnings(scene, hero, settings, 32, 32, std::vector<sn::Delay>(ARRAY_INDEX_LIMIT + 1, sn::Delay{100, 1}), false);
    ASSERT_EQ(kinds(w), std::vector<std::string>{"too_many_frames"});
    EXPECT_EQ(w[0].message, "The animation has 65537 frames; a Sprite holds at most 65536.");
    EXPECT_TRUE(sn::sceneWarnings(scene, hero, settings, 32, 32, std::vector<sn::Delay>(129, sn::Delay{100, 1}), false).empty())
        << "129 frames: more than the old 128-frame capacity";
    sn::Settings dx;
    dx.directx = true;
    w = sn::sceneWarnings(scene, hero, dx, 32, 32, still, true);
    ASSERT_EQ(kinds(w), std::vector<std::string>{"directx_in_engine"});
    EXPECT_EQ(w[0].fix, "Turn directx off for this engine's sprites.");
    EXPECT_TRUE(sn::sceneWarnings(scene, NULL_ENTITY, dx, 32, 32, still, true).empty()) << "only with an entity";
    mesh.submeshes[0].material.shader = "library:simple_lit";
    w = sn::sceneWarnings(scene, hero, settings, 32, 32, still, true);
    ASSERT_EQ(kinds(w), std::vector<std::string>{"shader_ignores_normals"});
    EXPECT_EQ(w[0].message, "The sprite's material shader library:simple_lit does not sample a normal map.");
    // library:sprite reads the material's Normal Texture (Catalog::shaderReadsMaterialNormalMap), with or
    // without its features: no warning
    mesh.submeshes[0].material.shader = "library:sprite";
    EXPECT_TRUE(sn::sceneWarnings(scene, hero, settings, 32, 32, still, true).empty());
    mesh.submeshes[0].material.shaderFeatures = {"OUTLINE"};
    EXPECT_TRUE(sn::sceneWarnings(scene, hero, settings, 32, 32, still, true).empty());
    mesh.submeshes[0].material.shaderFeatures.clear();
    mesh.submeshes[0].material.shader.clear();

    // zero delays: the only warning without a scene
    w = sn::sceneWarnings(nullptr, NULL_ENTITY, settings, 32, 32, {{0, 1}, {50, 1}, {5, 1}}, false);
    ASSERT_EQ(kinds(w), std::vector<std::string>{"zero_delay"});
    EXPECT_EQ(w[0].message, "2 frames have a delay under 10 ms; they play at 100 ms, as browsers do.");
    EXPECT_TRUE(sn::sceneWarnings(nullptr, hero, dx, 16, 16, still, false).empty());
    // too many frames needs a Sprite to hold them: without a scene only zero_delay
    EXPECT_EQ(kinds(sn::sceneWarnings(nullptr, NULL_ENTITY, settings, 32, 32, std::vector<sn::Delay>(129, sn::Delay{0, 1}), false)),
              std::vector<std::string>{"zero_delay"});
    EXPECT_TRUE(sn::sceneWarnings(nullptr, NULL_ENTITY, settings, 32, 32, std::vector<sn::Delay>(129, sn::Delay{100, 1}), false).empty());
    // 9.5 ms rounds to 10 (Delay::playbackMs keeps it); 9.4 does not
    EXPECT_TRUE(sn::sceneWarnings(nullptr, NULL_ENTITY, settings, 32, 32, {{19, 2}}, false).empty());
    EXPECT_EQ(kinds(sn::sceneWarnings(nullptr, NULL_ENTITY, settings, 32, 32, {{47, 5}}, false)), std::vector<std::string>{"zero_delay"});

    // a linked material file (the Platform's project holds the links, no editor needed)
    project()->linkMaterialFile(sceneId, hero, 0, dir + "/hero.material");
    w = sn::sceneWarnings(scene, hero, settings, 32, 32, still, true);
    ASSERT_EQ(kinds(w), std::vector<std::string>{"material_linked"});
    EXPECT_EQ(w[0].message, "The sprite's material comes from " + dir + "/hero.material; the scene's copy changed, the file did not.");
    project()->unlinkMaterialFile(sceneId, hero, 0);
}

TEST(SpriteNormalsScene, SubmeshSourcesAndSpriteFrameRules) {
    const std::string dir = folder("submesh");
    writePng(dir + "/a.png", balls(32, 32, 32, 12));
    writePng(dir + "/b.png", balls(16, 16, 16, 6));
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity hero = sprite(sceneId, dir + "/a.png");
    MeshComponent& mesh = scene->getComponent<MeshComponent>(hero);
    mesh.numSubmeshes = 2;
    mesh.submeshes[1].material.baseColorTexture = Texture(dir + "/b.png");
    SpriteComponent& sc = scene->getComponent<SpriteComponent>(hero);
    sc.framesRect[0] = {"a", Rect(0, 0, 16, 32)};
    sc.framesRect[1] = {"b", Rect(16, 0, 16, 32)};
    sc.numFramesRect = 2;

    // submesh 1: its own texture, no Sprite frames
    std::string texture, error;
    std::vector<sn::Cell> cells;
    ASSERT_TRUE(sn::framesFromSprite(scene, hero, 1, texture, cells, error)) << error;
    EXPECT_EQ(texture, dir + "/b.png");
    EXPECT_TRUE(cells.empty());
    EXPECT_FALSE(sn::framesFromSprite(scene, hero, 2, texture, cells, error));
    sn::Spec spec;
    spec.sceneId = sceneId;
    spec.entity = hero;
    spec.submesh = 1;
    sn::Prepared prepared;
    std::vector<sn::OutputFile> files = generateFiles(spec, scene, &prepared);
    EXPECT_EQ(prepared.sourcePath, dir + "/b.png");
    EXPECT_EQ(prepared.request.color.width, 16);
    EXPECT_EQ(prepared.framesFrom, sn::FramesFrom::Single);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].path, dir + "/b_normal.png");
    json recipe = recipeOf(files[0]);
    EXPECT_EQ(recipe["source"]["submesh"], 1);
    EXPECT_EQ(recipe["source"]["kind"], "sprite");
    sn::Spec back;
    std::vector<std::string> notes;
    ASSERT_TRUE(sn::specFromRecipe(recipe, root() / fs::u8path(files[0].path), root(), back, notes, error)) << error;
    EXPECT_EQ(back.submesh, 1u);
    EXPECT_EQ(back.source, dir + "/b.png");
    spec.submesh = 0;
    EXPECT_NE(spec.fingerprint(), [&] { sn::Spec one = spec; one.submesh = 1; return one.fingerprint(); }());
    // submesh 0 (the default): no source.submesh in the recipe
    files = generateFiles(spec, scene, &prepared);
    EXPECT_FALSE(recipeOf(files[0])["source"].contains("submesh"));
    EXPECT_EQ(prepared.framesFrom, sn::FramesFrom::Sprite);

    // warnings for submesh 1: its own texture's size, its own material link
    scene->setAmbientLight2D(0.2f);
    EXPECT_EQ(kinds(sn::sceneWarnings(scene, hero, sn::Settings(), 16, 16, {}, false)), (std::vector<std::string>{"no_light", "size_mismatch"}));
    EXPECT_EQ(kinds(sn::sceneWarnings(scene, hero, sn::Settings(), 16, 16, {}, false, 1)), std::vector<std::string>{"no_light"});
    project()->linkMaterialFile(sceneId, hero, 1, dir + "/b.material");
    EXPECT_EQ(kinds(sn::sceneWarnings(scene, hero, sn::Settings(), 16, 16, {}, false, 1)), (std::vector<std::string>{"no_light", "material_linked"}));
    EXPECT_EQ(kinds(sn::sceneWarnings(scene, hero, sn::Settings(), 32, 32, {}, false, 0)), std::vector<std::string>{"no_light"});
    project()->unlinkMaterialFile(sceneId, hero, 1);

    // outside a project (empty projectDir) a Sprite's texture is its absolute path, whatever the working directory
    {
        const fs::path cwd = fs::current_path();
        fs::current_path(fs::temp_directory_path());
        sn::Spec loose = spec;
        loose.output.normal = (root() / fs::u8path(dir) / "loose_normal.png").string();
        sn::Prepared p0;
        std::vector<sn::Problem> problems;
        const bool ok = sn::prepare(fs::path(), loose, scene, p0, problems);
        fs::current_path(cwd);
        ASSERT_TRUE(ok) << (problems.empty() ? "" : problems[0].message);
        EXPECT_EQ(p0.sourceAbsolute, (root() / fs::u8path(dir) / "a.png").lexically_normal());
        EXPECT_EQ(p0.request.color.width, 32);
        // plannedOutputs names the same files as prepare (the default normal beside the texture, the
        // height map, the frames JSON), from the same source path
        sn::Spec defaults = spec;
        defaults.settings.height = true;
        defaults.output.framesJson = true;
        fs::current_path(fs::temp_directory_path());
        const bool prepared = sn::prepare(fs::path(), defaults, scene, p0, problems);
        fs::current_path(cwd);
        ASSERT_TRUE(prepared) << (problems.empty() ? "" : problems[0].message);
        auto paths = [](const std::vector<sn::PlannedOutput>& outputs) {
            std::vector<std::string> out;
            for (const sn::PlannedOutput& o : outputs) out.push_back(o.role + "=" + o.path);
            return out;
        };
        const std::vector<sn::PlannedOutput> planned = sn::plannedOutputs(fs::path(), defaults, sn::SourceKind::Image);
        EXPECT_EQ(paths(planned), paths(p0.outputs));
        ASSERT_FALSE(planned.empty());
        EXPECT_EQ(planned[0].path, jasperengine::editor::FileUtils::pathToGenericUtf8((root() / fs::u8path(dir) / "a_normal.png").lexically_normal()));
    }

    // overlapping Sprite frames
    sc.framesRect[1] = {"b", Rect(8, 0, 16, 32)};
    std::optional<sn::Problem> p = findKind(sn::checkSpec(root(), spec, scene), "frames_mismatch");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->fix, "Use frames none, or reslice the sprite without overlap");
    spec.frames.mode = sn::FramesSpec::Mode::None;
    EXPECT_TRUE(sn::checkSpec(root(), spec, scene).empty());

    // Cells recorded as the Sprite's: frames.from sprite, a sequence
    sn::Spec given = stillSpec(dir + "/a.png");
    given.frames.mode = sn::FramesSpec::Mode::Cells;
    given.frames.cells = {{0, 0, 16, 32}, {16, 0, 16, 32}};
    given.frames.from = sn::FramesFrom::Sprite;
    files = generateFiles(given, nullptr, &prepared);
    EXPECT_EQ(prepared.framesFrom, sn::FramesFrom::Sprite);
    EXPECT_TRUE(prepared.request.sequence);
    EXPECT_EQ(recipeOf(files[0])["frames"]["from"], "sprite");
    given.frames.from = sn::FramesFrom::Given;
    files = generateFiles(given, nullptr, &prepared);
    EXPECT_EQ(recipeOf(files[0])["frames"]["from"], "given");
    EXPECT_TRUE(prepared.request.sequence) << "Cells with 2 or more cells are a sequence";
    given.frames.cells = {{0, 0, 20, 32}, {16, 0, 16, 32}};
    p = findKind(sn::checkSpec(root(), given), "frames_mismatch");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->fix, "Give frames that do not overlap.");
}

TEST(SpriteNormalsSession, OutsideAProjectPathsAreRelativeToThePng) {
    const fs::path dir = fs::temp_directory_path() / ("jasper-spritenormals-cli-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    std::vector<uint8_t> png;
    ASSERT_TRUE(ts::encodePng(balls(32, 32, 32, 12), png));
    writeBytes(dir / "in" / "hero.png", png);
    sn::Spec spec = stillSpec((dir / "in" / "hero.png").string());
    spec.output.normal = (dir / "out" / "hero_n.png").string();
    spec.output.framesJson = true;
    sn::Prepared prepared;
    std::vector<sn::Problem> problems;
    ASSERT_TRUE(sn::prepare(fs::path(), spec, nullptr, prepared, problems)) << (problems.empty() ? "" : problems[0].message);
    const sn::Result result = sn::generate(prepared.request);
    std::vector<sn::OutputFile> files;
    std::string error, failed;
    ASSERT_TRUE(sn::encodeOutputs(prepared, result, files, error)) << error;
    const json recipe = recipeOf(files[0]);
    EXPECT_EQ(recipe["source"]["path"], "../in/hero.png");
    EXPECT_EQ(recipe["outputs"]["normal"], "hero_n.png");
    EXPECT_EQ(json::parse(role(files, "frames_json")->bytes)["source"], "../in/hero.png");
    ASSERT_TRUE(sn::writeOutputsDirect(files, failed, error)) << error;
    const sn::Staleness s = sn::checkStaleness(fs::path(), dir / "out" / "hero_n.png");
    EXPECT_EQ(s.state, sn::Staleness::State::Fresh) << s.message;
    EXPECT_EQ(s.sourceAbsolute, (dir / "in" / "hero.png").lexically_normal());
    sn::Spec back;
    std::vector<std::string> notes;
    ASSERT_TRUE(sn::specFromRecipe(recipe, dir / "out" / "hero_n.png", fs::path(), back, notes, error)) << error;
    EXPECT_EQ(fs::path(back.source), (dir / "in" / "hero.png").lexically_normal());
    fs::remove_all(dir);
}

// generate_sprite_normals' save_path sheet (mcp::SheetWrite, Phase 3 spec 3.4): a rollback puts the
// old bytes back, or removes the new file and its folders; a sheet whose write fails leaves the file
// it would replace whole (a temporary file renamed over it), and when a rollback can't write the
// previous bytes back the caller is told (a file-size limit makes the writes fail: the root user of
// CI ignores read-only files)
TEST(SpriteNormalsSession, SheetWriteRollsBackAndReportsAFailedRestore) {
    const fs::path dir = fs::temp_directory_path() / ("jasper-spritenormals-sheet-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    const std::vector<uint8_t> old(8192, 'A'), sheet(8192, 'B');
    auto readAll = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    writeBytes(dir / "shots" / "sheet.png", old);
    std::string error, warning;
    {
        // replaced, then rolled back: the old bytes
        mcp::SheetWrite write;
        ASSERT_TRUE(write.write(dir, dir / "shots" / "sheet.png", sheet, error, &warning)) << error;
        EXPECT_EQ(readAll(dir / "shots" / "sheet.png"), sheet);
        EXPECT_TRUE(write.rollback(&warning));
        EXPECT_EQ(readAll(dir / "shots" / "sheet.png"), old);
    }
    {
        // a new file in new folders, rolled back by the destructor: none of them left
        mcp::SheetWrite write;
        ASSERT_TRUE(write.write(dir, dir / "new" / "deep" / "sheet.png", sheet, error)) << error;
        EXPECT_TRUE(fs::exists(dir / "new" / "deep" / "sheet.png"));
    }
    EXPECT_FALSE(fs::exists(dir / "new"));
    {
        // committed: kept
        mcp::SheetWrite write;
        ASSERT_TRUE(write.write(dir, dir / "kept.png", sheet, error)) << error;
        write.commit();
    }
    EXPECT_EQ(readAll(dir / "kept.png"), sheet);
    {
        // a path that can't be opened (a folder) is left alone
        fs::create_directories(dir / "folder.png");
        mcp::SheetWrite write;
        warning.clear();
        EXPECT_FALSE(write.write(dir, dir / "folder.png", sheet, error, &warning));
        EXPECT_FALSE(error.empty());
        EXPECT_TRUE(warning.empty()) << warning;
        EXPECT_TRUE(fs::is_directory(dir / "folder.png"));
    }
#ifndef _WIN32
    {
        // the sheet's write fails half way (past a 4 KiB file-size limit, POSIX's RLIMIT_FSIZE): the 8 KiB file it would
        // replace is whole, no temporary file is left, nothing to warn about
        struct rlimit saved{};
        ASSERT_EQ(getrlimit(RLIMIT_FSIZE, &saved), 0);
        void (*previousHandler)(int) = std::signal(SIGXFSZ, SIG_IGN);
        struct rlimit small = saved;
        small.rlim_cur = 4096;
        ASSERT_EQ(setrlimit(RLIMIT_FSIZE, &small), 0);
        {
            mcp::SheetWrite write;
            error.clear();
            warning.clear();
            const bool written = write.write(dir, dir / "shots" / "sheet.png", sheet, error, &warning);
            EXPECT_FALSE(written);
            EXPECT_FALSE(error.empty());
            EXPECT_TRUE(warning.empty()) << warning;
        }
        EXPECT_EQ(readAll(dir / "shots" / "sheet.png"), old) << "the replaced file is whole";
        EXPECT_EQ(std::distance(fs::directory_iterator(dir / "shots"), fs::directory_iterator()), 1) << "no temporary file left";
        // a small sheet goes through; its rollback can't write the 8 KiB back: reported
        mcp::SheetWrite write;
        const std::vector<uint8_t> tiny(100, 'C');
        const bool written = write.write(dir, dir / "shots" / "sheet.png", tiny, error, &warning);
        warning.clear();
        const bool restored = write.rollback(&warning);
        setrlimit(RLIMIT_FSIZE, &saved);
        std::signal(SIGXFSZ, previousHandler);
        EXPECT_EQ(readAll(dir / "shots" / "sheet.png"), written ? tiny : old);
        EXPECT_TRUE(written);
        EXPECT_FALSE(restored);
        EXPECT_NE(warning.find("its previous version could not be written back"), std::string::npos) << warning;
        EXPECT_NE(warning.find("sheet.png"), std::string::npos) << warning;
        EXPECT_EQ(std::distance(fs::directory_iterator(dir / "shots"), fs::directory_iterator()), 1) << "no temporary file left";
        EXPECT_TRUE(write.rollback()) << "rolled back once only";
    }
#endif
    fs::remove_all(dir);
}
