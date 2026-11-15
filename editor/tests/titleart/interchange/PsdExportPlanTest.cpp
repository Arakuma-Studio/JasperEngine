// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The layered PSD export's plan (docs/specs/S87-title-localisation-interchange.md R38, R39, the
// plan ported from Pachislop src/io/psd.ts), without a GPU: groups as PSD groups, a group with
// effects holding its members hidden under its "fx", clipped layers flagged and masked by their
// base's alpha before its effects, native adjustment layers and "(baked)" ones, layer masks on the
// records, split titles (rims, face, fx); then assembly over synthetic renders, written and read
// back.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/interchange/psd/PsdExport.h"
#include "titleart/interchange/psd/PsdReader.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "support/PosixCompat.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::psd;
using titleart::Json;
namespace fs = std::filesystem;

namespace {

    titleart::TitleArtDoc document() {
        titleart::TitleArtDoc doc = titleart::makeNewTitleArt();
        doc.root["canvas"] = Json{{"width", 64}, {"height", 32}};
        doc.root["exposed"] = Json::array();
        const Json shape = Json{{"type", "shape"}, {"name", "Plate"}, {"size", 20}};
        Json title = Json{{"type", "text"}, {"name", "Title"}, {"text", "AB"}, {"size", 20},
                          {"style", {{"rims", Json::array({Json{{"width", 3}}, Json{{"width", 2}}})}}}};
        Json clippedShape = Json{{"type", "shape"}, {"name", "Shine"}, {"clip", true}, {"blend", "screen"}};
        Json clippedLevels = Json{{"type", "adjustment"}, {"name", "Clip Levels"}, {"clip", true}, {"adjustment", {{"type", "levels"}}}};
        Json clippedExposure = Json{{"type", "adjustment"}, {"name", "Clip Exposure"}, {"clip", true}, {"adjustment", {{"type", "exposure"}}}};
        Json group = Json{{"type", "group"}, {"name", "FX Group"},
                          {"effects", Json::array({Json{{"type", "drop_shadow"}}})},
                          {"children", Json::array({Json{{"type", "shape"}, {"name", "In A"}}, Json{{"type", "shape"}, {"name", "In B"}}})}};
        Json masked = Json{{"type", "shape"}, {"name", "Masked"}, {"masks", Json::array({Json{{"type", "shape"}}})}};
        Json hidden = Json{{"type", "shape"}, {"name", "Hidden"}, {"visible", false}, {"blend", "luminosity"}};
        Json hue = Json{{"type", "adjustment"}, {"name", "Hue"}, {"adjustment", {{"type", "hue_saturation"}, {"hue", 20}}}};
        Json vibrance = Json{{"type", "adjustment"}, {"name", "Vibrance"}, {"adjustment", {{"type", "vibrance"}, {"vibrance", 20}}}};
        doc.root["layers"] = Json::array({shape, title, clippedShape, clippedLevels, clippedExposure, group, masked, hidden, hue, vibrance});
        titleart::normalizeTitleArt(doc, nullptr);
        return doc;
    }

    const PsdPlanNode* named(const std::vector<PsdPlanNode>& nodes, const std::string& name) {
        for (const PsdPlanNode& n : nodes) {
            if (n.name == name) return &n;
            if (const PsdPlanNode* inner = named(n.children, name)) return inner;
        }
        return nullptr;
    }

    bool warned(const PsdExportPlan& plan, const std::string& text) {
        for (const std::string& w : plan.warnings) if (w.find(text) != std::string::npos) return true;
        return false;
    }

}

