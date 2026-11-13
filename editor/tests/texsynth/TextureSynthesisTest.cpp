// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor side of the texture synthesis (texsynth/SynthesisSession, the recipe check of
// util/ProjectChecks): what the core tests (editor/texsynth/tests) can't reach. File names that
// are not UTF-8 and garbled recipes are reported, never thrown. The window's Save writes without
// the MCP server (ProjectFileWrites).

#include <gtest/gtest.h>

#include "App.h"
#include "AppSettings.h"
#include "Backend.h"
#include "JasperEngine.h"
#include "Platform.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "component/MeshComponent.h"
#include "mcp/McpServer.h"
#include "mcp/tools/W4Shared.h"
#include "texsynth/SynthesisRecipe.h"
#include "texsynth/SynthesisSession.h"
#include "util/ProjectChecks.h"
#include "window/TextureSynthesisWindow.h"

#include "imgui.h"

#include "json.hpp"

#include "support/PosixCompat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace ts = jasperengine::editor::texsynth;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

    fs::path tempProject(const std::string& name) {
        const fs::path dir = fs::temp_directory_path() / ("jasper-synthesis-test-" + name + "-" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir / "assets");
        fs::create_directories(dir / ".jasperengine");
        std::ofstream(dir / "project.yaml") << "name: test\n";
        return dir;
    }

    ts::Image smallImage(int size, uint8_t seed) {
        ts::Image image = ts::Image::make(size, size, 3, 8);
        for (size_t i = 0; i < image.data.size(); i++) image.data[i] = static_cast<uint8_t>((i * 37 + seed * 11) % 251);
        return image;
    }

    void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // A PNG carrying `recipeText` as its jasper.synthesis chunk (valid CRC)
    void writePngWithText(const fs::path& path, const std::string& recipeText) {
        std::vector<uint8_t> png, out;
        ASSERT_TRUE(ts::encodePng(smallImage(8, 1), png));
        std::string error;
        ASSERT_TRUE(ts::insertRecipeChunk(png, recipeText, out, error)) << error;
        writeBytes(path, out);
    }

    std::vector<ProjectChecks::Finding> findingsWithCode(const std::vector<ProjectChecks::Finding>& findings, const std::string& code) {
        std::vector<ProjectChecks::Finding> out;
        for (const ProjectChecks::Finding& f : findings) {
            if (f.code == code) out.push_back(f);
        }
        return out;
    }

}

// ---- file names that are not UTF-8

TEST(TextureSynthesisSession, FingerprintOfNonUtf8PathsNeitherThrowsNorLosesBytes) {
    ts::SynthesisSpec a;
    a.examples.push_back({"assets/caf\xE9.png", "", "", true});
    a.output = "assets/out.png";
    ts::SynthesisSpec b = a;
    b.examples[0].path = "assets/caf\xE8.png";   // another Latin-1 byte: replaced by U+FFFD both would compare equal
    std::string fa, fb;
    EXPECT_NO_THROW(fa = a.fingerprint());
    EXPECT_NO_THROW(fb = b.fingerprint());
    EXPECT_NE(fa, fb);
    EXPECT_EQ(fa, a.fingerprint());
}

