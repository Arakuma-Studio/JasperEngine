// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The texture cook's import step (docs/specs/S46-pixel-art-cleanup.md R33, D8) through the cooker's
// hook: the key changes only with a step, the plan and the cook see the step's output, a step that
// does not apply cooks the raw source and says why, and importStepRecipeText reads what a .meta
// says. The last cases run the real recipe from pixelart/cleanup: one run serves a source's plan, cook
// and families while they share TextureCookStep::memo and nothing outlives them, and the queue
// recooks for an outside .meta edit of the step alone.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"
#include "support/TextureCorpus.h"

#include "Project.h"
#include "command/type/SetAssetMetaCmd.h"

#include "pixelart/PixelImage.h"
#include "pixelart/cleanup/CleanupImportCook.h"
#include "pixelart/cleanup/CleanupRecipe.h"
#include "pixelart/cleanup/CleanupSample.h"
#include "pixelart/cleanup/CleanupSynthetic.h"
#include "texture/TextureCookCache.h"
#include "texture/TextureCookQueue.h"
#include "texture/TextureCooker.h"

#include "yaml-cpp/yaml.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;
namespace cu = jasperengine::editor::pixelart::cleanup;
using jasperengine::editor::pixelart::PixelImage;
namespace fs = std::filesystem;

namespace {

    PixelImage stripes(int width, int height) {
        PixelImage image(width, height);
        std::span<uint8_t> pixels = image.editPixels();
        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = static_cast<uint8_t>(i & 0xff);
            pixels[i + 1] = 40;
            pixels[i + 2] = 200;
            pixels[i + 3] = 255;
        }
        return image;
    }

    // a clean 16 x 16 sprite upscaled 8 times: the real fix applies
    PixelImage fixableSprite() {
        return cu::upscale(cu::demoSprite(), {8.0, 0.0, 0.0, 0, 0.0, 7});
    }

    struct StepBench {
        ScratchDir scratch;
        TextureCookCache cache;
        fs::path source;
        explicit StepBench(const std::string& name, const PixelImage& image = stripes(32, 32)) : scratch(name) {
            cache.open(scratch.path / "cache", "", 256ull << 20);
            source = scratch.path / "art.png";
            const std::string bytes = image.encodePng();
            std::ofstream file(source, std::ios::binary);
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        TextureCookRequest request() {
            TextureCookRequest r;
            r.source = source;
            r.assetPath = "art.png";
            r.settings.type = TextureImportType::Default;
            r.settings.quality = TextureCookQuality::None;
            r.settings.mipmaps = false;
            r.family = TextureFamily::Desktop;
            return r;
        }
    };

    TextureCookStep stubStep(const std::string& key, TextureCookStep::Outcome outcome, std::atomic<int>* calls = nullptr) {
        TextureCookStep step;
        step.key = key;
        step.run = [outcome, calls](const uint8_t*, int, int, const std::string&) {
            if (calls) (*calls)++;
            return outcome;
        };
        return step;
    }

    TextureCookStep::Outcome fixedOutcome(int width, int height) {
        TextureCookStep::Outcome outcome;
        outcome.applied = true;
        outcome.width = width;
        outcome.height = height;
        outcome.rgba.assign(static_cast<size_t>(width) * height * 4, 0xff);
        outcome.verdict = "fixable";
        return outcome;
    }

}

TEST(CleanupImportCook, ATextureWithoutAStepKeepsItsKey) {
    StepBench bench("cleanup-key-plain");
    TextureCookPlan plain;
    std::string error;
    ASSERT_TRUE(TextureCooker::plan(bench.cache, bench.request(), plain, error)) << error;
    // the key a texture without the key has always had: nothing joins the hash
    EXPECT_EQ(plain.key, TextureCookCache::key(plain.sourceSha256, plain.resolved, TextureFamily::Desktop, plain.choice));
    EXPECT_NE(plain.key, TextureCookCache::key(plain.sourceSha256, plain.resolved, TextureFamily::Desktop, plain.choice, "step"));
    EXPECT_TRUE(plain.analysis.stepState.empty());
    EXPECT_EQ(plain.analysis.width, 32);
    // the raw cook: same bytes and no step recorded
    const TextureCookResult cooked = TextureCooker::cook(bench.cache, bench.request(), nullptr);
    ASSERT_TRUE(cooked.ok) << cooked.error;
    EXPECT_EQ(cooked.record.logicalWidth, 32);
    EXPECT_TRUE(cooked.plan.analysis.stepState.empty());
}

