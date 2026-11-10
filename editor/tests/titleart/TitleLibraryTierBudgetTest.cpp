// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S86 §6.7 (R13, R48) on the library's heaviest 3D items (S84 Deviation 23's 3d_tier_budget
// failures): at every Realtime tier the mesher meets the tier's triangle, solid and mesh-memory
// budgets by coarsening, with its bands (those the tier's solid limit keeps) and bevels, and its
// silhouette; check_title's 3d_tier_budget passes. Red control: MESHER_TEST_NO_TIER_BUDGET gives
// the meshes the tier's reductions alone build, which are over budget. Counted triangles and
// bounds only: load-insensitive.

#include <gtest/gtest.h>

#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtChecks.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/library/TitleLibrary.h"
#include "titleart/library/TitleLibraryApply.h"
#include "titleart/library/TitleLibraryImports.h"
#include "titleart/render/TitleContent.h"
#include "titleart/three_d/Title3DEditor.h"
#include "titleart/three_d/Title3DSource.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleTierFeatures.h"
#include "titleart/three_d/mesher/TitleMesher.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;
using titleart::Json;
using titleart::TitleArtDoc;
using titleart::TitleTier;
namespace fs = std::filesystem;

namespace {

    // the pack's fonts where a library apply names them (TitleArt/Fonts/<id>/<file>)
    class PackFonts : public titleart::AssetResolver {
    public:
        titleart::ResolvedTitleAsset resolve(const titleart::TitleAssetRef& ref, titleart::TitleAssetKind) const override {
            titleart::ResolvedTitleAsset out;
            const std::string prefix = std::string(TITLE_FONTS_ASSET_DIR) + "/";
            if (ref.path.rfind(prefix, 0) != 0) return out;
            const fs::path file = TitleLibrary::packDir() / "fonts" / ref.path.substr(prefix.size());
            std::error_code ec;
            if (!fs::is_regular_file(file, ec)) return out;
            out.found = true;
            out.path = file.string();
            out.assetPath = ref.path;
            return out;
        }
        std::shared_ptr<FontFace> loadFont(const titleart::ResolvedTitleAsset& asset, int faceIndex, std::string* error) const override {
            auto& face = faces[asset.path + "#" + std::to_string(faceIndex)];
            if (!face) face = FontFace::loadFile(asset.path, faceIndex, error, FontFaceRasterizer::Optional);
            return face;
        }
        bool readBytes(const titleart::ResolvedTitleAsset&, std::vector<uint8_t>&, std::string* error) const override {
            if (error) *error = "fonts only";
            return false;
        }
        bool loadImage(const titleart::ResolvedTitleAsset&, titleart::TitleImageData&, std::string*) const override { return false; }
        std::shared_ptr<FontFace> builtinFont() const override { return FontRegistry::getBuiltInLatin(); }

    private:
        mutable std::map<std::string, std::shared_ptr<FontFace>> faces;
    };

    struct Case {
        const char* ref;
        bool isTemplate;
        const char* sample;     // a look's text, a template's language
    };

    // the gate's samples of the items S84 Deviation 23 lists (looks: HERO, 大当り, 1234; templates: en, ja)
    const Case CASES[] = {
        {"scifi/reactor-core-3d", false, "HERO"}, {"scifi/reactor-core-3d", false, "大当り"}, {"scifi/reactor-core-3d", false, "1234"},
        {"neon/tube-3d", false, "HERO"},          {"neon/tube-3d", false, "大当り"},          {"neon/tube-3d", false, "1234"},
        {"gems/treasure-banner", true, "en"},     {"gems/treasure-banner", true, "ja"},
    };

    // the document edit_title's create {look, text} / {template, language} makes
    TitleArtDoc documentOf(const Case& c) {
        titleart::NewTitleArtOptions options;
        if (!c.isTemplate) options.text = c.sample;
        const TitleArtDoc start = titleart::makeNewTitleArt(options);
        const Json op = c.isTemplate ? Json{{"op", "apply_template"}, {"template", c.ref}, {"keep_text", false}, {"language", c.sample}}
                                     : Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", c.ref}, {"apply_font", true}};
        const TitleOpsResult r = TitleArtOps::apply(start, Json::array({op}), TitleOpsOptions{0x86});
        EXPECT_TRUE(r.ok) << c.ref << ": " << r.error();
        return r.doc;
    }

    std::vector<const Json*> layers3D(const TitleArtDoc& doc) {
        std::vector<const Json*> out;
        titleart::forEachLayer(doc.root, [&](const Json& layer, const Json*, int) {
            if (titleart::layerTypeHasStyle(titleart::layerTypeOf(layer)) && layer.value("dimension", std::string("2d")) == "3d") out.push_back(&layer);
            return true;
        });
        return out;
    }

    titleart::three_d::TitleMeshRequest requestOf(const Json& layer, const TitleArtDoc& doc, const titleart::AssetResolver& assets, TitleTier tier) {
        auto content = titleart::makeGeometryTitleContentProvider();
        titleart::three_d::TitleMeshRequest request;
        request.source = titleart::three_d::collectTitleSource(layer, doc, *content, &assets, nullptr);
        request.style = titleart::decodeStyle3D(layer);
        request.blocks = titleart::three_d::collectCharBlocks(layer, request.style.bands.size());
        request.tier = tier;
        return request;
    }