TEST(TextureSynthesisSession, CheckSpecRefusesNonUtf8Paths) {
    const fs::path root = tempProject("utf8");
    std::vector<uint8_t> png;
    ASSERT_TRUE(ts::encodePng(smallImage(16, 2), png));
    writeBytes(root / "assets" / "caf\xE9.png", png);
    writeBytes(root / "assets" / "rock.png", png);

    ts::SynthesisSpec spec;
    spec.mode = ts::Mode::Tileable;
    spec.width = spec.height = 32;
    spec.examples.push_back({"assets/caf\xE9.png", "", "", true});
    spec.output = "assets/tile.png";
    bool found = false;
    for (const ts::Problem& p : ts::checkSpec(root, spec)) {
        if (p.field == "examples[0].path" && p.message.find("not valid UTF-8") != std::string::npos &&
            p.message.find("caf\\xE9.png") != std::string::npos) {
            found = true;
        }
        EXPECT_TRUE(ts::isValidUtf8(p.message)) << "messages are printable: " << p.message;
    }
    EXPECT_TRUE(found);

    spec.examples[0].path = "assets/rock.png";
    spec.output = "assets/til\xE9.png";
    found = false;
    for (const ts::Problem& p : ts::checkSpec(root, spec)) found = found || (p.field == "output" && p.message.find("not valid UTF-8") != std::string::npos);
    EXPECT_TRUE(found);

    // a companion found next to the example whose name is not UTF-8: left out with a warning.
    // APFS (macOS) refuses such a name (EILSEQ): there the file cannot exist to be found
    writeBytes(root / "assets" / "rock_normal_\xE9.png", png);
    std::vector<std::string> warnings;
    ts::autoCompanions(root, spec, warnings);
    EXPECT_TRUE(spec.companions.empty());
    if (fs::exists(root / "assets" / "rock_normal_\xE9.png")) {
        ASSERT_EQ(warnings.size(), 1u);
        EXPECT_NE(warnings[0].find("not valid UTF-8"), std::string::npos) << warnings[0];
    } else {
        EXPECT_TRUE(warnings.empty());
    }
    fs::remove_all(root);
}

TEST(TextureSynthesisSession, EncodeOutputsReportsANonUtf8SourceInsteadOfThrowing) {
    const fs::path root = tempProject("encode");
    ts::Request request;
    request.mode = ts::Mode::Extend;
    request.width = request.height = 16;
    request.quality = ts::Quality::Draft;
    request.examples.push_back(ts::Example::fromImage(smallImage(16, 3)));
    const ts::Result result = ts::synthesize(request);
    ASSERT_EQ(result.status, ts::Status::Ok);
    request.examples[0].image.path = root / "assets" / "caf\xE9.png";   // recorded in the recipe
    ts::SynthesisSpec spec;
    spec.examples.push_back({"assets/caf\xE9.png", "", "", true});
    spec.output = "assets/out.png";
    std::vector<ts::OutputFile> files;
    std::string error;
    bool ok = true;
    EXPECT_NO_THROW(ok = ts::encodeOutputs(root, spec, request, result, files, error));
    EXPECT_FALSE(ok);
    EXPECT_NE(error.find("not valid UTF-8"), std::string::npos) << error;
    fs::remove_all(root);
}

// ---- garbled recipes

TEST(TextureSynthesisSession, SpecFromAGarbledRecipeIsAnErrorNotACrash) {
    const fs::path root = "/tmp/project";
    const std::vector<std::string> garbled = {
        R"({"format":1,"mode":"tileable","examples":[{"path":5}]})",
        R"({"format":1,"mode":"tileable","examples":[{"path":"a.png"}],"params":{"nearestNeighbors":"many"}})",
        R"({"format":1,"mode":"tileable","examples":[{"path":"a.png"}],"params":[1,2]})",
        R"({"format":1,"mode":"tileable","examples":[{"path":"a.png"}],"size":{"w":1}})",
        R"({"format":1,"mode":"tileable","examples":[{"path":"a.png"}],"size":[1.5,2]})",
        R"({"format":1,"mode":"tileable","examples":[{"path":"a.png","sampleable":"no"}]})",
        R"({"format":1,"mode":"tileable","examples":[7]})",
        R"({"format":1,"mode":"tileable","examples":[{"path":"a.png"}],"companions":[{"name":2,"sources":[]}]})",
        R"({"format":1,"mode":"tileable","examples":[{"path":"a.png"}],"companions":"normal"})",
        R"({"format":1,"mode":"inpaint","examples":[{"path":"a.png"}],"inpaintMask":{"channel":4}})",
        R"({"format":1,"mode":"guided","examples":[{"path":"a.png"}],"targetGuide":[1]})",
        R"({"format":1,"mode":"tileable","examples":[{"path":"a.png"}],"outputs":{"primary":["o.png"]}})",
        R"({"format":1,"mode":3,"examples":[{"path":"a.png"}]})",
        R"({"format":1,"mode":"tileable","examples":[{"path":"a.png"}],"pathBase":false})",
        R"({"format":1,"mode":"tileable","examples":[{"path":"a.png"}],"seed":{"v":1}})",
    };
    for (const std::string& text : garbled) {
        ts::SynthesisSpec spec;
        std::vector<std::string> notes;
        std::string error;
        bool ok = true;
        EXPECT_NO_THROW(ok = ts::specFromRecipe(json::parse(text), root / "assets" / "t.png", root, spec, notes, error)) << text;
        EXPECT_FALSE(ok) << text;
        EXPECT_NE(error.find("garbled"), std::string::npos) << text << " -> " << error;
    }
    // a good one still restores
    ts::SynthesisSpec spec;
    std::vector<std::string> notes;
    std::string error;
    ASSERT_TRUE(ts::specFromRecipe(json::parse(R"({"format":1,"mode":"tileable","quality":"draft","seed":"18446744073709551615","size":[48,32],
        "pathBase":"project","examples":[{"path":"assets/a.png","sha1":"x"}],"outputs":{"primary":"assets/t.png"}})"),
                                   root / "assets" / "t.png", root, spec, notes, error))
        << error;
    EXPECT_EQ(spec.seed, 18446744073709551615ULL);
    EXPECT_EQ(spec.width, 48);
    EXPECT_EQ(spec.examples[0].path, "assets/a.png");
}

