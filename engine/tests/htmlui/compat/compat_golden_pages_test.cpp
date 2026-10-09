// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The golden pages against Chromium (phase-2 spec 5.3): every fixtures/compat/pages/*.html is
// loaded through the compat layer like the reference script loads it in Chromium, and checked
// by the five gates of BrowserCompare.h. Pages that need fork features the build lacks
// (<meta name="htmlui-test-requires">) run the diagnostics snapshot and a no-crash render
// only, and say why. Artifacts (engine images, heat maps, metrics) go to
// <binary>/htmlui-test-output/compat/: look at them.
//
// The metrics themselves (CompatBrowserCompare.*) are tested without a GPU on synthetic
// images, on the spec's calibration cases.

#include "BrowserCompare.h"

#include "harness/ImageCompare.h"
#include "harness/TestSystem.h"

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/ForkFeatures.h"

#include "Log.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::compat;
namespace hc = jasperengine::htmlui::compat;

namespace {

    std::vector<std::string> pageNames(){
        std::vector<std::string> names;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(pagesDirectory(), error)){
            if (entry.path().extension() == ".html")
                names.push_back(entry.path().stem().string());
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    bool readText(const std::string& path, std::string& text){
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;
        std::ostringstream stream;
        stream << file.rdbuf();
        text = stream.str();
        return true;
    }

    bool writeText(const std::string& path, const std::string& text){
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
        std::ofstream file(path, std::ios::binary);
        file << text;
        return bool(file);
    }

    // a w x h image of one premultiplied color
    Image solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255){
        Image image;
        image.width = w;
        image.height = h;
        image.rgba.resize(size_t(w) * h * 4);
        for (size_t i = 0; i < image.rgba.size(); i += 4){
            image.rgba[i] = r;
            image.rgba[i + 1] = g;
            image.rgba[i + 2] = b;
            image.rgba[i + 3] = a;
        }
        return image;
    }

    void fill(Image& image, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255){
        for (int y = std::max(0, y0); y < std::min(image.height, y1); y++){
            for (int x = std::max(0, x0); x < std::min(image.width, x1); x++){
                uint8_t* p = &image.rgba[(size_t(y) * image.width + x) * 4];
                p[0] = r;
                p[1] = g;
                p[2] = b;
                p[3] = a;
            }
        }
    }

    // a small "page": background, a panel, a button with a 1px anti-aliased-like border
    Image page(int shiftX = 0, uint8_t panelGreen = 0x66, bool button = true){
        Image image = solid(320, 200, 0x10, 0x14, 0x20);
        fill(image, 40 + shiftX, 30, 280 + shiftX, 170, 0x30, panelGreen, 0xcc);
        if (button){
            fill(image, 80 + shiftX, 80, 240 + shiftX, 120, 0xee, 0xee, 0xee);
            fill(image, 81 + shiftX, 81, 239 + shiftX, 119, 0x7c, 0x3a, 0xed);
        }
        return image;
    }

}

// ---- the metrics (no GPU) -------------------------------------------------------------------------

TEST(CompatBrowserCompare, IdenticalImagesPass){
    const Image a = page();
    const ImageMetrics metrics = compareImages(a, a, 48);
    ASSERT_TRUE(metrics.comparable);
    EXPECT_EQ(metrics.fuzzyBadPixels, 0u);
    EXPECT_EQ(metrics.maxBlock, 0.0);
    EXPECT_TRUE(metrics.passes(kChromiumNoTextGate));
    EXPECT_TRUE(metrics.passes(kEngineGoldenGate));
}

TEST(CompatBrowserCompare, CalibrationCases){
    // spec 5.3: a 1px shift and a small color change pass the Chromium gate; a 4px shift, a
    // missing button and a large color change do not
    const Image reference = page();

    const ImageMetrics shift1 = compareImages(page(1), reference, 48);
    EXPECT_EQ(shift1.fuzzyBadPixels, 0u) << shift1.text();
    EXPECT_TRUE(shift1.passes(kChromiumNoTextGate)) << shift1.text();
    EXPECT_FALSE(shift1.passes(kEngineGoldenGate)) << shift1.text();   // the engine golden sees it

    const ImageMetrics color = compareImages(page(0, 0x77), reference, 48);
    EXPECT_EQ(color.fuzzyBadPixels, 0u) << color.text();
    EXPECT_TRUE(color.passes(kChromiumNoTextGate)) << color.text();

    const ImageMetrics shift4 = compareImages(page(4), reference, 48);
    EXPECT_GT(shift4.fuzzyBadPixels, 0u) << shift4.text();
    EXPECT_GT(shift4.maxBlock, 64.0) << shift4.text();
    EXPECT_FALSE(shift4.passes(kChromiumNoTextGate)) << shift4.text();

    const ImageMetrics missing = compareImages(page(0, 0x66, false), reference, 48);
    EXPECT_GT(missing.fuzzyBadPercent, 5.0) << missing.text();
    EXPECT_FALSE(missing.passes(kChromiumNoTextGate)) << missing.text();

    const ImageMetrics recolor = compareImages(page(0, 0xee), reference, 48);
    EXPECT_FALSE(recolor.passes(kChromiumNoTextGate)) << recolor.text();
}

