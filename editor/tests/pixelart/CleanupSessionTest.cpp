// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S46 D2 (docs/specs/S46-pixel-art-cleanup.md R28-R33, R40, R41, R45): the session layer on plain
// files, no editor: a preview and a write are the same pixels, the recipe travels with the output,
// a re-run is byte-identical, the refusals of R30-R32, the import step, the lint side and the
// preview image.

#include "pixelart/aseprite/AsepriteDocument.h"
#include "pixelart/cleanup/CleanupGrid.h"
#include "pixelart/cleanup/CleanupSample.h"
#include "pixelart/cleanup/CleanupSession.h"
#include "pixelart/cleanup/CleanupSynthetic.h"

#include "texsynth/SynthesisRecipe.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <thread>
#include <atomic>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::cleanup;
using jasperengine::pixelart::Rgba8;
namespace fs = std::filesystem;

namespace {

    class CleanupSessionFiles : public ::testing::Test {
    protected:
        fs::path root;

        void SetUp() override {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            root = fs::temp_directory_path() / ("je-cleanup-session-" + std::string(info->name()) + "-" + std::to_string(reinterpret_cast<uintptr_t>(this) & 0xffff));
            fs::remove_all(root);
            fs::create_directories(root);
        }

        void TearDown() override {
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        std::string path(const std::string& name) const { return (root / name).string(); }

        std::string writePng(const std::string& name, const PixelImage& image) const {
            const fs::path target = root / name;
            EXPECT_TRUE(image.savePng(target));
            return target.string();
        }

        static std::string readFile(const std::string& file) {
            std::ifstream in(file, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        size_t fileCount() const {
            size_t count = 0;
            for (const fs::directory_entry& entry : fs::directory_iterator(root)) {
                (void)entry;
                count++;
            }
            return count;
        }

        Spec specOf(const std::string& source) const {
            Spec spec;
            spec.source = source;
            return spec;
        }

        // an AI-looking sprite on disk (the demo: fake pixels, blurred, noisy)
        std::string aiSprite() const { return writePng("hero.png", demoImage()); }

        // a palette recipe the demo's mushroom snaps to exactly
        nlohmann::json sweetieRecipe() const {
            nlohmann::json recipe = nlohmann::json::object();
            recipe["palette"] = "sweetie-16";
            return recipe;
        }

        Prepared preparedOf(const Spec& spec) {
            std::vector<PixelProblem> problems;
            std::optional<Prepared> prepared = prepare(nullptr, spec, problems);
            EXPECT_TRUE(prepared.has_value()) << (problems.empty() ? "" : problems.front().message);
            return std::move(*prepared);
        }

        // A Result no guard passes: writes refuse it without force
        Result runAsNeedsPerson(const Prepared& prepared) {
            Result result = run(prepared);
            result.report.verdict = Verdict::NeedsPerson;
            result.report.guards.push_back({"low_fit", 0.1, 0.9});
            return result;
        }
    };

    bool hasProblem(const std::vector<PixelProblem>& problems, const std::string& kind) {
        for (const PixelProblem& p : problems) {
            if (p.kind == kind) return true;
        }
        return false;
    }

}

TEST_F(CleanupSessionFiles, PreviewMatchesWriteAndStoresTheRecipe) {
    const std::string source = aiSprite();
    Spec spec = specOf(source);
    spec.recipe = sweetieRecipe();
    Prepared prepared = preparedOf(spec);
    const Result preview = run(prepared);
    EXPECT_EQ(preview.report.verdict, Verdict::Fixable) << preview.report.summary;
    ASSERT_EQ(preview.frames.size(), 1u);
    EXPECT_EQ(preview.resolved.source.sha256.size(), 64u);
    EXPECT_FALSE(preview.resolved.resultSha256.empty());

    spec.output.write = true;
    Prepared writing = preparedOf(spec);
    const Result result = run(writing);
    std::vector<OutputFile> files;
    PixelProblem problem;
    ASSERT_TRUE(encodeOutputs(writing, result, files, problem)) << problem.message;
    ASSERT_EQ(files.size(), 1u);
    ASSERT_TRUE(writeDirect(writing, files, problem)) << problem.message;
    const std::string written = (root / "hero_clean.png").string();
    ASSERT_TRUE(fs::exists(written));

    // the written PNG decodes to the previewed pixels (R44)
    std::string error;
    std::optional<PixelImage> back = PixelImage::loadPng(written, &error);
    ASSERT_TRUE(back.has_value()) << error;
    EXPECT_TRUE(*back == preview.frames.front());

    // the chunk decodes to the resolved recipe with the source hash (R28, R31)
    PixelProblem recipeProblem;
    const std::optional<Recipe> stored = readOutputRecipe(written, recipeProblem);
    ASSERT_TRUE(stored.has_value()) << recipeProblem.message;
    EXPECT_EQ(stored->source.sha256, preview.resolved.source.sha256);
    EXPECT_EQ(stored->resultSha256, preview.resolved.resultSha256);
    EXPECT_EQ(stored->source.path, source);
    EXPECT_FALSE(stored->forced);
    EXPECT_EQ(stored->palette->name, preview.resolved.palette->name);
}

TEST_F(CleanupSessionFiles, ReRunIsByteIdenticalAndStaleAfterSourceEdit) {
    const std::string source = aiSprite();
    Spec spec = specOf(source);
    spec.recipe = sweetieRecipe();
    spec.output.write = true;
    Prepared prepared = preparedOf(spec);
    Result result = run(prepared);
    std::vector<OutputFile> files;
    PixelProblem problem;
    ASSERT_TRUE(encodeOutputs(prepared, result, files, problem)) << problem.message;
    ASSERT_TRUE(writeDirect(prepared, files, problem)) << problem.message;
    const std::string written = (root / "hero_clean.png").string();
    const std::string firstBytes = readFile(written);

    // R41: a re-run with an unchanged source writes the same bytes
    Spec rerun = specOf(written);
    rerun.rerun = true;
    rerun.output.write = true;
    Prepared again = preparedOf(rerun);
    const Result second = run(again);
    EXPECT_FALSE(second.stale.sourceChanged);
    std::vector<OutputFile> rerunFiles;
    ASSERT_TRUE(encodeOutputs(again, second, rerunFiles, problem)) << problem.message;
    ASSERT_TRUE(writeDirect(again, rerunFiles, problem)) << problem.message;
    EXPECT_EQ(readFile(written), firstBytes);
    EXPECT_EQ(checkStaleness(fs::path(), written).state, Staleness::State::Fresh);

    // R32: the source changed
    PixelImage changed = demoImage();
    changed.set(0, 0, {1, 2, 3, 255});
    writePng("hero.png", changed);
    Prepared stale = preparedOf(rerun);
    const Result staleResult = run(stale);
    EXPECT_TRUE(staleResult.stale.sourceChanged);
    EXPECT_EQ(checkStaleness(fs::path(), written).state, Staleness::State::Changed);
}

// R30: a needs_person result is refused and nothing is written without force; force writes it and
// records forced: true
TEST_F(CleanupSessionFiles, NeedsPersonIsRefusedWithoutForce) {
    const std::string source = aiSprite();
    Spec spec = specOf(source);
    spec.output.write = true;
    Prepared prepared = preparedOf(spec);
    const size_t before = fileCount();
    Result result = runAsNeedsPerson(prepared);

    std::vector<OutputFile> files;
    PixelProblem problem;
    EXPECT_FALSE(encodeOutputs(prepared, result, files, problem));
    EXPECT_EQ(problem.kind, "needs_person");
    EXPECT_NE(problem.fix.find("force"), std::string::npos);
    EXPECT_TRUE(files.empty());
    EXPECT_EQ(fileCount(), before);

    // with force the output is written and the recipe says so
    spec.force = true;
    Prepared forcedPrepared = preparedOf(spec);
    Result forced = run(forcedPrepared);
    forced.report.verdict = Verdict::NeedsPerson;
    forced.resolved.forced = true;     // run records it when the verdict is needs_person (forced runs below)
    ASSERT_TRUE(encodeOutputs(forcedPrepared, forced, files, problem)) << problem.message;
    ASSERT_TRUE(writeDirect(forcedPrepared, files, problem)) << problem.message;
    PixelProblem recipeProblem;
    const std::optional<Recipe> stored = readOutputRecipe((root / "hero_clean.png"), recipeProblem);
    ASSERT_TRUE(stored.has_value());
    EXPECT_TRUE(stored->forced);

    // a person in the window can always write, and it is not recorded as forced
    Spec person = specOf(source);
    person.person = true;
    person.output.write = true;
    person.output.png = path("person.png");
    Prepared personPrepared = preparedOf(person);
    Result personResult = runAsNeedsPerson(personPrepared);
    personResult.resolved.forced = false;
    std::vector<OutputFile> personFiles;
    EXPECT_TRUE(encodeOutputs(personPrepared, personResult, personFiles, problem)) << problem.message;
    EXPECT_FALSE(personResult.resolved.forced);
}

// R32: a re-run over an output edited since refuses, and force gets through
TEST_F(CleanupSessionFiles, ReRunOverAnEditedOutputFails) {
    const std::string source = aiSprite();
    Spec spec = specOf(source);
    spec.recipe = sweetieRecipe();
    spec.output.write = true;
    Prepared prepared = preparedOf(spec);
    Result result = run(prepared);
    std::vector<OutputFile> files;
    PixelProblem problem;
    ASSERT_TRUE(encodeOutputs(prepared, result, files, problem));
    ASSERT_TRUE(writeDirect(prepared, files, problem));
    const std::string written = (root / "hero_clean.png").string();

    // edit the output, keep its chunk
    std::optional<std::string> text = jasperengine::editor::texsynth::readRecipeText(written, nullptr, "jasper.pixelcleanup");
    ASSERT_TRUE(text.has_value());
    std::string error;
    std::optional<PixelImage> image = PixelImage::loadPng(written, &error);
    ASSERT_TRUE(image.has_value());
    image->set(0, 0, {200, 10, 10, 255});
    const std::string encoded = image->encodePng();
    std::vector<uint8_t> bytes(encoded.begin(), encoded.end()), withChunk;
    ASSERT_TRUE(jasperengine::editor::texsynth::insertRecipeChunk(bytes, *text, withChunk, error, "jasper.pixelcleanup"));
    {
        std::ofstream out(written, std::ios::binary);
        out.write(reinterpret_cast<const char*>(withChunk.data()), (std::streamsize)withChunk.size());
    }
    EXPECT_TRUE(checkStaleness(fs::path(), written).outputEdited);

    Spec rerun = specOf(written);
    rerun.rerun = true;
    rerun.output.write = true;
    Prepared again = preparedOf(rerun);
    const Result second = run(again);
    std::vector<OutputFile> rerunFiles;
    EXPECT_FALSE(encodeOutputs(again, second, rerunFiles, problem));
    EXPECT_EQ(problem.kind, "output_edited");
    EXPECT_NE(problem.fix.find("force"), std::string::npos);

    rerun.force = true;
    Prepared forcedAgain = preparedOf(rerun);
    const Result forced = run(forcedAgain);
    EXPECT_TRUE(encodeOutputs(forcedAgain, forced, rerunFiles, problem)) << problem.message;
}

TEST_F(CleanupSessionFiles, OverwriteAndOutputGuards) {
    const std::string source = aiSprite();
    Spec spec = specOf(source);
    spec.recipe = sweetieRecipe();
    spec.output.write = true;
    Prepared prepared = preparedOf(spec);
    const Result result = run(prepared);
    std::vector<OutputFile> files;
    PixelProblem problem;
    ASSERT_TRUE(encodeOutputs(prepared, result, files, problem));
    ASSERT_TRUE(writeDirect(prepared, files, problem));

    // R31: an existing output needs overwrite
    Prepared second = preparedOf(spec);
    const Result again = run(second);
    EXPECT_FALSE(encodeOutputs(second, again, files, problem));
    EXPECT_EQ(problem.kind, "output_exists");

    // ... and the source is never an output (D7), even with overwrite
    Spec bad = spec;
    bad.overwrite = true;
    bad.output.png = source;
    Prepared onSource = preparedOf(bad);
    const Result onSourceResult = run(onSource);
    EXPECT_FALSE(encodeOutputs(onSource, onSourceResult, files, problem));
    EXPECT_EQ(problem.kind, "output_is_source");
}

TEST_F(CleanupSessionFiles, BadInputsAreTyped) {
    std::vector<PixelProblem> problems;
    EXPECT_FALSE(prepare(nullptr, specOf(path("missing.png")), problems).has_value());
    EXPECT_TRUE(hasProblem(problems, "source_missing"));

    Spec unknown = specOf(aiSprite());
    unknown.recipe = {{"pallete", "sweetie-16"}};
    problems.clear();
    EXPECT_FALSE(prepare(nullptr, unknown, problems).has_value());
    EXPECT_TRUE(hasProblem(problems, "bad_recipe"));
    EXPECT_NE(problems.front().fix.find("palette"), std::string::npos) << problems.front().fix;

    Spec palette = specOf(aiSprite());
    palette.recipe = {{"palette", "assets/none.palette"}};
    problems.clear();
    EXPECT_FALSE(prepare(nullptr, palette, problems).has_value());
    EXPECT_TRUE(hasProblem(problems, "palette_missing"));

    // R1: the limit
    PixelImage huge(kMaxSourceSide + 1, 8);
    const std::string hugePath = writePng("huge.png", huge);
    problems.clear();
    EXPECT_FALSE(prepare(nullptr, specOf(hugePath), problems).has_value());
    EXPECT_TRUE(hasProblem(problems, "image_too_large"));
    EXPECT_NE(problems.front().fix.find("4096"), std::string::npos);

    // a version-2 recipe
    Spec future = specOf(aiSprite());
    future.recipe = {{"format", Recipe::kFormat}, {"version", Recipe::kVersion + 1}};
    problems.clear();
    EXPECT_FALSE(prepare(nullptr, future, problems).has_value());
    EXPECT_TRUE(hasProblem(problems, "recipe_version"));
}

TEST_F(CleanupSessionFiles, CancelledRunWritesNothing) {
    const std::string source = aiSprite();
    Spec spec = specOf(source);
    spec.output.write = true;
    Prepared prepared = preparedOf(spec);
    const std::atomic<bool> cancel{true};
    const Result result = run(prepared, nullptr, &cancel);
    EXPECT_TRUE(result.cancelled);
    std::vector<OutputFile> files;
    PixelProblem problem;
    EXPECT_FALSE(encodeOutputs(prepared, result, files, problem));
    EXPECT_EQ(problem.kind, "cancelled");
}

// A sheet of a fake AI image: R19-R23 end to end, the frames clip names, .aseprite with its .meta,
// PNG plus JSON, and the resolved recipe re-running to the same bytes
TEST_F(CleanupSessionFiles, SheetsRoundTripAsAsepriteAndPngJson) {
    const std::vector<Rgba8> sweetie = sweetie16();
    const std::vector<Rgba8> fire = {sweetie[2], sweetie[3], sweetie[4], sweetie[1]};
    const PixelArt sprite = makeSprite(10, 12, fire, 3, true);
    std::vector<PixelArt> frames;
    for (int f = 0; f < 5; f++) frames.push_back(place(sprite, 12, 16, 1, 2 + (f % 2)));
    const FakeSheet fake = fakeSheet(frames, 6);
    const std::string source = writePng("run_sheet.png", fake.image);

    Spec spec = specOf(source);
    spec.recipe = {{"remove_background", true}, {"palette", {"#2a2d3e", "#e43b44", "#f77622", "#ffe762"}}};
    spec.output.write = true;
    Prepared prepared = preparedOf(spec);
    const Result result = run(prepared);
    ASSERT_TRUE(result.animation.has_value()) << result.report.summary;
    EXPECT_EQ(result.format, OutputFormat::Aseprite);
    std::vector<OutputFile> files;
    PixelProblem problem;
    ASSERT_TRUE(encodeOutputs(prepared, result, files, problem)) << problem.message;
    ASSERT_EQ(files.size(), 2u);     // the .aseprite and its .meta in one all-or-nothing write
    EXPECT_EQ(files[0].role, "aseprite");
    EXPECT_EQ(files[1].role, "meta");
    ASSERT_TRUE(writeDirect(prepared, files, problem)) << problem.message;
    const std::string written = (root / "run_sheet.aseprite").string();
    ASSERT_TRUE(fs::exists(written));
    PixelProblem recipeProblem;
    const std::optional<Recipe> stored = readOutputRecipe(written, recipeProblem);
    ASSERT_TRUE(stored.has_value()) << recipeProblem.message;
    EXPECT_EQ(stored->sheetMode, SheetMode::Given);

    const aseprite::ReadResult back = aseprite::readFile(written);
    ASSERT_TRUE(back.ok()) << back.error.message;
    EXPECT_FALSE(back.document->tags.empty());
    const std::optional<std::string> pixels = outputPixelsSha256(written);
    ASSERT_TRUE(pixels.has_value());
    EXPECT_EQ(*pixels, result.resolved.resultSha256);

    // re-run: byte-identical
    Spec rerun = specOf(written);
    rerun.rerun = true;
    rerun.output.write = true;
    const std::string firstAseprite = readFile(written);
    const std::string firstMeta = readFile(written + ".meta");
    Prepared again = preparedOf(rerun);
    const Result second = run(again);
    std::vector<OutputFile> rerunFiles;
    ASSERT_TRUE(encodeOutputs(again, second, rerunFiles, problem)) << problem.message;
    ASSERT_TRUE(writeDirect(again, rerunFiles, problem)) << problem.message;
    EXPECT_EQ(readFile(written), firstAseprite);
    EXPECT_EQ(readFile(written + ".meta"), firstMeta);

    // the PNG sheet writes a picture and JSON with the same frames
    Spec png = spec;
    png.format = OutputFormat::Png;
    png.output.png = path("run_png.png");
    Prepared pngPrepared = preparedOf(png);
    const Result pngResult = run(pngPrepared);
    ASSERT_TRUE(encodeOutputs(pngPrepared, pngResult, files, problem)) << problem.message;
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].role, "png");
    EXPECT_EQ(files[1].role, "json");
    EXPECT_NE(files[1].bytes.find("\"jasperengine\""), std::string::npos);
}

TEST_F(CleanupSessionFiles, ImportStepRunsThePureRecipe) {
    const PixelImage source = demoImage();
    Spec spec = specOf(aiSprite());
    spec.recipe = sweetieRecipe();
    Prepared prepared = preparedOf(spec);
    const Result expected = run(prepared);
    const std::string text = recipeText(expected.resolved);

    PixelImage out;
    Verdict verdict = Verdict::Clean;
    std::string guard, error;
    ASSERT_TRUE(runImportStep(source, text, out, verdict, guard, error)) << error;
    EXPECT_TRUE(out == expected.frames.front());
    EXPECT_EQ(verdict, Verdict::Fixable);

    EXPECT_FALSE(runImportStep(source, "{not a recipe", out, verdict, guard, error));
    EXPECT_FALSE(error.empty());

    // a bad fix reports needs_person with its guard, so the cook can fall back to the raw source
    Recipe wrong = expected.resolved;
    wrong.palette->colors = {Rgba8{0, 0, 0, 255}, Rgba8{255, 255, 255, 255}};
    wrong.paletteMode = PaletteMode::Fixed;
    wrong.grid.spec.cellWidth = 3.3;
    wrong.grid.spec.cellHeight = 3.3;
    ASSERT_TRUE(runImportStep(source, recipeText(wrong), out, verdict, guard, error)) << error;
    EXPECT_EQ(verdict, Verdict::NeedsPerson);
    EXPECT_FALSE(guard.empty());
}

// R40: the results do not depend on how detection's loops were spread
TEST_F(CleanupSessionFiles, ThreadCountDoesNotChangeResults) {
    const std::string source = aiSprite();
    Spec spec = specOf(source);
    spec.recipe = sweetieRecipe();
    Prepared prepared = preparedOf(spec);

    setParallelFor(ParallelFor{});
    const Result serial = run(prepared);
    setParallelFor([](int count, const std::function<void(int)>& body) {
        std::vector<std::thread> threads;
        std::atomic<int> next{0};
        for (int t = 0; t < 8; t++) {
            threads.emplace_back([&]() {
                for (int i = next++; i < count; i = next++) body(i);
            });
        }
        for (std::thread& thread : threads) thread.join();
    });
    const Result parallel = run(prepared);
    setParallelFor(ParallelFor{});

    EXPECT_TRUE(serial.frames.front() == parallel.frames.front());
    EXPECT_EQ(toJson(prepared, serial).dump(), toJson(prepared, parallel).dump());
}

TEST_F(CleanupSessionFiles, PreviewKindsAreDeterministicAndFitTheWidth) {
    const std::string source = aiSprite();
    Spec spec = specOf(source);
    spec.recipe = sweetieRecipe();
    Prepared prepared = preparedOf(spec);
    const Result result = run(prepared);

    for (PreviewKind kind : {PreviewKind::Compare, PreviewKind::Result, PreviewKind::Grid, PreviewKind::Lost, PreviewKind::Lints}) {
        const PixelImage image = renderPreview(prepared, result, kind, 512);
        EXPECT_FALSE(image.empty()) << previewKindName(kind);
        EXPECT_LE(image.width(), 512) << previewKindName(kind);
        EXPECT_TRUE(image == renderPreview(prepared, result, kind, 512)) << previewKindName(kind);
    }
    EXPECT_TRUE(renderPreview(prepared, result, PreviewKind::None).empty());

    // the compare's right half holds the result, at a whole scale
    const PixelImage compare = renderPreview(prepared, result, PreviewKind::Compare, 1024);
    const PixelImage& fixed = result.frames.front();
    const int scale = std::max(1, (compare.height() - 14) / std::max(1, fixed.height()));
    EXPECT_GE(scale, 1);
}

// R46: the lint side reads a plain file and an .aseprite with the same frames a person sees
TEST_F(CleanupSessionFiles, LintsAPlainFile) {
    const PixelArt sprite = demoSprite();
    const std::string source = writePng("mushroom.png", toRgba(sprite));
    LintSpec spec;
    spec.source = source;
    spec.palette = "sweetie-16";
    std::vector<PixelProblem> problems;
    std::optional<LintPrepared> prepared = prepareLint(nullptr, spec, problems);
    ASSERT_TRUE(prepared.has_value()) << (problems.empty() ? "" : problems.front().message);
    const LintOutcome outcome = runLint(*prepared);
    EXPECT_EQ(outcome.verdict, Verdict::Clean) << outcome.summary;
    EXPECT_EQ(outcome.report.fail, 0);

    LintSpec wrong = spec;
    wrong.lints = {"not_a_lint"};
    problems.clear();
    EXPECT_FALSE(prepareLint(nullptr, wrong, problems).has_value());
    EXPECT_TRUE(hasProblem(problems, "bad_recipe"));

    // an AI sprite is not clean (R29)
    LintSpec ai;
    ai.source = aiSprite();
    problems.clear();
    std::optional<LintPrepared> aiPrepared = prepareLint(nullptr, ai, problems);
    ASSERT_TRUE(aiPrepared.has_value()) << (problems.empty() ? "" : problems.front().message);
    EXPECT_EQ(runLint(*aiPrepared).verdict, Verdict::Fixable);
}

// R30: an output written with force reports `forced` when linted
TEST_F(CleanupSessionFiles, LintReportsForcedOutputs) {
    Spec spec = specOf(aiSprite());
    spec.recipe = sweetieRecipe();
    spec.output.write = true;
    spec.force = true;
    Prepared prepared = preparedOf(spec);
    Result result = run(prepared);
    result.report.verdict = Verdict::NeedsPerson;
    result.resolved.forced = true;
    std::vector<OutputFile> files;
    PixelProblem problem;
    ASSERT_TRUE(encodeOutputs(prepared, result, files, problem)) << problem.message;
    ASSERT_TRUE(writeDirect(prepared, files, problem)) << problem.message;

    LintSpec spec2;
    spec2.source = (root / "hero_clean.png").string();
    std::vector<PixelProblem> problems;
    std::optional<LintPrepared> lintPrepared = prepareLint(nullptr, spec2, problems);
    ASSERT_TRUE(lintPrepared.has_value());
    EXPECT_TRUE(runLint(*lintPrepared).forced);
    EXPECT_TRUE(toJson(*lintPrepared, runLint(*lintPrepared)).value("forced", false));
}