TEST(TextureSynthesisChecks, Latin1NamesAndGarbledRecipesAreReportedNotFatal) {
    const fs::path root = tempProject("check");
    const fs::path assets = root / "assets";
    // a plain PNG with a Latin-1 name (the check used to abort writing its cache key)
    std::vector<uint8_t> png;
    ASSERT_TRUE(ts::encodePng(smallImage(8, 4), png));
    writeBytes(assets / "caf\xE9.png", png);
    // a recipe with a valid CRC but wrong types
    writePngWithText(assets / "garbled.png", R"({"format":1,"mode":"tileable","role":"primary","pathBase":"project","examples":[{"path":5}]})");
    writePngWithText(assets / "role.png", R"({"format":1,"mode":"tileable","role":7,"examples":[{"path":"assets/x.png","sha1":"s"}]})");
    // not JSON at all
    writePngWithText(assets / "text.png", "not json");
    // a CRC mismatch: one byte of the recipe flipped
    {
        std::vector<uint8_t> out;
        std::string error;
        ASSERT_TRUE(ts::insertRecipeChunk(png, R"({"format":1,"mode":"tileable"})", out, error));
        out[60] ^= 0x01;
        writeBytes(assets / "crc.png", out);
    }
    for (int pass = 0; pass < 2; pass++) {   // the second pass runs on the cache
        ProjectChecks::SynthesisCheck check;
        EXPECT_NO_THROW(check = ProjectChecks::checkSynthesisRecipes(root, {})) << "pass " << pass;
        const std::vector<ProjectChecks::Finding> invalid = findingsWithCode(check.findings, "synthesis-recipe-invalid");
        std::vector<std::string> paths;
        for (const ProjectChecks::Finding& f : invalid) paths.push_back(f.path);
        std::sort(paths.begin(), paths.end());
        EXPECT_EQ(paths, (std::vector<std::string>{"assets/crc.png", "assets/garbled.png", "assets/role.png", "assets/text.png"})) << "pass " << pass;
        for (const ProjectChecks::Finding& f : invalid) {
            EXPECT_EQ(f.severity, "warning");
            EXPECT_EQ(f.tool, "texture");
        }
        const auto garbledFinding = std::find_if(invalid.begin(), invalid.end(), [](const ProjectChecks::Finding& f) { return f.path == "assets/garbled.png"; });
        ASSERT_NE(garbledFinding, invalid.end());
        EXPECT_NE(garbledFinding->message.find("examples[0].path is a number"), std::string::npos) << garbledFinding->message;
        // the cache was written, as valid JSON, without the key it could not hold
        std::ifstream cacheFile(root / ".jasperengine" / "synthesis-check-cache.json");
        const std::string text((std::istreambuf_iterator<char>(cacheFile)), std::istreambuf_iterator<char>());
        const json cache = json::parse(text, nullptr, false);
        ASSERT_TRUE(cache.is_object()) << text;
        EXPECT_TRUE(cache["pngs"].contains("assets/garbled.png"));
        for (auto it = cache["pngs"].begin(); it != cache["pngs"].end(); ++it) EXPECT_TRUE(ts::isValidUtf8(it.key()));
    }
    // a cache file of another shape is ignored, not fatal
    std::ofstream(root / ".jasperengine" / "synthesis-check-cache.json", std::ios::trunc) << R"({"version":"1","pngs":[]})";
    EXPECT_NO_THROW(ProjectChecks::checkSynthesisRecipes(root, {}));
    // files mode with the Latin-1 PNG and a garbled one
    EXPECT_NO_THROW(ProjectChecks::checkSynthesisRecipes(root, {assets / "caf\xE9.png", assets / "garbled.png"}));
    fs::remove_all(root);
}