TEST(PsdExportPlan, LayersGroupsClipsAdjustments) {
    const titleart::TitleArtDoc doc = document();
    const PsdExportPlan plan = planPsdExport(doc, {});
    EXPECT_EQ(plan.jobs[(size_t)plan.mergedJob].purpose, "merged");
    // top level, bottom first
    std::vector<std::string> names;
    for (const PsdPlanNode& n : plan.nodes) names.push_back(n.name);
    EXPECT_EQ(names, (std::vector<std::string>{"Plate", "Title", "Shine", "Clip Levels", "Clip Exposure (baked)", "FX Group", "Masked", "Hidden", "Hue",
                                               "Vibrance (baked)"}));
    // clipped layers: flagged and masked by the base's alpha before its effects
    const PsdPlanNode* shine = named(plan.nodes, "Shine");
    ASSERT_NE(shine, nullptr);
    EXPECT_TRUE(shine->clipping);
    EXPECT_EQ(shine->blendKey, "scrn");
    ASSERT_EQ(shine->clipBases.size(), 1u);
    const PsdRenderJob& base = plan.jobs[(size_t)shine->clipBases[0]];
    EXPECT_EQ(base.purpose, "clip_base");
    EXPECT_TRUE(titleart::findLayer(base.doc->root, doc.layers()[1]["id"].get<std::string>())->value("effects", Json::array()).empty());
    // a clipped native adjustment, a clipped baked one (opaque inside the base, clipped)
    const PsdPlanNode* levels = named(plan.nodes, "Clip Levels");
    EXPECT_EQ(levels->kind, PsdPlanNode::Kind::Adjust);
    EXPECT_EQ(levels->adjustmentKey, "levl");
    EXPECT_TRUE(levels->clipping);
    const PsdPlanNode* exposure = named(plan.nodes, "Clip Exposure (baked)");
    EXPECT_EQ(exposure->kind, PsdPlanNode::Kind::Pixels);
    EXPECT_TRUE(exposure->opaque);
    EXPECT_TRUE(exposure->clipping);
    // a group with effects: its members hidden, its baked render "fx" on top
    const PsdPlanNode* group = named(plan.nodes, "FX Group");
    ASSERT_EQ(group->kind, PsdPlanNode::Kind::Group);
    ASSERT_EQ(group->children.size(), 3u);
    EXPECT_TRUE(group->children[0].hidden);
    EXPECT_TRUE(group->children[1].hidden);
    EXPECT_EQ(group->children[2].name, "fx");
    EXPECT_FALSE(group->children[2].hidden);
    // masks on the record, the pixels drawn whole
    const PsdPlanNode* mask = named(plan.nodes, "Masked");
    ASSERT_GE(mask->maskJob, 0);
    EXPECT_EQ(plan.jobs[(size_t)mask->maskJob].purpose, "mask");
    const Json* drawn = titleart::findLayer(plan.jobs[(size_t)mask->job].doc->root, doc.layers()[6]["id"].get<std::string>());
    EXPECT_TRUE((*drawn)["masks"].empty());
    // a hidden layer stays a hidden record, a blend Photoshop has natively
    EXPECT_TRUE(named(plan.nodes, "Hidden")->hidden);
    EXPECT_EQ(named(plan.nodes, "Hidden")->blendKey, "lum ");
    // a top-level baked adjustment hides what it covers
    EXPECT_EQ(named(plan.nodes, "Hue")->adjustmentKey, "hue2");
    EXPECT_TRUE(named(plan.nodes, "Plate")->hidden) << "Vibrance (baked) stands in for every layer below it";
    EXPECT_TRUE(warned(plan, "adjustment_baked: the adjustment \"Vibrance\""));
    EXPECT_TRUE(warned(plan, "adjustment_baked: the adjustment \"Clip Exposure\""));
}

TEST(PsdExportPlan, SplitTitles) {
    PsdPlanOptions options;
    options.splitTitles = true;
    const PsdExportPlan plan = planPsdExport(document(), options);
    const PsdPlanNode* title = named(plan.nodes, "Title");
    ASSERT_NE(title, nullptr);
    ASSERT_EQ(title->kind, PsdPlanNode::Kind::Group);
    EXPECT_EQ(title->blendKey, "pass");
    std::vector<std::string> parts;
    for (const PsdPlanNode& n : title->children) parts.push_back(n.name);
    // rims outermost first, the face, fx, then the layers clipped to the title, clipped to fx
    EXPECT_EQ(parts, (std::vector<std::string>{"rim 2", "rim 1", "face", "fx", "Shine", "Clip Levels", "Clip Exposure (baked)"}));
    EXPECT_TRUE(title->children[0].hidden);
    EXPECT_FALSE(title->children[3].hidden);
    // rim 2 alone: its cumulative width, the face hidden, no effects
    const titleart::TitleArtDoc& rim2 = *plan.jobs[(size_t)title->children[0].job].doc;
    const Json* layer = titleart::findLayer(rim2.root, plan.jobs[(size_t)title->children[0].job].isolateLayer);
    ASSERT_NE(layer, nullptr);
    ASSERT_EQ((*layer)["style"]["rims"].size(), 1u);
    EXPECT_DOUBLE_EQ((*layer)["style"]["rims"][0]["width"].get<double>(), 5.0);
    EXPECT_DOUBLE_EQ((*layer)["style"]["face"]["opacity"].get<double>(), 0.0);
}

