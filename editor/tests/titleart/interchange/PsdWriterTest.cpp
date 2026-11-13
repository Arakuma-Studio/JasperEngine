// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Our layered PSD writer (docs/specs/S87-title-localisation-interchange.md R39, D10, AC20):
// RoundTrip8 / RoundTrip16 read the writer's bytes back with our reader (layers, groups,
// clipping, masks, blend keys, names, the merged image, resources) and check determinism and the
// size cap; PsdReadAccepts runs tools/mcp/psdread.py, the independent standard-library reader,
// on the same bytes, and its red control flips one length byte and expects psdread to fail
// naming the offset.

#include <gtest/gtest.h>

#include "titleart/interchange/IccSrgb.h"
#include "titleart/interchange/psd/PsdAdjustments.h"
#include "titleart/interchange/psd/PsdReader.h"
#include "titleart/interchange/psd/PsdWriter.h"
#include "io/AtomicFile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "support/PosixCompat.h"

using namespace jasperengine::editor;
using namespace jasperengine::editor::psd;
namespace fs = std::filesystem;

namespace {

    constexpr int W = 40, H = 30;

    // a layer of `w` x `h` at (x, y): colour `rgb`, opaque where `inside(px, py)`
    PsdWriteLayer pixels(const std::string& name, int x, int y, int w, int h, const uint16_t rgb[3], int bps,
                         bool (*inside)(int, int) = nullptr) {
        PsdWriteLayer l;
        l.name = name;
        l.x = x;
        l.y = y;
        l.width = w;
        l.height = h;
        l.rgba.assign((size_t)w * h * 4 * bps, 0);
        const uint16_t maxValue = bps == 2 ? 65535 : 255;
        for (int py = 0; py < h; py++) {
            for (int px = 0; px < w; px++) {
                const bool on = inside ? inside(px, py) : true;
                const uint16_t v[4] = {rgb[0], rgb[1], rgb[2], (uint16_t)(on ? maxValue : 0)};
                for (int c = 0; c < 4; c++) {
                    const size_t i = ((size_t)py * w + px) * 4 + c;
                    // transparent pixels carry a colour the writer must drop
                    const uint16_t value = on || c == 3 ? v[c] : (uint16_t)(maxValue / 2);
                    if (bps == 2) std::memcpy(l.rgba.data() + i * 2, &value, 2);
                    else l.rgba[i] = (uint8_t)value;
                }
            }
        }
        return l;
    }

    PsdWriteDocument document(int bps) {
        PsdWriteDocument doc;
        doc.width = W;
        doc.height = H;
        doc.bitDepth = bps * 8;
        doc.dpi = 144;
        const uint16_t scale = bps == 2 ? 257 : 1;
        const uint16_t red[3] = {(uint16_t)(200 * scale), (uint16_t)(10 * scale), (uint16_t)(20 * scale)};
        const uint16_t blue[3] = {(uint16_t)(5 * scale), (uint16_t)(6 * scale), (uint16_t)(250 * scale)};
        const uint16_t grey[3] = {(uint16_t)(90 * scale), (uint16_t)(90 * scale), (uint16_t)(90 * scale)};
        doc.layers.push_back(pixels("Background", 0, 0, W, H, grey, bps));
        PsdWriteLayer group;
        group.name = "Logo";
        group.group = true;
        group.blendKey = "pass";
        PsdWriteLayer base = pixels("T\xc3\xadtulo \xe2\x9c\x93", 4, 4, 20, 10, red, bps, [](int x, int) { return x >= 3; });
        base.hasMask = true;
        base.maskX = 0;
        base.maskY = 0;
        base.maskWidth = W;
        base.maskHeight = H;
        base.mask.assign((size_t)W * H * bps, 0);
        for (int i = 0; i < W * H / 2; i++) {
            if (bps == 2) {
                const uint16_t v = 65535;
                std::memcpy(base.mask.data() + (size_t)i * 2, &v, 2);
            } else {
                base.mask[(size_t)i] = 255;
            }
        }
        group.children.push_back(base);
        PsdWriteLayer clipped = pixels("Shine", 6, 6, 8, 4, blue, bps);
        clipped.clipping = true;
        clipped.blendKey = "scrn";
        clipped.opacity = 128;
        group.children.push_back(clipped);
        doc.layers.push_back(group);
        PsdWriteLayer hidden = pixels("Hidden", 30, 20, 5, 5, blue, bps);
        hidden.visible = false;
        hidden.blendKey = "mul ";
        hidden.fillOpacity = 100;
        doc.layers.push_back(hidden);
        PsdWriteLayer levels;
        levels.name = "Levels";
        std::string key;
        titleAdjustmentToPsd(jasperengine::titleart::Json{{"type", "levels"}, {"channels", {{"rgb", {{"in_black", 12}, {"in_white", 240},
                                                          {"gamma", 1.5}, {"out_black", 0}, {"out_white", 255}}}}}}, key, levels.adjustmentData);
        levels.adjustmentKey = key;
        doc.layers.push_back(levels);
        doc.merged.assign((size_t)W * H * 4 * bps, 0);
        for (size_t i = 0; i < (size_t)W * H * 4 * bps; i++) doc.merged[i] = (uint8_t)(i * 7);
        return doc;
    }