    uint32_t indicesOf(const titleart::TitleMesh& mesh, titleart::TitleSlotRole role, int band = -1) {
        uint32_t n = 0;
        for (const titleart::TitleMeshSlot& s : mesh.slots) {
            if (s.role == role && (band < 0 || s.band == band)) n += s.indexCount;
        }
        return n;
    }

    class TitleLibraryTierBudget : public ::testing::Test {
    protected:
        void SetUp() override {
            registerTitleLibraryOps();
            registerTitle3DEditor();
            TitleLibrary::get().setBuiltinsForTesting({});
            TitleLibrary::get().setProject({});
        }
        PackFonts fonts;
    };

}

TEST_F(TitleLibraryTierBudget, TheLibrarysHeaviestItemsFitEveryTierWithTheirStack) {
    ASSERT_TRUE(fs::is_directory(TitleLibrary::packDir() / "fonts")) << "the staged pack: " << TitleLibrary::packDir().string();
    int overWithoutBudget = 0;
    for (const Case& c : CASES) {
        const TitleArtDoc doc = documentOf(c);
        const std::vector<const Json*> layers = layers3D(doc);
        ASSERT_FALSE(layers.empty()) << c.ref;
        for (const Json* layer : layers) {
            std::array<float, 4> silhouette{};      // High's xy bounds (High drops no band here)
            for (TitleTier tier : {TitleTier::High, TitleTier::Medium, TitleTier::Low}) {
                const std::string what = std::string(c.ref) + " (" + c.sample + ") '" + layer->value("name", std::string()) + "' at " + titleart::titleTierName(tier);
                titleart::three_d::TitleMeshRequest request = requestOf(*layer, doc, fonts, tier);
                titleart::three_d::TitleMesher mesher;
                const auto mesh = mesher.build(request);
                request.testFlags = titleart::three_d::MESHER_TEST_NO_TIER_BUDGET;
                const auto unbudgeted = mesher.build(request);
                const titleart::TitleTierLimits limits = titleart::titleTierLimits(tier);
                std::cout << "[tier budget] " << what << ": " << mesh->stats.triangles << " triangles, " << mesh->bytes() << " bytes (the tier's reductions alone: "
                          << unbudgeted->stats.triangles << ", " << unbudgeted->bytes() << "; limits " << limits.maxTriangles << ", " << limits.meshBytes << ")\n";
                if (!titleart::titleTierFit(unbudgeted->stats, unbudgeted->bytes(), tier).fits) overWithoutBudget++;
                EXPECT_LE(mesh->stats.triangles, limits.maxTriangles) << what;
                EXPECT_LE(mesh->bytes(), limits.meshBytes) << what;
                // the stack the tier builds keeps every band of the unbudgeted mesh, and the bevels
                const titleart::Title3D style = titleart::decodeStyle3D(*layer);
                for (size_t b = 0; b < style.bands.size(); b++) {
                    if (indicesOf(*unbudgeted, titleart::TitleSlotRole::BandFront, (int)b) == 0) continue;
                    EXPECT_GT(indicesOf(*mesh, titleart::TitleSlotRole::BandFront, (int)b), 0u) << what << ": band " << b << " kept";
                    EXPECT_GT(indicesOf(*mesh, titleart::TitleSlotRole::BandSide, (int)b), 0u) << what << ": band " << b << "'s side kept";
                }
                EXPECT_EQ(indicesOf(*mesh, titleart::TitleSlotRole::Bevel) > 0, indicesOf(*unbudgeted, titleart::TitleSlotRole::Bevel) > 0) << what << ": the bevel kept";
                // the silhouette stays: a dropped band hands its grow to the next one out, coarsening
                // keeps the outline (curve segments move it by their sagitta at most)
                const std::array<float, 4> bounds = {mesh->boundsMin[0], mesh->boundsMin[1], mesh->boundsMax[0], mesh->boundsMax[1]};
                if (tier == TitleTier::High) silhouette = bounds;
                for (size_t k = 0; k < 4; k++) EXPECT_NEAR(bounds[k], silhouette[k], 0.01 * request.source.size) << what << ": the silhouette's bound " << k;
            }
        }
        // check_title's verdict on the same document
        std::vector<const CheckDef*> checks;
        std::string error;
        std::vector<std::string> valid;
        ASSERT_TRUE(TitleArtChecks::select({"3d_tier_budget"}, checks, error, valid)) << error;
        TitleCheckContext context(doc, "item.titleart");
        context.assets = &fonts;
        const TitleCheckReport report = TitleArtChecks::run(checks, context);
        for (const TitleCheckResult& r : report.checks) {
            EXPECT_NE(r.status, TitleCheckStatus::Fail) << c.ref << " (" << c.sample << "): " << r.id << ": " << r.message;
        }
    }
    // the red control: these items are over at least one tier without the budget's coarsening
    EXPECT_GE(overWithoutBudget, 3);
}
