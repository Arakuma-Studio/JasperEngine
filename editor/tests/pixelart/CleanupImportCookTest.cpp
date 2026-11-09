// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The texture cook's import step (docs/specs/S46-pixel-art-cleanup.md R33, D8) through the cooker's
// hook: the key changes only with a step, the plan and the cook see the step's output, a step that
// does not apply cooks the raw source and says why, and importStepRecipeText reads what a .meta
// says. The last cases run the real recipe from pixelart/cleanup.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"
#include "support/TextureCorpus.h"

#include "pixelart/PixelImage.h"
#include "pixelart/cleanup/CleanupImportCook.h"
#include "pixelart/cleanup/CleanupRecipe.h"
#include "pixelart/cleanup/CleanupSample.h"
#include "pixelart/cleanup/CleanupSynthetic.h"
#include "texture/TextureCookCache.h"
#include "texture/TextureCooker.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;
namespace cu = jasperengine::editor::pixelart::cleanup;
using jasperengine::editor::pixelart::PixelImage;
namespace fs = std::filesystem;

namespace {

    struct StepBench {
        ScratchDir scratch;
        TextureCookCache cache;
        fs::path source;
        explicit StepBench(const std::string& name, int width = 32, int height = 32) : scratch(name) {
            cache.open(scratch.path / "cache", "", 256ull << 20);
            source = scratch.path / "art.png";
            PixelImage image(width, height);
            std::span<uint8_t> pixels = image.editPixels();
            for (size_t i = 0; i < pixels.size(); i += 4) {
                pixels[i] = static_cast<uint8_t>(i & 0xff);
                pixels[i + 1] = 40;
                pixels[i + 2] = 200;
                pixels[i + 3] = 255;
            }
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