    uint16_t sampleOf(const PsdPlane& p, size_t i, int bps) {
        if (bps == 1) return p.samples[i];
        uint16_t v;
        std::memcpy(&v, p.samples.data() + i * 2, 2);
        return v;
    }

    void spoolAll(std::vector<PsdWriteLayer>& layers, int bitDepth, PsdChannelSpool& spool) {
        std::string error;
        for (PsdWriteLayer& l : layers) {
            if (l.group) {
                spoolAll(l.children, bitDepth, spool);
                continue;
            }
            if (l.adjustmentKey.empty()) {
                ASSERT_TRUE(psdSpoolPixels(l, bitDepth, spool, error)) << error;
            }
            if (l.hasMask) {
                ASSERT_TRUE(psdSpoolMask(l, bitDepth, spool, error)) << error;
            }
        }
    }

    size_t workingFiles(const fs::path& dir) {
        size_t count = 0;
        for (const auto& entry : fs::directory_iterator(dir)) count += jasperengine::io::isTemporarySiblingName(entry.path().filename().string()) ? 1 : 0;
        return count;
    }

    // R49: the file assembled from the spool is the same file, with no layer left in memory
    void streamed(int bps) {
        std::vector<uint8_t> batch;
        std::string error;
        ASSERT_TRUE(writePsd(document(bps), batch, error)) << error;
        const fs::path dir = fs::temp_directory_path() / ("je_psd_spool_" + std::to_string(::getpid()) + "_" + std::to_string(bps));
        fs::create_directories(dir);
        const fs::path spoolPath = dir / "x.spool";
        {
            PsdWriteDocument doc = document(bps);
            PsdChannelSpool spool;
            ASSERT_TRUE(spool.open(spoolPath, error)) << error;
            spoolAll(doc.layers, bps * 8, spool);
            ASSERT_TRUE(psdSpoolMerged(doc, spool, error)) << error;
            EXPECT_TRUE(doc.merged.empty()) << "the merged image is freed";
            for (const PsdWriteLayer& l : doc.layers) EXPECT_TRUE(l.rgba.empty()) << l.name << " is freed";
            ASSERT_TRUE(writePsdFile(doc, spool, dir / "x.psd", error)) << error;
            EXPECT_EQ(workingFiles(dir), 1u) << "the working file, a temporary sibling of the spool's name";
        }
        EXPECT_EQ(workingFiles(dir), 0u) << "the working file goes with the spool";
        std::ifstream in(dir / "x.psd", std::ios::binary);
        const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(file, batch) << "the same bytes as writing in memory";
        fs::remove_all(dir);
    }