// ---- WriteFilesCmd: all or nothing (the synthesis writes its set through it)

TEST(TextureSynthesisWrite, AFailedMultiFileWriteChangesNothing) {
    const fs::path dir = tempProject("write");
    auto text = [](const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    };
    std::ofstream(dir / "assets" / "tile.png", std::ios::binary) << "old primary";
    std::ofstream(dir / "assets" / "blocker", std::ios::binary) << "a file";
    int changes = 0;
    auto after = [&changes](const std::vector<fs::path>&) { changes++; };

    // the primary replaced, a companion in a new directory, then one under a file: all undone
    {
        mcp::w4::WriteFilesCmd cmd({{dir / "assets" / "tile.png", "new primary"},
                                    {dir / "assets" / "made" / "deeper" / "tile_normal.png", "normal"},
                                    {dir / "assets" / "blocker" / "tile_height.png", "height"}},
                                   after);
        auto failure = std::make_shared<std::string>();
        cmd.reportFailureTo(failure);
        EXPECT_FALSE(cmd.execute());
        EXPECT_EQ(text(dir / "assets" / "tile.png"), "old primary");
        EXPECT_FALSE(fs::exists(dir / "assets" / "made")) << "the directories it created are gone";
        EXPECT_NE(failure->find("assets/blocker/tile_height.png"), std::string::npos) << *failure;
        EXPECT_NE(failure->find("nothing was changed"), std::string::npos) << *failure;
        EXPECT_EQ(changes, 0);
    }
    // a target that is a directory: the files before it are put back too
    fs::create_directories(dir / "assets" / "dir.png");
    {
        mcp::w4::WriteFilesCmd cmd({{dir / "assets" / "tile.png", "new primary"}, {dir / "assets" / "fresh.png", "fresh"},
                                    {dir / "assets" / "dir.png", "x"}},
                                   after);
        auto failure = std::make_shared<std::string>();
        cmd.reportFailureTo(failure);
        EXPECT_FALSE(cmd.execute());
        EXPECT_EQ(text(dir / "assets" / "tile.png"), "old primary");
        EXPECT_FALSE(fs::exists(dir / "assets" / "fresh.png"));
        EXPECT_TRUE(fs::is_directory(dir / "assets" / "dir.png"));
        EXPECT_NE(failure->find("dir.png"), std::string::npos) << *failure;
    }
    // success, undo, redo
    {
        mcp::w4::WriteFilesCmd cmd({{dir / "assets" / "tile.png", "new primary"}, {dir / "assets" / "a" / "b" / "n.png", "normal"}}, after);
        ASSERT_TRUE(cmd.execute());
        EXPECT_EQ(text(dir / "assets" / "tile.png"), "new primary");
        EXPECT_EQ(text(dir / "assets" / "a" / "b" / "n.png"), "normal");
        cmd.undo();
        EXPECT_EQ(text(dir / "assets" / "tile.png"), "old primary");
        EXPECT_FALSE(fs::exists(dir / "assets" / "a")) << "nested directories it created are removed by undo";
        ASSERT_TRUE(cmd.execute());
        EXPECT_EQ(text(dir / "assets" / "a" / "b" / "n.png"), "normal");
        EXPECT_EQ(changes, 3);
    }
    fs::remove_all(dir);
}