TEST(CompatBrowserCompare, FuzzyBadCountsBothDirections){
    // a dot only in one image is bad from either side
    Image a = solid(32, 32, 0, 0, 0, 0);
    Image b = a;
    fill(b, 10, 10, 13, 13, 255, 255, 255);
    const ImageMetrics ab = compareImages(a, b, 48);
    const ImageMetrics ba = compareImages(b, a, 48);
    EXPECT_EQ(ab.fuzzyBadPixels, ba.fuzzyBadPixels);
    EXPECT_GE(ab.fuzzyBadPixels, 1u);
    // the block of the dot
    EXPECT_EQ(ab.maxBlockX, 8);
    EXPECT_EQ(ab.maxBlockY, 8);
}

TEST(CompatBrowserCompare, SizesMustMatch){
    const ImageMetrics metrics = compareImages(solid(10, 10, 0, 0, 0), solid(12, 10, 0, 0, 0), 48);
    EXPECT_FALSE(metrics.comparable);
    EXPECT_FALSE(metrics.passes(kChromiumNoTextGate));
    EXPECT_NE(metrics.error.find("10x10"), std::string::npos) << metrics.error;
    EXPECT_FALSE(compareImages(Image(), solid(2, 2, 0, 0, 0), 48).comparable);
}

TEST(CompatBrowserCompare, PremultipliedPngRoundTrip){
    std::vector<uint8_t> straight = {255, 0, 0, 128, 0, 255, 0, 255, 10, 20, 30, 0, 200, 100, 50, 64};
    const Image image = premultiplied(2, 2, straight);
    EXPECT_EQ(image.rgba[0], 128);   // 255 * 128 / 255
    EXPECT_EQ(image.rgba[3], 128);
    EXPECT_EQ(image.rgba[8], 0);     // transparent: no color
    const std::string path = artifactDirectory() + "/selftest/roundtrip.png";
    ASSERT_TRUE(savePng(path, image));
    Image loaded;
    ASSERT_TRUE(loadPng(path, loaded));
    ASSERT_EQ(loaded.width, 2);
    for (size_t i = 0; i < image.rgba.size(); i++)
        EXPECT_NEAR(loaded.rgba[i], image.rgba[i], 1) << i;
}

TEST(CompatBrowserCompare, HeatMapMarksDifferences){
    const Image reference = page();
    const Image actual = page(0, 0x66, false);
    const Image map = heatMap(actual, reference, 48);
    ASSERT_EQ(map.width, reference.width);
    const uint8_t* inside = &map.rgba[(size_t(100) * map.width + 160) * 4];   // where the button is missing
    const uint8_t* outside = &map.rgba[(size_t(10) * map.width + 10) * 4];
    EXPECT_GT(inside[0], inside[1] + 60);
    EXPECT_EQ(outside[0], outside[1]);
    EXPECT_EQ(outside[3], 255);
}

TEST(CompatBrowserCompare, LayoutTolerances){
    LayoutBox box;
    box.id = "a";
    box.w = 300.0f;
    box.h = 40.0f;
    EXPECT_EQ(layoutTolerance(box, 'x'), 3.0f);
    box.tolerance = "text";
    EXPECT_EQ(layoutTolerance(box, 'w'), 12.0f);   // 4 % of 300
    EXPECT_EQ(layoutTolerance(box, 'h'), 6.0f);    // 4 % of 40 is below 6

    ReferenceLayout reference;
    LayoutBox r1;
    r1.id = "panel";
    r1.x = 100.0f;
    r1.y = 50.0f;
    r1.w = 400.0f;
    r1.h = 300.0f;
    LayoutBox r2 = r1;
    r2.id = "spun";
    r2.transformed = true;
    LayoutBox r3 = r1;
    r3.id = "gone";
    reference.elements = {r1, r2, r3};

    LayoutBox e1 = r1;
    e1.x = 102.5f;    // within 3
    e1.h = 304.0f;    // not
    const std::vector<LayoutDifference> differences = compareLayout({e1}, reference);
    ASSERT_EQ(differences.size(), 2u);
    EXPECT_EQ(differences[0].id, "panel");
    EXPECT_EQ(differences[0].field, 'h');
    EXPECT_NE(differences[0].text().find("tolerance 3.00"), std::string::npos) << differences[0].text();
    EXPECT_EQ(differences[1].id, "gone");   // the transformed one is not compared
    EXPECT_EQ(differences[1].field, 0);
}

