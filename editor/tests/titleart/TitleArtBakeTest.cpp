// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// S80 R32, R33, R64, AC26, AC45 without a GPU: the shared trimmed frame and its round_to sizes,
// alpha bleed by reach, the recipe chunk, the bake sidecar (unknown members kept, a newer version
// refused), freshness by recipe (fresh, stale with what changed, missing, invalid), the derived
// file names, and the image comparison check_title and diff_images share.

#include <gtest/gtest.h>

#include "texsynth/SynthImage.h"
#include "titleart/TitleArtBake.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtRefs.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/TitleArtSettings.h"
#include "yaml-cpp/yaml.h"
#include "util/ImageDiff.h"
#include "util/FontFace.h"

#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using TJson = titleart::Json;
using titleart::TitleRectI;

namespace {

    fs::path scratch(const std::string& name) {
        const fs::path dir = fs::temp_directory_path() / ("jasper-titleart-bake-test-" + std::to_string(::getpid())) / name;
        fs::remove_all(dir);
        fs::create_directories(dir);
        return dir;
    }

    void writeAll(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    // resolves `path` hints under one folder (no database)
    class FolderAssets : public titleart::AssetResolver {
    public:
        explicit FolderAssets(fs::path root) : root(std::move(root)) {}
        titleart::ResolvedTitleAsset resolve(const titleart::TitleAssetRef& ref, titleart::TitleAssetKind) const override {
            titleart::ResolvedTitleAsset out;
            const fs::path file = root / ref.path;
            std::error_code ec;
            if (ref.path.empty() || !fs::is_regular_file(file, ec)) return out;
            out.found = true;
            out.path = file.string();
            out.assetPath = ref.path;
            out.guid = ref.guid;
            return out;
        }
        std::shared_ptr<FontFace> loadFont(const titleart::ResolvedTitleAsset&, int, std::string*) const override { return nullptr; }
        bool readBytes(const titleart::ResolvedTitleAsset&, std::vector<uint8_t>&, std::string*) const override { return false; }
        bool loadImage(const titleart::ResolvedTitleAsset&, titleart::TitleImageData&, std::string*) const override { return false; }
        std::shared_ptr<FontFace> builtinFont() const override { return nullptr; }
        fs::path root;
    };

    texsynth::Image rgba(int w, int h) {
        return texsynth::Image::make(w, h, 4, 8);
    }

    void put(texsynth::Image& image, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        uint8_t* p = image.pixel(x, y);
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    }

    // a baked PNG of `doc`'s output `id` with the recipe the bake would write for `status`
    void bakeLike(const fs::path& document, const titleart::TitleArtDoc& doc, const TitleBakeStatus& status, const std::string& id,
                  uint32_t bakeVersion) {
        texsynth::Image image = rgba(8, 4);
        std::vector<uint8_t> png, withRecipe;
        ASSERT_TRUE(texsynth::encodePng(image, png));
        TitleBakeRecipe recipe;
        recipe.sourceGuid = doc.guid();
        recipe.sourcePath = document.filename().string();
        recipe.sourceHash = status.sourceHash;
        recipe.assets = status.assets;
        recipe.bakeVersion = bakeVersion;
        recipe.output = id;
        recipe.frame = {0, 0, 8, 4};
        std::string error;
        ASSERT_TRUE(insertTitleBakeRecipe(png, recipe, withRecipe, error)) << error;
        const fs::path out = document.parent_path() / (document.filename().string() + (id == "main" ? ".png" : "." + id + ".png"));
        writeAll(out, std::string(withRecipe.begin(), withRecipe.end()));
    }

}

TEST(TitleArtBakeTest, OutputNames) {
    const fs::path doc = fs::path("/p/assets/logo.titleart");
    EXPECT_EQ(titleArtOutputPath(doc, TJson{{"id", "main"}, {"file", ""}}), fs::path("/p/assets/logo.titleart.png"));
    EXPECT_EQ(titleArtOutputPath(doc, TJson{{"id", "glow"}, {"file", ""}}), fs::path("/p/assets/logo.titleart.glow.png"));
    EXPECT_EQ(titleArtOutputPath(doc, TJson{{"id", "glow"}, {"file", "out/glow.png"}}), fs::path("/p/assets/out/glow.png"));
    EXPECT_EQ(titleArtSidecarPath(doc), fs::path("/p/assets/logo.titleart.bake.json"));
}

TEST(TitleArtBakeTest, SharedFrameRoundsToAndPads) {
    // two outputs: their union plus padding, sides to a multiple of the largest round_to
    std::vector<TitleFrameInput> outputs(2);
    outputs[0].contentBounds = {100, 200, 50, 30};
    outputs[0].paddingCanvasPx = 4;
    outputs[0].roundTo = 4;
    outputs[1].contentBounds = {130, 190, 40, 20};
    outputs[1].paddingCanvasPx = 2;
    outputs[1].roundTo = 8;
    const TitleRectI frame = titleArtBakeFrame(1920, 1080, outputs);
    EXPECT_EQ(frame.x, 96);
    EXPECT_EQ(frame.y, 188);
    EXPECT_EQ(frame.width % 8, 0);
    EXPECT_EQ(frame.height % 8, 0);
    EXPECT_GE(frame.x + frame.width, 174);
    EXPECT_GE(frame.y + frame.height, 234);
    // at the canvas edge the rounding grows left and up instead
    std::vector<TitleFrameInput> edge(1);
    edge[0].contentBounds = {1900, 1070, 20, 10};
    edge[0].paddingCanvasPx = 0;
    edge[0].roundTo = 8;
    const TitleRectI clipped = titleArtBakeFrame(1920, 1080, edge);
    EXPECT_EQ(clipped.x + clipped.width, 1920);
    EXPECT_EQ(clipped.width % 8, 0);
    EXPECT_EQ(clipped.height % 8, 0);
    // an output that does not trim makes the frame the canvas; nothing visible is empty
    edge[0].trim = false;
    EXPECT_TRUE(titleArtBakeFrame(1920, 1080, edge) == (TitleRectI{0, 0, 1920, 1080}));
    std::vector<TitleFrameInput> none(1);
    EXPECT_TRUE(titleArtBakeFrame(1920, 1080, none).empty());
    int w = 0, h = 0;
    titleArtOutputSize({0, 0, 101, 50}, 0.5, 4, w, h);
    EXPECT_EQ(w, 52);
    EXPECT_EQ(h, 28);
}

TEST(TitleArtBakeTest, BleedReach) {
    for (int reach : {8, 64}) {
        texsynth::Image image = rgba(160, 160);
        put(image, 80, 80, 200, 100, 50, 255);
        put(image, 81, 80, 10, 20, 30, 1);
        bleedAlpha(image, reach);
        for (int y = 0; y < 160; y++) {
            for (int x = 0; x < 160; x++) {
                const uint8_t* p = image.pixel(x, y);
                if ((x == 80 || x == 81) && y == 80) continue;
                ASSERT_EQ(p[3], 0) << "alpha stays 0";
                const double d0 = std::hypot(x - 80, y - 80), d1 = std::hypot(x - 81, y - 80);
                const bool within = std::min(d0, d1) <= reach;
                if (!within) {
                    ASSERT_EQ(p[0] + p[1] + p[2], 0) << "beyond the reach is black at " << x << "," << y;
                } else {
                    // the nearest pixel with alpha > 0 gives its colour
                    const bool nearerSecond = d1 < d0;
                    if (std::abs(d0 - d1) < 0.5) continue;
                    ASSERT_EQ(p[0], nearerSecond ? 10 : 200) << "reach " << reach << " at " << x << "," << y;
                }
            }
        }
        // pixels with alpha > 0 are unchanged
        EXPECT_EQ(image.pixel(80, 80)[0], 200);
        EXPECT_EQ(image.pixel(81, 80)[2], 30);
    }
}

TEST(TitleArtBakeTest, RecipeRoundTrip) {
    TitleBakeRecipe recipe;
    recipe.sourceGuid = "0123456789abcdef0123456789abcdef";
    recipe.sourcePath = "titles/logo.titleart";
    recipe.sourceHash = "ab";
    recipe.assets = {{"g", "fonts/a.ttf", "cd"}};
    recipe.bakeVersion = 7;
    recipe.output = "glow";
    recipe.frame = {1, 2, 3, 4};
    recipe.scale = 2.0;
    recipe.bitDepth = 16;
    recipe.extra["title_3d_version"] = 3;
    TitleBakeRecipe back;
    std::string error;
    ASSERT_TRUE(TitleBakeRecipe::fromJson(recipe.toJson(), back, error)) << error;
    EXPECT_EQ(back.sourcePath, recipe.sourcePath);
    EXPECT_EQ(back.assets, recipe.assets);
    EXPECT_EQ(back.bakeVersion, 7u);
    EXPECT_TRUE(back.frame == recipe.frame);
    EXPECT_EQ(back.bitDepth, 16);
    EXPECT_EQ(back.extra.value("title_3d_version", 0), 3) << "a later spec's member survives";
    TJson newer = recipe.toJson();
    newer["version"] = 99;
    EXPECT_FALSE(TitleBakeRecipe::fromJson(newer, back, error));
}

TEST(TitleArtBakeTest, Sidecar) {
    TitleBakeInfo info;
    info.frame = {96, 188, 80, 48};
    info.originX = 960 - 96;
    info.originY = 540 - 188;
    info.outputs = {{"main", "logo.titleart.png", "all", 80, 48}, {"glow", "logo.titleart.glow.png", "glow", 160, 96}};
    info.alphaBounds = {100, 190, 70, 40};
    info.sourceHash = "ff";
    info.bakeVersion = 3;
    info.extra["anchors"] = TJson::array({TJson{{"x", 0.5}, {"y", 0.5}, {"size", 1}, {"strength", 1}, {"fade", 0}}});
    const std::string text = info.toText();
    // the frame in canvas pixels, image space (x, y, w, h), as S81's component reads it
    const TJson json = TJson::parse(text);
    EXPECT_EQ(json["format"], "jasperengine.titleart.bakeinfo");
    EXPECT_EQ(json["frame"]["x"], 96);
    EXPECT_EQ(json["frame"]["h"], 48);
    EXPECT_EQ(json["origin"][0], 864);
    TitleBakeInfo back;
    std::string error;
    ASSERT_TRUE(TitleBakeInfo::parse(text, back, error)) << error;
    EXPECT_TRUE(back.frame == info.frame);
    ASSERT_EQ(back.outputs.size(), 2u);
    EXPECT_EQ(back.outputs[1].width, 160);
    EXPECT_TRUE(back.extra.contains("anchors")) << "unknown members are kept";
    EXPECT_EQ(back.toText(), text) << "canonical";
    TJson newer = json;
    newer["version"] = 2;
    EXPECT_FALSE(TitleBakeInfo::parse(newer.dump(), back, error));
    EXPECT_NE(error.find("newer"), std::string::npos) << error;
}

TEST(TitleArtBakeTest, FreshStaleMissingInvalid) {
    const fs::path dir = scratch("status");
    writeAll(dir / "fonts/a.ttf", "font bytes");
    titleart::TitleArtDoc doc = titleart::makeNewTitleArt();
    doc.root["layers"][0]["font"] = TJson{{"path", "fonts/a.ttf"}, {"guid", "0123456789abcdef0123456789abcdef"}};
    doc.root["outputs"].push_back(TJson{{"id", "glow"}, {"content", "glow"}});
    titleart::normalizeTitleArt(doc, nullptr);
    const fs::path document = dir / "logo.titleart";
    writeAll(document, titleart::writeTitleArt(doc));
    FolderAssets assets(dir);

    TitleBakeStatus status = titleArtBakeStatus(document, doc, assets);
    ASSERT_EQ(status.outputs.size(), 2u);
    EXPECT_EQ(status.outputs[0].status, TitleOutputStatus::Missing);
    EXPECT_FALSE(status.anyBaked());
    ASSERT_EQ(status.assets.size(), 1u);
    EXPECT_FALSE(status.assets[0].sha256.empty()) << "the font is hashed";

    bakeLike(document, doc, status, "main", status.bakeVersion);
    bakeLike(document, doc, status, "glow", status.bakeVersion);
    TitleBakeInfo info;
    writeAll(titleArtSidecarPath(document), info.toText());
    status = titleArtBakeStatus(document, doc, assets);
    EXPECT_EQ(status.outputs[0].status, TitleOutputStatus::Fresh);
    EXPECT_TRUE(status.allFresh());

    // a font change makes both stale, naming the asset
    writeAll(dir / "fonts/a.ttf", "other font bytes!");
    status = titleArtBakeStatus(document, doc, assets);
    EXPECT_EQ(status.outputs[0].status, TitleOutputStatus::Stale);
    ASSERT_FALSE(status.outputs[0].changed.empty());
    EXPECT_NE(status.outputs[0].changed[0].find("fonts/a.ttf"), std::string::npos);
    writeAll(dir / "fonts/a.ttf", "font bytes");
    EXPECT_TRUE(titleArtBakeStatus(document, doc, assets).allFresh()) << "the font as baked: fresh again";

    // a document edit: stale with "document"
    titleart::TitleArtDoc edited = doc;
    edited.root["layers"][0]["text"] = "Edited";
    status = titleArtBakeStatus(document, edited, assets);
    EXPECT_EQ(status.outputs[1].changed.front(), "document");

    // a bake version bump stales only through the recipe's version
    const TitleBakeStatus current = titleArtBakeStatus(document, doc, assets);
    bakeLike(document, doc, current, "main", current.bakeVersion + 1);
    status = titleArtBakeStatus(document, doc, assets);
    EXPECT_EQ(status.outputs[0].status, TitleOutputStatus::Stale);
    EXPECT_EQ(status.outputs[0].changed.front(), "bake_version");
    EXPECT_EQ(status.outputs[1].status, TitleOutputStatus::Fresh) << "only the output baked with another version";

    // a PNG without a recipe, or another document's, is invalid
    std::vector<uint8_t> plain;
    ASSERT_TRUE(texsynth::encodePng(rgba(4, 4), plain));
    writeAll(dir / "logo.titleart.glow.png", std::string(plain.begin(), plain.end()));
    status = titleArtBakeStatus(document, doc, assets);
    EXPECT_EQ(status.outputs[1].status, TitleOutputStatus::Invalid);

    // the derived files a move carries: both PNGs and the sidecar
    const std::vector<fs::path> derived = titleArtDerivedFiles(document, &doc);
    EXPECT_EQ(derived.size(), 3u);
}

TEST(TitleArtBakeTest, RefsVisitEveryAssetKind) {
    titleart::TitleArtDoc doc = titleart::makeNewTitleArt();
    TJson& text = doc.root["layers"][0];
    text["font"] = TJson{{"path", "fonts/a.ttf"}, {"guid", ""}};
    text["fallback_fonts"] = TJson::array({TJson{{"path", "fonts/b.otf"}, {"guid", ""}}});
    titleart::normalizeTitleArt(doc, nullptr);
    doc.root["layers"][0]["style"]["face"]["reflection"]["map"] = TJson{{"path", "env/studio.png"}, {"guid", ""}};
    std::map<std::string, titleart::TitleAssetKind> seen;
    titleart::visitTitleArtRefs(doc.root, [&](const titleart::TitleAssetRef& ref, const std::string& field, titleart::TitleAssetKind kind) {
        if (!ref.empty()) seen[field] = kind;
        return true;
    });
    ASSERT_EQ(seen.count("layers[0].font"), 1u);
    EXPECT_EQ(seen["layers[0].font"], titleart::TitleAssetKind::Font);
    EXPECT_EQ(seen["layers[0].fallback_fonts[0]"], titleart::TitleAssetKind::Font);
    EXPECT_EQ(seen["layers[0].style.face.reflection.map"], titleart::TitleAssetKind::Environment);
}

TEST(ImageDiffTest, PremultipliedIgnoresColourUnderZeroAlpha) {
    texsynth::Image a = rgba(4, 2), b = rgba(4, 2);
    put(a, 0, 0, 200, 0, 0, 255);
    put(b, 0, 0, 200, 0, 0, 255);
    put(b, 3, 1, 0, 255, 0, 0);
    ImageDiffResult pre = diffImages(a, b);
    EXPECT_TRUE(pre.ok);
    EXPECT_EQ(pre.max, 0);
    ImageDiffOptions straight;
    straight.premultiplied = false;
    ImageDiffResult s = diffImages(a, b, straight);
    EXPECT_FALSE(s.ok);
    EXPECT_EQ(s.max, 255);
    EXPECT_TRUE(s.bbox.x == 3 && s.bbox.y == 1 && s.bbox.width == 1 && s.bbox.height == 1);
    EXPECT_DOUBLE_EQ(s.changedPct, 12.5);
    ImageDiffResult size = diffImages(a, rgba(2, 2));
    EXPECT_FALSE(size.sameSize);
    EXPECT_NE(size.error.find("4 x 2"), std::string::npos);
    const texsynth::Image panel = diffPanel(a, b);
    EXPECT_EQ(panel.width, 4 * 3 + 8);
}

TEST(TitleArtBakeTest, AlphaBounds) {
    texsynth::Image image = rgba(10, 10);
    put(image, 2, 3, 1, 1, 1, 1);
    put(image, 7, 8, 1, 1, 1, 255);
    const TitleRectI box = TitleArtRenderHost::alphaBoundsOf(image);
    EXPECT_TRUE(box == (TitleRectI{2, 3, 6, 6}));
    EXPECT_TRUE(TitleArtRenderHost::alphaBoundsOf(rgba(3, 3)).empty());
}

TEST(TitleArtSettingsTest, DefaultsAreNotWrittenAndOldProjectsLoad) {
    const TitleArtProjectSettings defaults;
    EXPECT_TRUE(defaults.autoBake);
    EXPECT_EQ(defaults.staleBakesOnExport, TitleStaleBakesPolicy::Rebake);
    EXPECT_EQ(defaults.encode().size(), 0u) << "a project at the defaults writes no titleArt node";
    EXPECT_TRUE(TitleArtProjectSettings::decode(YAML::Node()) == defaults) << "an older project.yaml has no node";
    TitleArtProjectSettings changed;
    changed.autoBake = false;
    changed.staleBakesOnExport = TitleStaleBakesPolicy::Fail;
    const YAML::Node node = changed.encode();
    EXPECT_TRUE(TitleArtProjectSettings::decode(YAML::Load(YAML::Dump(node))) == changed);
    std::vector<std::string> warnings;
    const TitleArtProjectSettings bad = TitleArtProjectSettings::decode(YAML::Load("{autoBake: maybe, staleBakesOnExport: sometimes}"), &warnings);
    EXPECT_TRUE(bad == defaults) << "values that do not read keep their defaults";
    EXPECT_EQ(warnings.size(), 2u);
}

TEST(TitleArtSettingsTest, Realtime3DQualityPerPlatform) {
    // S86 R37: high on desktop, medium on iOS, low on Android and HTML5; only a changed platform is written
    const TitleArtProjectSettings defaults;
    EXPECT_EQ(defaults.realtime3DQuality.of("windows"), "high");
    EXPECT_EQ(defaults.realtime3DQuality.of("ios"), "medium");
    EXPECT_EQ(defaults.realtime3DQuality.of("android"), "low");
    EXPECT_EQ(defaults.realtime3DQuality.of("web"), "low");
    EXPECT_EQ(defaults.realtime3DQuality.of("playstation"), "");
    TitleArtProjectSettings changed;
    *changed.realtime3DQuality.find("android") = "medium";
    const YAML::Node node = changed.encode();
    ASSERT_TRUE(node["realtime3DQuality"]);
    EXPECT_EQ(node["realtime3DQuality"].size(), 1u);
    EXPECT_EQ(node["realtime3DQuality"]["android"].as<std::string>(), "medium");
    EXPECT_TRUE(TitleArtProjectSettings::decode(YAML::Load(YAML::Dump(node))) == changed);
    std::vector<std::string> warnings;
    const TitleArtProjectSettings bad = TitleArtProjectSettings::decode(YAML::Load("{realtime3DQuality: {web: ultra}}"), &warnings);
    EXPECT_EQ(bad.realtime3DQuality.of("web"), "low");
    EXPECT_EQ(warnings.size(), 1u);
}