TEST(CleanupImportCook, TheCookKeyChangesWithTheRecipeAndThePipeline) {
    StepBench bench("cleanup-key-step");
    std::string error;
    TextureCookPlan a, b;
    TextureCookRequest withA = bench.request();
    withA.step = cu::importCookStep("{\"recipe\":1}");
    TextureCookRequest withB = bench.request();
    withB.step = cu::importCookStep("{\"recipe\":2}");
    ASSERT_TRUE(TextureCooker::plan(bench.cache, withA, a, error)) << error;
    ASSERT_TRUE(TextureCooker::plan(bench.cache, withB, b, error)) << error;
    EXPECT_NE(a.key, b.key);
    EXPECT_NE(withA.step.key.find("pipeline " + std::to_string(cu::Recipe::kPipelineVersion)), std::string::npos);
}

TEST(CleanupImportCook, TheCookIsOfTheFixedPixelsAtTheirSize) {
    StepBench bench("cleanup-applied");
    std::atomic<int> calls{0};
    TextureCookRequest request = bench.request();
    request.step = stubStep("stub:applied", fixedOutcome(8, 4), &calls);
    TextureCookPlan plan;
    std::string error;
    ASSERT_TRUE(TextureCooker::plan(bench.cache, request, plan, error)) << error;
    EXPECT_EQ(plan.analysis.width, 8);
    EXPECT_EQ(plan.analysis.height, 4);
    EXPECT_EQ(plan.analysis.stepState, "applied");
    EXPECT_EQ(plan.analysis.stepVerdict, "fixable");
    const TextureCookResult cooked = TextureCooker::cook(bench.cache, request, nullptr);
    ASSERT_TRUE(cooked.ok) << cooked.error;
    EXPECT_EQ(cooked.record.logicalWidth, 8);
    EXPECT_EQ(cooked.record.logicalHeight, 4);
    EXPECT_EQ(cooked.record.width, 8);
    // planned once, then read from the sources table (the cook itself runs the step on the decoded file)
    TextureCookPlan again;
    const int before = calls.load();
    ASSERT_TRUE(TextureCooker::plan(bench.cache, request, again, error)) << error;
    EXPECT_EQ(calls.load(), before);
    EXPECT_EQ(again.key, plan.key);
    EXPECT_EQ(again.analysis.stepState, "applied");
}

TEST(CleanupImportCook, AGuardedStepCooksTheRawSourceAndNamesTheGuard) {
    StepBench bench("cleanup-guarded");
    TextureCookStep::Outcome guarded;
    guarded.guard = "lost_share";
    TextureCookRequest request = bench.request();
    request.step = stubStep("stub:guarded", guarded);
    const TextureCookResult cooked = TextureCooker::cook(bench.cache, request, nullptr);
    ASSERT_TRUE(cooked.ok) << cooked.error;
    EXPECT_EQ(cooked.record.logicalWidth, 32);
    EXPECT_EQ(cooked.plan.analysis.stepState, "guarded");
    EXPECT_EQ(cooked.plan.analysis.stepGuard, "lost_share");
    bool warned = false;
    for (const std::string& warning : cooked.record.warnings) warned = warned || warning.find("lost_share") != std::string::npos;
    EXPECT_TRUE(warned);
}

TEST(CleanupImportCook, AStepThatFailsCooksTheRawSourceWithItsError) {
    StepBench bench("cleanup-failed");
    TextureCookStep::Outcome failed;
    failed.error = "bad recipe";
    TextureCookRequest request = bench.request();
    request.step = stubStep("stub:failed", failed);
    const TextureCookResult cooked = TextureCooker::cook(bench.cache, request, nullptr);
    ASSERT_TRUE(cooked.ok) << cooked.error;
    EXPECT_EQ(cooked.record.logicalWidth, 32);
    EXPECT_EQ(cooked.plan.analysis.stepState, "failed");
    EXPECT_EQ(cooked.plan.analysis.stepGuard, "bad recipe");
}