    void roundTrip(int bps) {
        const PsdWriteDocument doc = document(bps);
        std::vector<uint8_t> bytes, again;
        std::string error;
        ASSERT_TRUE(writePsd(doc, bytes, error)) << error;
        ASSERT_TRUE(writePsd(doc, again, error));
        EXPECT_EQ(bytes, again) << "the same document gives the same bytes";
        PsdFile file;
        ASSERT_TRUE(readPsd(bytes.data(), bytes.size(), file, error)) << error;
        EXPECT_EQ(file.version, 1);
        EXPECT_EQ(file.width, W);
        EXPECT_EQ(file.depth, bps * 8);
        EXPECT_EQ(file.mode, 3);
        EXPECT_DOUBLE_EQ(file.dpi, 144.0);
        EXPECT_EQ(file.icc, srgbIccProfile());
        EXPECT_TRUE(file.hasRealMergedData);
        EXPECT_EQ(file.layersFrom16, bps == 2) << "16-bit layers live in Lr16";
        // records: background, end marker, base, shine, group, hidden, levels
        ASSERT_EQ(file.layers.size(), 7u);
        const std::vector<PsdNode> tree = psdTree(file);
        ASSERT_EQ(tree.size(), 4u);
        ASSERT_EQ(tree[1].children.size(), 2u);
        const PsdLayerRecord& group = file.layers[(size_t)tree[1].layer];
        EXPECT_EQ(group.name, "Logo");
        EXPECT_EQ(group.section, 1);
        EXPECT_EQ(group.sectionBlend, "pass");
        const PsdLayerRecord& base = file.layers[(size_t)tree[1].children[0].layer];
        EXPECT_EQ(base.name, "T\xc3\xadtulo \xe2\x9c\x93") << "luni keeps the Unicode name";
        // trimmed to its alpha: x 3..19 of the 20 x 10 pixels at (4, 4)
        EXPECT_EQ(base.bounds.left, 7);
        EXPECT_EQ(base.bounds.top, 4);
        EXPECT_EQ(base.bounds.width(), 17);
        EXPECT_EQ(base.bounds.height(), 10);
        EXPECT_EQ(sampleOf(base.planes[0], 0, bps), bps == 2 ? 200 * 257 : 200);
        EXPECT_EQ(sampleOf(base.alpha, 0, bps), bps == 2 ? 65535 : 255);
        ASSERT_TRUE(base.hasMask);
        EXPECT_EQ(base.maskBounds.width(), W);
        EXPECT_EQ(sampleOf(base.maskPlane, 0, bps), bps == 2 ? 65535 : 255);
        EXPECT_EQ(sampleOf(base.maskPlane, (size_t)W * H - 1, bps), 0);
        const PsdLayerRecord& shine = file.layers[(size_t)tree[1].children[1].layer];
        EXPECT_TRUE(shine.clipping);
        EXPECT_EQ(shine.blendKey, "scrn");
        EXPECT_EQ(shine.opacity, 128);
        const PsdLayerRecord& hidden = file.layers[(size_t)tree[2].layer];
        EXPECT_FALSE(hidden.visible);
        EXPECT_EQ(hidden.blendKey, "mul ");
        EXPECT_EQ(hidden.fillOpacity, 100);
        const PsdLayerRecord& levels = file.layers[(size_t)tree[3].layer];
        EXPECT_EQ(levels.adjustmentKey, "levl");
        jasperengine::titleart::Json adjustment;
        ASSERT_TRUE(psdAdjustmentToTitle(levels.adjustmentKey, levels.adjustmentData, adjustment, error)) << error;
        EXPECT_DOUBLE_EQ(adjustment["channels"]["rgb"]["in_black"].get<double>(), 12.0);
        EXPECT_DOUBLE_EQ(adjustment["channels"]["rgb"]["gamma"].get<double>(), 1.5);
        // ids are sequential
        for (size_t i = 0; i < file.layers.size(); i++) EXPECT_EQ(file.layers[i].id, (uint32_t)(i + 1));
        // the merged image as written
        EXPECT_EQ(psdMergedRgba(file, bps * 8), doc.merged);
    }

}

TEST(PsdWriterTest, StreamedFileEqualsTheBatchFile8) {
    streamed(1);
}

TEST(PsdWriterTest, StreamedFileEqualsTheBatchFile16) {
    streamed(2);
}

TEST(PsdWriter, RoundTrip8) {
    roundTrip(1);
}

TEST(PsdWriter, RoundTrip16) {
    roundTrip(2);
}

TEST(PsdWriter, TransparentColourIsZero) {
    PsdWriteDocument doc = document(1);
    std::vector<uint8_t> bytes;
    std::string error;
    ASSERT_TRUE(writePsd(doc, bytes, error));
    PsdFile file;
    ASSERT_TRUE(readPsd(bytes.data(), bytes.size(), file, error)) << error;
    // the background is opaque; the base's left columns were dropped by the trim, so take a layer
    // with holes: a 1 px transparent pixel inside the trimmed box keeps colour 0
    PsdWriteDocument holed = doc;
    holed.layers.resize(1);
    holed.layers[0].rgba[(5 * W + 5) * 4 + 3] = 0;
    ASSERT_TRUE(writePsd(holed, bytes, error));
    ASSERT_TRUE(readPsd(bytes.data(), bytes.size(), file, error)) << error;
    EXPECT_EQ(file.layers[0].planes[0].samples[5 * W + 5], 0);
    EXPECT_EQ(file.layers[0].planes[0].samples[5 * W + 6], 90);
}