TEST(CompatBrowserCompare, MissingForkFeatures){
    const hc::ForkFeatures none = hc::ForkFeatures::none();
    EXPECT_EQ(missingForkFeatures({"grid", "pseudo-elements"}, none), (std::vector<std::string>{"grid", "pseudo-elements"}));
    EXPECT_TRUE(missingForkFeatures({"grid", "pseudo-elements"}, hc::ForkFeatures::all()).empty());
    EXPECT_EQ(missingForkFeatures({"grid"}, none.with(hc::ForkFeature::Grid, true)), std::vector<std::string>{});
    // an unknown name is never present
    EXPECT_EQ(missingForkFeatures({"teleportation"}, hc::ForkFeatures::all()), std::vector<std::string>{"teleportation"});
}

// The committed references: every page has its three files, its layout names the page and
// every id it lists exists in the HTML, and the requirements are fork features.
TEST(CompatGoldenReferences, CompleteAndConsistent){
    const std::vector<std::string> pages = pageNames();
    ASSERT_GE(pages.size(), 8u);
    for (const std::string& name : pages){
        SCOPED_TRACE(name);
        Image full, noText;
        EXPECT_TRUE(loadPng(referenceDirectory() + "/" + name + ".png", full));
        EXPECT_TRUE(loadPng(referenceDirectory() + "/" + name + ".notext.png", noText));
        EXPECT_EQ(full.width, 1280);
        EXPECT_EQ(full.height, 720);
        EXPECT_EQ(noText.width, 1280);
        ReferenceLayout layout;
        std::string error;
        ASSERT_TRUE(loadReferenceLayout(referenceDirectory() + "/" + name + ".layout.json", layout, &error)) << error;
        EXPECT_EQ(layout.page, name + ".html");
        EXPECT_EQ(layout.viewportWidth, 1280);
        EXPECT_EQ(layout.viewportHeight, 720);
        EXPECT_FALSE(layout.elements.empty());
        std::string html;
        ASSERT_TRUE(readText(pagesDirectory() + "/" + name + ".html", html));
        for (const LayoutBox& box : layout.elements)
            EXPECT_NE(html.find("id=\"" + box.id + "\""), std::string::npos) << box.id;
        for (const std::string& feature : layout.requires)
            EXPECT_TRUE(missingForkFeatures({feature}, hc::ForkFeatures::all()).empty()) << feature;
    }
}

// ---- the pages ------------------------------------------------------------------------------------

class CompatGoldenPages : public ::testing::TestWithParam<std::string> {};