TEST(CleanupImportCook, TheStepMemoFollowsTheSourceAndTheKey) {
    StepBench bench("cleanup-memo");
    std::atomic<int> calls{0};
    TextureCookRequest request = bench.request();
    request.step = stubStep("stub:one", fixedOutcome(4, 4), &calls);
    TextureCookPlan plan;
    std::string error;
    ASSERT_TRUE(TextureCooker::plan(bench.cache, request, plan, error)) << error;
    int before = calls.load();
    EXPECT_GE(before, 1);
    // another step: its own memo entry
    TextureCookRequest other = request;
    other.step.key = "stub:two";
    ASSERT_TRUE(TextureCooker::plan(bench.cache, other, plan, error)) << error;
    EXPECT_EQ(calls.load(), before + 1);
    // the file changes: every step runs again
    {
        std::ofstream file(bench.source, std::ios::binary | std::ios::app);
        file.write("x", 1);
    }
    fs::last_write_time(bench.source, fs::last_write_time(bench.source) + std::chrono::seconds(2));
    before = calls.load();
    ASSERT_TRUE(TextureCooker::plan(bench.cache, request, plan, error)) << error;
    EXPECT_EQ(calls.load(), before + 1);
}

TEST(CleanupImportCook, RecipeTextIsOffForMissingDisabledAndAsepriteSources) {
    ScratchDir scratch("cleanup-meta");
    const fs::path image = scratch.path / "a.png";
    const fs::path meta = scratch.path / "a.png.meta";
    const auto write = [&](const std::string& yaml) {
        std::ofstream file(meta, std::ios::binary | std::ios::trunc);
        file << "metaVersion: 1\nguid: 4b1e9c0d2a7f4e31b6c58d0f9e2a7c13\n" << yaml;
    };
    write("import:\n  textureImporter: {type: default}\n");
    EXPECT_EQ(cu::importStepRecipeText(image), "");
    write("import:\n  pixelCleanup: {enabled: false, recipe: {}}\n");
    EXPECT_EQ(cu::importStepRecipeText(image), "");
    write("import:\n  pixelCleanup: {enabled: true, recipe: {follow: straight}}\n");
    const std::string text = cu::importStepRecipeText(image);
    EXPECT_NE(text.find("\"follow\":\"straight\""), std::string::npos) << text;
    // an .aseprite never takes the step
    const fs::path ase = scratch.path / "a.aseprite";
    {
        std::ofstream file(fs::path(ase.string() + ".meta"), std::ios::binary | std::ios::trunc);
        file << "metaVersion: 1\nguid: 4b1e9c0d2a7f4e31b6c58d0f9e2a7c14\nimport:\n  pixelCleanup: {enabled: true, recipe: {}}\n";
    }
    EXPECT_EQ(cu::importStepRecipeText(ase), "");
    // canonical: an equivalent recipe, with whitespace and order changed, has the same text
    write("import:\n  pixelCleanup: {enabled: true, recipe: '{ \"seed\": 1 , \"follow\" : \"straight\"}'}\n");
    EXPECT_EQ(cu::importStepRecipeText(image), text);
}

TEST(CleanupImportCook, ARealFixableImageIsFixedAtItsSpriteSize) {
    // a clean 16 x 16 sprite upscaled 8 times
    const PixelImage image = cu::upscale(cu::demoSprite(), {8.0, 0.0, 0.0, 0, 0.0, 7});
    const std::string recipe = cu::recipeText(cu::Recipe());
    const TextureCookStep step = cu::importCookStep(recipe);
    TextureCookStep::Outcome outcome = step.run(image.pixels().data(), image.width(), image.height(), "any-hash");
    EXPECT_TRUE(outcome.applied) << outcome.error << outcome.guard;
    EXPECT_EQ(outcome.verdict, "fixable");
    EXPECT_EQ(outcome.width, 16);
    EXPECT_EQ(outcome.height, 16);
}

TEST(CleanupImportCook, NeedsPersonOnAChangedSourceCooksTheRawSourceWithTheGuard) {
    // noise: no grid, not native: needs_person
    PixelImage noise(64, 64);
    std::span<uint8_t> pixels = noise.editPixels();
    cu::Random random(9);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = static_cast<uint8_t>(random() * 256);
        pixels[i + 1] = static_cast<uint8_t>(random() * 256);
        pixels[i + 2] = static_cast<uint8_t>(random() * 256);
        pixels[i + 3] = 255;
    }
    cu::Recipe recorded;
    recorded.source.sha256 = "some-other-file";
    const TextureCookStep step = cu::importCookStep(cu::recipeText(recorded));
    TextureCookStep::Outcome outcome = step.run(noise.pixels().data(), noise.width(), noise.height(), "this-file");
    EXPECT_FALSE(outcome.applied);
    EXPECT_FALSE(outcome.guard.empty());
}