TEST(PsdExportPlan, AssembleWriteAndRead) {
    const PsdExportPlan plan = planPsdExport(document(), {});
    constexpr int W = 16, H = 8;
    std::vector<texsynth::Image> renders;
    for (size_t i = 0; i < plan.jobs.size(); i++) {
        texsynth::Image img = texsynth::Image::make(W, H, 4, 8);
        for (size_t p = 0; p < (size_t)W * H; p++) {
            img.data[p * 4 + 0] = (uint8_t)(10 + i);
            img.data[p * 4 + 3] = plan.jobs[i].purpose == "clip_base" ? (p % W < 8 ? 255 : 0) : 255;
        }
        renders.push_back(std::move(img));
    }
    PsdWriteDocument psd;
    std::string error;
    ASSERT_TRUE(assemblePsd(plan, renders, 8, 144, psd, error)) << error;
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(writePsd(psd, bytes, error)) << error;
    PsdFile file;
    ASSERT_TRUE(readPsd(bytes.data(), bytes.size(), file, error)) << error;
    EXPECT_EQ(file.width, W);
    EXPECT_DOUBLE_EQ(file.dpi, 144.0);
    const PsdLayerRecord* shine = nullptr;
    for (const PsdLayerRecord& l : file.layers) if (l.name == "Shine") shine = &l;
    ASSERT_NE(shine, nullptr);
    EXPECT_TRUE(shine->clipping);
    EXPECT_EQ(shine->bounds.width(), 8) << "masked by the base's alpha: the right half is gone";
    // a renders/jobs mismatch is refused
    renders.pop_back();
    EXPECT_FALSE(assemblePsd(plan, renders, 8, 72, psd, error));
}

// R49: the streamed PSD equals the one assembled in memory, and holds at most 3 x the frame's
// RGBA16F size however many layers it has (red control: holding the renders, as a batch does,
// is far over that for this plan)
TEST(PsdExportPlan, StreamingMatchesTheBatchUnderTheMemoryCap) {
    const PsdExportPlan plan = planPsdExport(document(), {});
    constexpr int W = 64, H = 32;
    const size_t cap = 3 * (size_t)W * H * 8;
    for (int bits : {8, 16}) {
        const int bps = bits / 8;
        std::vector<texsynth::Image> renders;
        for (size_t i = 0; i < plan.jobs.size(); i++) {
            texsynth::Image img = texsynth::Image::make(W, H, 4, bits);
            for (size_t p = 0; p < (size_t)W * H; p++) {
                const uint16_t alpha = plan.jobs[i].purpose == "clip_base" ? (p % W < 16 ? 0xffff : 0) : 0xffff;
                const uint16_t red = (uint16_t)(257 * (10 + i));
                if (bps == 1) {
                    img.data[p * 4 + 0] = (uint8_t)red;
                    img.data[p * 4 + 3] = (uint8_t)alpha;
                } else {
                    std::memcpy(img.data.data() + (p * 4 + 0) * 2, &red, 2);
                    std::memcpy(img.data.data() + (p * 4 + 3) * 2, &alpha, 2);
                }
            }
            renders.push_back(std::move(img));
        }
        PsdWriteDocument batch;
        std::string error;
        ASSERT_TRUE(assemblePsd(plan, renders, bits, 72, batch, error)) << error;
        std::vector<uint8_t> batchBytes;
        ASSERT_TRUE(writePsd(batch, batchBytes, error)) << error;

        const fs::path dir = fs::temp_directory_path() / ("je_psd_stream_" + std::to_string(::getpid()) + "_" + std::to_string(bits));
        fs::create_directories(dir);
        PsdChannelSpool spool;
        ASSERT_TRUE(spool.open(dir / "x.spool", error)) << error;
        PsdAssembler assembler(plan, bits, 72, W, H, &spool);
        for (size_t i = 0; i < renders.size(); i++) ASSERT_TRUE(assembler.add((int)i, renders[i], error)) << error;
        ASSERT_TRUE(assembler.complete(error)) << error;
        ASSERT_TRUE(writePsdFile(assembler.document(), spool, dir / "x.psd", error)) << error;
        std::ifstream in(dir / "x.psd", std::ios::binary);
        const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(file, batchBytes) << bits << "-bit: the same file";
        EXPECT_LE(assembler.peakBytes(), cap) << bits << "-bit: peak " << assembler.peakBytes() << " of " << plan.jobs.size() << " renders";
        EXPECT_GT(plan.jobs.size() * (size_t)W * H * 8, cap) << "the plan is large enough for the cap to mean something";
        fs::remove_all(dir);
    }
}

TEST(PsdExportPlan, AssemblerRefusesARenderItDoesNotExpect) {
    const PsdExportPlan plan = planPsdExport(document(), {});
    PsdAssembler assembler(plan, 8, 72, 16, 8, nullptr);
    std::string error;
    texsynth::Image wrong = texsynth::Image::make(4, 4, 4, 8);
    EXPECT_FALSE(assembler.add(0, wrong, error));
    EXPECT_NE(error.find("render_failed"), std::string::npos) << error;
    EXPECT_FALSE(assembler.complete(error)) << "no render has arrived";
    EXPECT_FALSE(assembler.add(99999, wrong, error));
}