// ---- SynthesisRun: callbacks, cancel, shutdown

namespace {

    ts::Request heavyRequest() {
        // seconds of preparation (blur, pyramids of a 3072 x 3072 example) before the first stage
        ts::Request r;
        r.mode = ts::Mode::Guided;
        r.quality = ts::Quality::High;
        r.overrides.backtrackStages = 10;
        r.examples.push_back(ts::Example::fromImage(smallImage(3072, 5)));
        r.targetGuide = ts::ImageInput::fromImage(smallImage(64, 6));
        return r;
    }

    // In a child process (shutdown is for good): runs being prepared are cancelled and waited
    // for, and no callback runs once the editor shuts down
    void shutdownWhileRunning() {
        std::atomic<int> finished{0};
        std::vector<std::shared_ptr<ts::SynthesisRun>> runs;
        for (int i = 0; i < 3; i++) {
            runs.push_back(ts::SynthesisRun::start(heavyRequest(), {}, [&finished](ts::SynthesisRun&) { finished++; }));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto start = std::chrono::steady_clock::now();
        const bool stopped = ts::SynthesisRun::cancelAll(10.0);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        bool cancelled = true;
        for (const auto& run : runs) cancelled = cancelled && run->finished() && run->result().status == ts::Status::Cancelled;
        // _Exit: the answer is the exit code; the engine S02's TestMain runs (and the editor's
        // static App) are not torn down, which LeakSanitizer at exit() would report as leaks
        std::_Exit(stopped && seconds < 2.0 && cancelled && finished == 0 ? 0 : 1);
    }

}

TEST(TextureSynthesisRun, FinishesCallsBackAndCancels) {
    ts::Request r;
    r.mode = ts::Mode::Tileable;
    r.width = r.height = 48;
    r.quality = ts::Quality::Draft;
    r.examples.push_back(ts::Example::fromImage(smallImage(32, 7)));
    std::atomic<int> calls{0};
    std::shared_ptr<ts::SynthesisRun> run = ts::SynthesisRun::start(r, {}, [&calls](ts::SynthesisRun& done) {
        EXPECT_TRUE(done.finished());
        calls++;
    });
    for (int i = 0; i < 500 && calls == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(run->result().status, ts::Status::Ok);

    // cancelRunning (a project switch): the run ends cancelled, still calling back
    std::shared_ptr<ts::SynthesisRun> heavy = ts::SynthesisRun::start(heavyRequest(), {}, [&calls](ts::SynthesisRun&) { calls++; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ts::SynthesisRun::cancelRunning();
    for (int i = 0; i < 300 && !heavy->finished(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(heavy->finished()) << "a cancel reaches the preparation";
    EXPECT_EQ(heavy->result().status, ts::Status::Cancelled);
    for (int i = 0; i < 100 && calls < 2; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(calls, 2);
}

TEST(TextureSynthesisRunDeathTest, ShutdownCancelsWaitsAndCallsNothingBack) {
    EXPECT_EXIT(shutdownWhileRunning(), ::testing::ExitedWithCode(0), "");
}

// ---- the window's Save without the MCP server

namespace {

    // The child's end, on failure too: the answer is the exit code. exit() would destroy the static
    // App while the open project's background work (thumbnails, the asset index) still reads it, and
    // LeakSanitizer would report what is set up for good and change the code
    [[noreturn]] void endChild(int code) {
        std::fflush(stderr);
        std::fflush(stdout);
        std::_Exit(code);
    }

    // In a child process (the editor's App, Platform and project set up for good; no MCP server):
    // the Texture Synthesis window's Save is one project undo step (ProjectFileWrites), its undo
    // and redo work, and a scene texture reading the output is released each time so it loads the
    // new pixels
    void windowSaveWithoutTheServer(const fs::path& dir, const fs::path& config) {
        setenv("XDG_CONFIG_HOME", config.c_str(), 1);
        AppSettings::initialize();
        App& app = Backend::getApp();
        Project* p = app.getProject();
        static editor::Platform* platform = nullptr;
        platform = new editor::Platform(p);
        static char arg0[] = "jasperengine-editor-tests";
        static char* argv[] = {arg0, nullptr};
        Engine::systemInit(1, argv, platform);
        Engine::pauseGameEvents(true);
        Engine::setAsyncLoading(false);
        if (!p->loadProject(dir, false)) endChild(2);
        Engine::setAsyncLoading(false);
        if (mcp::McpServer::get().isStarted()) endChild(3);
        std::vector<uint8_t> png;
        if (!ts::encodePng(smallImage(32, 3), png)) endChild(4);
        writeBytes(dir / "assets" / "rock.png", png);

        // a Sprite drawing the output (as a scene would once it is assigned)
        p->createNewScene("Synth", SceneType::SCENE_2D);
        const uint32_t sceneId = p->getSelectedSceneId();
        auto* create = new CreateEntityCmd(p, sceneId, "Rock", EntityCreationType::SPRITE);
        create->setQuiet(true);
        CommandHandle::get(sceneId)->addCommandNoMerge(create);
        Texture& texture = p->getScene(sceneId)->scene->getComponent<MeshComponent>(create->getEntity()).submeshes[0].material.baseColorTexture;
        texture = Texture("assets/rock_tile.png");

        // the window reads ImGui's clock (a headless context, no frame drawn)
        ImGui::CreateContext();
        TextureSynthesisWindow window(p);
        ts::SynthesisSpec spec;
        spec.mode = ts::Mode::Tileable;
        spec.examples.push_back({"assets/rock.png"});
        spec.width = spec.height = 48;
        spec.quality = ts::Quality::Draft;
        spec.output = "assets/rock_tile.png";
        window.setSpec(spec);
        std::string error;
        if (!window.generate(error)) {
            std::fprintf(stderr, "generate: %s\n", error.c_str());
            endChild(5);
        }
        for (int i = 0; i < 2000 && !(window.currentRun() && window.currentRun()->finished()); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!window.hasResult()) endChild(6);
        CommandHistory* history = p->getProjectCommandHistory();
        const size_t before = history->position();
        std::vector<std::string> written;
        if (!window.save(false, written, error)) {
            std::fprintf(stderr, "save: %s\n", error.c_str());
            endChild(7);
        }
        const fs::path out = dir / "assets" / "rock_tile.png";
        if (written != std::vector<std::string>{"assets/rock_tile.png"} || !fs::is_regular_file(out)) endChild(8);
        if (history->position() != before + 1) endChild(9);
        texture.load();
        if (!texture.hasData()) endChild(10);
        // undo: gone, the texture released; redo: back, released again
        if (!history->undo() || fs::exists(out) || texture.hasData()) endChild(11);
        if (!history->redo() || !fs::is_regular_file(out)) endChild(12);
        texture.load();
        if (!texture.hasData()) endChild(13);
        // a replacing Save: released for the new pixels
        if (!window.save(true, written, error) || texture.hasData()) endChild(14);
        endChild(0);
    }

}

TEST(TextureSynthesisWindowDeathTest, SaveNeedsNoMcpServer) {
    // An exec'd death test runs this body again in the child. It takes the parent's folders, and the
    // parent removes them once the child is gone: the child's jobs write there until it ends
    static const char* const inherited = "JASPER_SYNTHESIS_WINDOW_SAVE_DIR";
    const char* given = std::getenv(inherited);
    const fs::path dir = given ? fs::path(given) : tempProject("window-save");
    const fs::path config = dir.parent_path() / (dir.filename().string() + "-config");
    fs::create_directories(config);
    setenv(inherited, dir.c_str(), 1);
    // (exec'd: the Objective-C runtime refuses a forked child of a process with threads)
    GTEST_FLAG_SET(death_test_style, "threadsafe"); EXPECT_EXIT(windowSaveWithoutTheServer(dir, config), ::testing::ExitedWithCode(0), "");
    unsetenv(inherited);
    fs::remove_all(dir);
    fs::remove_all(config);
}