TEST(CleanupImportCook, NeedsPersonOnTheSavedSourceStillCooksTheFix) {
    // the recipe vouches for this very file: a person saw the verdict and saved it (R33)
    const PixelImage image = cu::upscale(cu::demoSprite(), {8.0, 0.0, 0.0, 0, 0.0, 7});
    cu::Recipe recorded;
    recorded.grid.kind = cu::GridKind::Cells;
    recorded.grid.spec = {8, 8, 0, 0};
    recorded.palette = cu::RecipePalette();
    recorded.palette->colors = {{0, 0, 0, 255}};
    recorded.paletteMode = cu::PaletteMode::Fixed;
    recorded.source.sha256 = "this-file";
    const TextureCookStep step = cu::importCookStep(cu::recipeText(recorded));
    TextureCookStep::Outcome outcome = step.run(image.pixels().data(), image.width(), image.height(), "this-file");
    EXPECT_TRUE(outcome.applied) << outcome.error << outcome.guard;
}

// the queue's plan and cook stages are separate requests: the memo they share runs the fix once
TEST(CleanupImportCook, ThePlanAndTheCookStagesShareOneFix) {
    StepBench bench("cleanup-stages", fixableSprite());
    TextureCookRequest request = bench.request();
    request.step = cu::importCookStep(cu::recipeText(cu::Recipe()));
    request.step.memo = std::make_shared<TextureCookStep::Memo>();
    const uint64_t before = cu::importCookFixesRun();
    TextureCookPlan plan;
    std::string error;
    ASSERT_TRUE(TextureCooker::plan(bench.cache, request, plan, error)) << error;
    ASSERT_EQ(plan.analysis.stepState, "applied");
    const TextureCookResult cooked = TextureCooker::cook(bench.cache, request, nullptr);
    ASSERT_TRUE(cooked.ok) << cooked.error;
    EXPECT_EQ(cooked.record.logicalWidth, 16);
    EXPECT_EQ(cu::importCookFixesRun(), before + 1) << "the cook stage ran the fix again";
    // without a memo, one cook() call is both stages, and runs it once too
    StepBench alone("cleanup-stages-alone", fixableSprite());
    TextureCookRequest single = alone.request();
    single.step = cu::importCookStep(cu::recipeText(cu::Recipe()));
    const uint64_t first = cu::importCookFixesRun();
    ASSERT_TRUE(TextureCooker::cook(alone.cache, single, nullptr).ok);
    EXPECT_EQ(cu::importCookFixesRun(), first + 1);
}

// the step keeps nothing itself: an output no cook takes (here the plan of a texture already cooked)
// is not held for whoever asks next
TEST(CleanupImportCook, TheStepHoldsNoOutcomeNobodyTakes) {
    const PixelImage image = fixableSprite();
    StepBench bench("cleanup-holds-nothing", image);
    TextureCookRequest request = bench.request();
    request.step = cu::importCookStep(cu::recipeText(cu::Recipe()));
    ASSERT_TRUE(TextureCooker::cook(bench.cache, request, nullptr).ok);
    // the sources table forgets the file (same bytes, new mtime): the plan runs the fix, and finds the key cooked
    fs::last_write_time(bench.source, fs::last_write_time(bench.source) + std::chrono::seconds(2));
    uint64_t before = cu::importCookFixesRun();
    TextureCookPlan plan;
    std::string error;
    ASSERT_TRUE(TextureCooker::plan(bench.cache, request, plan, error)) << error;
    ASSERT_EQ(cu::importCookFixesRun(), before + 1);
    ASSERT_TRUE(bench.cache.has(plan.key));
    // the same pixels, file and recipe: a run, not that plan's output
    before = cu::importCookFixesRun();
    const TextureCookStep::Outcome again = request.step.run(image.pixels().data(), image.width(), image.height(), plan.sourceSha256);
    EXPECT_TRUE(again.applied);
    EXPECT_EQ(cu::importCookFixesRun(), before + 1) << "an outcome nobody took was still held";
    (void)request.step.run(image.pixels().data(), image.width(), image.height(), plan.sourceSha256);
    EXPECT_EQ(cu::importCookFixesRun(), before + 2);
}