TEST(PsdWriter, RefusesTooLarge) {
    PsdWriteDocument doc;
    doc.width = PSD_MAX_SIDE + 1;
    doc.height = 4;
    std::vector<uint8_t> bytes;
    std::string error;
    EXPECT_FALSE(writePsd(doc, bytes, error));
    EXPECT_EQ(error.rfind("psd_too_large", 0), 0u) << error;
    doc.width = 4;
    doc.bitDepth = 32;
    EXPECT_FALSE(writePsd(doc, bytes, error));
}

TEST(PsdWriter, NativeAdjustmentsRoundTrip) {
    using jasperengine::titleart::Json;
    const Json cases[] = {
        Json{{"type", "curves"}, {"channels", {{"rgb", {{"points", Json::array({Json{{"x", 0}, {"y", 0}}, Json{{"x", 0.5}, {"y", 0.75}}, Json{{"x", 1}, {"y", 1}}})}}}}}},
        Json{{"type", "hue_saturation"}, {"hue", 40}, {"saturation", -20}, {"lightness", 5}, {"colorize", false}},
        Json{{"type", "brightness_contrast"}, {"brightness", 30}, {"contrast", -15}},
        Json{{"type", "invert"}},
        Json{{"type", "posterize"}, {"levels", 6}},
        Json{{"type", "threshold"}, {"level", 99}},
    };
    for (const Json& c : cases) {
        std::string key, error;
        std::vector<uint8_t> data;
        ASSERT_TRUE(titleAdjustmentToPsd(c, key, data)) << c.dump();
        Json back;
        ASSERT_TRUE(psdAdjustmentToTitle(key, data, back, error)) << error;
        EXPECT_EQ(back["type"], c["type"]);
        for (const char* k : {"hue", "saturation", "lightness", "brightness", "contrast", "levels", "level"}) {
            if (c.contains(k)) {
                EXPECT_DOUBLE_EQ(back[k].get<double>(), c[k].get<double>()) << k;
            }
        }
        if (c["type"] == "curves") {
            EXPECT_NEAR(back["channels"]["rgb"]["points"][1]["y"].get<double>(), 0.75, 1.0 / 255);
        }
    }
    std::string key;
    std::vector<uint8_t> data;
    EXPECT_FALSE(titleAdjustmentToPsd(Json{{"type", "vibrance"}}, key, data)) << "vibrance is written baked";
}

// AC20: the independent reader accepts the writer's bytes; red control: one flipped length byte
TEST(PsdWriter, PsdReadAccepts) {
    if (std::system("python3 --version > /dev/null 2>&1") != 0) GTEST_SKIP() << "python3 is not on this machine";
    const fs::path dir = fs::temp_directory_path() / "je_psdwriter_psdread";
    fs::create_directories(dir);
    const std::string script = std::string(JASPER_TEST_REPO_DIR) + "/tools/mcp/psdread.py";
    for (int bps : {1, 2}) {
        std::vector<uint8_t> bytes;
        std::string error;
        ASSERT_TRUE(writePsd(document(bps), bytes, error)) << error;
        const fs::path good = dir / ("good" + std::to_string(bps * 8) + ".psd");
        std::ofstream(good, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
        EXPECT_EQ(std::system(("python3 -I \"" + script + "\" --check --icc \"" + good.string() + "\"").c_str()), 0) << bps * 8 << "-bit";
        // the layer and mask section's length, 4 bytes past the header, colour mode data and resources
        size_t at = 26;
        at += 4 + ((size_t)bytes[at] << 24 | (size_t)bytes[at + 1] << 16 | (size_t)bytes[at + 2] << 8 | bytes[at + 3]);
        at += 4 + ((size_t)bytes[at] << 24 | (size_t)bytes[at + 1] << 16 | (size_t)bytes[at + 2] << 8 | bytes[at + 3]);
        bytes[at + 1] ^= 0x40;
        const fs::path bad = dir / ("bad" + std::to_string(bps * 8) + ".psd");
        std::ofstream(bad, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
        const std::string out = (dir / "bad.txt").string();
        EXPECT_NE(std::system(("python3 -I \"" + script + "\" --check \"" + bad.string() + "\" 2> \"" + out + "\"").c_str()), 0);
        std::ifstream in(out);
        const std::string message((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(message.find("offset"), std::string::npos) << message;
    }
    fs::remove_all(dir);
}