TEST_P(CompatGoldenPages, MatchesChromium){
    const std::string name = GetParam();
    const std::string artifacts = artifactDirectory();

    ReferenceLayout reference;
    std::string error;
    ASSERT_TRUE(loadReferenceLayout(referenceDirectory() + "/" + name + ".layout.json", reference, &error)) << error;

    BrowserPage browser;
    if (!browser.valid())
        GTEST_SKIP() << "no headless GPU context";
    TestSystem::get().clearLogs();
    ASSERT_TRUE(browser.load(name + ".html", &error)) << error;

    // what compat made of the page, for inspection
    if (const std::shared_ptr<const hc::CompatDocument> compiled = browser.compiled()){
        std::string rcss;
        for (const hc::StylesheetOutput& sheet : compiled->stylesheets)
            rcss += "/* " + (sheet.virtualPath.empty() ? std::string("inline <style>") : sheet.virtualPath) + " */\n" + sheet.rcss + "\n";
        rcss += "/* hints */\n" + compiled->hints.rcss + "\n/* !important */\n" + compiled->important.rcss;
        writeText(artifacts + "/" + name + ".rml", compiled->rml);
        writeText(artifacts + "/" + name + ".rcss", rcss);
    }

    // gate 5: the diagnostics snapshot
    const std::string diagnostics = browser.diagnosticsText();
    const std::string diagPath = engineGoldenDirectory() + "/" + name + ".diag.txt";
    if (isGoldenUpdateMode()){
        ASSERT_TRUE(writeText(diagPath, diagnostics));
    }else{
        std::string expected;
        ASSERT_TRUE(readText(diagPath, expected)) << diagPath << " is missing (JASPER_HTMLUI_UPDATE_GOLDEN=1 writes it)";
        EXPECT_EQ(diagnostics, expected) << "diagnostics of " << name << ".html changed; now:\n" << diagnostics;
    }
    // RmlUi accepts everything compat produced
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "HTMLUI-GEN-0006"), 0u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "HTMLUI-GEN-0006"), 0u);

    ASSERT_TRUE(browser.applyStates(reference, &error)) << error;

    Image engine, engineNoText;
    ASSERT_TRUE(browser.render(engine, true)) << "the page did not draw";
    ASSERT_TRUE(savePng(artifacts + "/" + name + ".png", engine));
    ASSERT_TRUE(browser.render(engineNoText, false)) << "the page did not draw without text";
    ASSERT_TRUE(savePng(artifacts + "/" + name + ".notext.png", engineNoText));

    const std::vector<std::string> missing = missingForkFeatures(reference.requires, hc::ForkFeatures::present());
    if (!missing.empty()){
        std::string list;
        for (const std::string& feature : missing)
            list += (list.empty() ? "" : ", ") + feature;
        std::cout << "[  SKIPPED ] " << name << ": image and layout gates need the fork features " << list
                  << " (phase 3); diagnostics snapshot and render checked\n";
        return;
    }

    std::ostringstream metrics;
    metrics << name << "\n";

    // gate 1: no-text image vs Chromium
    Image referenceNoText;
    ASSERT_TRUE(loadPng(referenceDirectory() + "/" + name + ".notext.png", referenceNoText));
    {
        const ImageMetrics noText = compareImages(engineNoText, referenceNoText, kChromiumNoTextGate.channelThreshold);
        savePng(artifacts + "/" + name + ".notext.diff.png", heatMap(engineNoText, referenceNoText, kChromiumNoTextGate.channelThreshold));
        metrics << kChromiumNoTextGate.name << ": " << noText.text() << "\n";
        EXPECT_TRUE(noText.passes(kChromiumNoTextGate)) << name << " " << kChromiumNoTextGate.name << ": " << noText.text()
                                                        << " (limits " << kChromiumNoTextGate.maxFuzzyBadPercent << " %, "
                                                        << kChromiumNoTextGate.maxBlock << "); heat map " << artifacts << "/" << name
                                                        << ".notext.diff.png";
    }

    // gate 2: layout vs Chromium
    const std::vector<LayoutDifference> layout = compareLayout(browser.layout(reference), reference);
    metrics << "layout: " << layout.size() << " differences\n";
    for (const LayoutDifference& difference : layout){
        metrics << "  " << difference.text() << "\n";
        ADD_FAILURE() << name << " layout: " << difference.text();
    }

    // gate 4: the full image vs Chromium, reported only (stb_truetype and Skia rasterize text differently)
    Image referenceFull;
    if (loadPng(referenceDirectory() + "/" + name + ".png", referenceFull)){
        const ImageMetrics full = compareImages(engine, referenceFull, kChromiumFullGate.channelThreshold);
        savePng(artifacts + "/" + name + ".diff.png", heatMap(engine, referenceFull, kChromiumFullGate.channelThreshold));
        metrics << kChromiumFullGate.name << ": " << full.text() << "\n";
        std::cout << "[ METRICS  ] " << name << " full image vs Chromium: " << full.text() << "\n";
    }

    // gate 3: the engine golden, our own output
    const std::string goldenPath = engineGoldenDirectory() + "/" + name + ".png";
    if (isGoldenUpdateMode()){
#if defined(SOKOL_VULKAN)
        ADD_FAILURE() << "goldens are only updated on glcore (HTMLUI_BACKEND=glcore)";
#else
        ASSERT_TRUE(savePng(goldenPath, engine));
#endif
    }else{
        Image golden;
        ASSERT_TRUE(loadPng(goldenPath, golden)) << goldenPath << " is missing (JASPER_HTMLUI_UPDATE_GOLDEN=1 writes it)";
        const ImageMetrics regression = compareImages(engine, golden, kEngineGoldenGate.channelThreshold);
        if (!regression.passes(kEngineGoldenGate))
            savePng(artifacts + "/" + name + ".engine.diff.png", heatMap(engine, golden, kEngineGoldenGate.channelThreshold));
        metrics << kEngineGoldenGate.name << ": " << regression.text() << "\n";
        EXPECT_TRUE(regression.passes(kEngineGoldenGate)) << name << " " << kEngineGoldenGate.name << ": " << regression.text();
    }

    writeText(artifacts + "/" + name + ".metrics.txt", metrics.str());
    std::cout << metrics.str();
}

INSTANTIATE_TEST_SUITE_P(CompatGolden, CompatGoldenPages, ::testing::ValuesIn(pageNames()),
                         [](const ::testing::TestParamInfo<std::string>& info){
                             std::string name = info.param;
                             std::replace(name.begin(), name.end(), '-', '_');
                             return name;
                         });