// an export cooks every family of a texture: one fix serves them all, one family at a time or side by side
TEST(CleanupImportCook, EveryFamilyOfATextureSharesOneFix) {
    for (const int threads : {1, 4}) {
        StepBench bench("cleanup-families-" + std::to_string(threads), fixableSprite());
        std::map<std::string, TextureCookInputs> inputs;
        TextureCookInputs in;
        in.assetPath = "art.png";
        in.absolute = bench.source;
        in.settings = bench.request().settings;
        in.hasSettings = true;
        in.pixelCleanupRecipe = cu::recipeText(cu::Recipe());
        inputs[in.assetPath] = in;
        std::vector<TextureBatchItem> items;
        for (TextureFamily family : {TextureFamily::Desktop, TextureFamily::Mobile, TextureFamily::AndroidEtc2, TextureFamily::Web}) {
            TextureBatchItem item;
            item.assetPath = in.assetPath;
            item.family = family;
            items.push_back(item);
        }
        const uint64_t before = cu::importCookFixesRun();
        TextureBatchCook::run(bench.cache, TextureProjectSettings(), inputs, items, threads, 2ull << 30, nullptr, nullptr);
        for (const TextureBatchItem& item : items) {
            ASSERT_TRUE(item.result.ok) << textureFamilyName(item.family) << ": " << item.result.error;
            EXPECT_FALSE(item.result.cacheHit) << textureFamilyName(item.family);
            EXPECT_EQ(item.result.record.logicalWidth, 16) << textureFamilyName(item.family);
        }
        EXPECT_EQ(cu::importCookFixesRun(), before + 1) << threads << " thread(s): a family ran the fix again";
    }
}

// S46 R33 through the queue: an edit of the .meta outside the editor that changes only
// import.pixelCleanup recooks, and the recipe the queue reads comes with the settings' read
TEST(CleanupImportCookQueue, AnOutsideEditOfTheStepAloneRecooks) {
    jasperengine::test::TestTempDir dir{"cleanup-queue-meta"};
    fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/textures", dir.path(), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    auto project = std::make_unique<Project>();
    project->setSkipProjectLock(true);
    ASSERT_TRUE(project->loadProject(dir.path(), false));
    project->getAssetIndex().waitUntilReady();
    TextureCookQueue& queue = TextureCookQueue::get();
    queue.attach(project.get());
    const std::string texture = "textures/gradient.png";
    const fs::path absolute = dir.path() / "assets" / texture;
    SetAssetMetaCmd settings(project.get(), "assets/" + texture, "import.textureImporter", YAML::Load("{version: 1, maxSize: 512}"));
    ASSERT_TRUE(settings.execute());
    queue.request(texture, TextureFamily::Desktop);
    TextureImportSettings stored;
    std::string recipe = "unread";
    ASSERT_TRUE(queue.settingsOf(texture, stored, &recipe));
    EXPECT_EQ(recipe, "");
    queue.cancelAll();
    ASSERT_FALSE(queue.isPending(texture, TextureFamily::Desktop));

    // a text editor adds the step: the importer's settings stay as they were
    const fs::path meta = SetAssetMetaCmd::metaPathOf(absolute);
    YAML::Node root = YAML::LoadFile(meta.string());
    root["import"]["pixelCleanup"] = YAML::Load("{enabled: true, recipe: {follow: straight}}");
    {
        std::ofstream file(meta, std::ios::binary | std::ios::trunc);
        file << YAML::Dump(root) << "\n";
    }
    queue.settingsChanged(texture);
    EXPECT_TRUE(queue.isPending(texture, TextureFamily::Desktop)) << "the step's recipe is a cook input";
    ASSERT_TRUE(queue.settingsOf(texture, stored, &recipe));
    EXPECT_NE(recipe.find("\"follow\":\"straight\""), std::string::npos) << recipe;
    EXPECT_EQ(stored.maxSize.value_or(0), 512);

    // told again with nothing new: no recook
    queue.cancelAll();
    queue.settingsChanged(texture);
    EXPECT_FALSE(queue.isPending(texture, TextureFamily::Desktop));

    // the cook's inputs: the settings and the step from one read of the .meta
    TextureCookInputs inputs;
    ASSERT_TRUE(gatherTextureInputs(*project, texture, inputs));
    EXPECT_TRUE(inputs.hasSettings);
    EXPECT_EQ(inputs.settings.maxSize.value_or(0), 512);
    EXPECT_EQ(inputs.pixelCleanupRecipe, recipe);

    queue.attach(nullptr);
    closeProject(*project);
}
