// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The phase-3 browser-parity fixtures against Chromium (phase-3 spec 5.2): every
// fixtures/p3/<area>/<name>.html with its <name>.json sidecar is loaded through the compat layer,
// its steps run (FixtureRunner), and every capture is compared with Chromium's
// (golden/browser/<area>/<name>[.<capture>].png, <name>.boxes.json; written by
// tools/htmlui/render-reference.mjs --fixtures):
//
//   image: the strict metric (compareStrict: channel 24 with a 3x3 neighbourhood, bad <= 0.1 %, no
//          4-connected bad cluster above 16 pixels); reported only for text-heavy fixtures
//          ("boxes": true: stb_truetype and Skia rasterize glyphs differently)
//   boxes: the border box of every element with an id within 1 px (elements under a CSS transform
//          are left to the image)
//
// A sidecar may override the gate ("tolerance") only with a "comment" saying why. Fixtures listed in
// fixtures/p3/known-failures.txt are expected to fail (reported as KNOWN; the test fails when one
// passes). The harness's own fixtures (area testkit) check that a fixture matches, that a 1 px offset
// is within the tolerance and that a 2 px offset is reported ("selfTest" in the sidecar: the reference
// had CSS injected). Artifacts (engine images, heat maps, metrics, the compat output) go to
// <binary>/htmlui-test-output/browser/<area>/: look at them.

#include "FixtureRunner.h"

#include "harness/ImageCompare.h"

#include "htmlui/compat/DocumentLoader.h"

#include "DefaultFont.h"

#include "json.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::compat;
namespace hc = jasperengine::htmlui::compat;

namespace {

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

    const FixtureList& browserKnownFailures(){
        static const FixtureList list = loadFixtureList(browserFixturesDirectory() + "/known-failures.txt");
        return list;
    }

    std::string captureFile(const BrowserFixture& fixture, const std::string& suffix){
        return fixture.area + "/" + fixture.name + (suffix.empty() ? "" : "." + suffix) + ".png";
    }

    // golden/browser/<area>/<name>.boxes.json: capture suffix -> boxes
    bool loadReferenceBoxes(const std::string& path, std::map<std::string, std::vector<LayoutBox>>& captures, std::string* error){
        captures.clear();
        std::string text;
        if (!readText(path, text)){
            if (error)
                *error = "cannot read " + path;
            return false;
        }
        try{
            const nlohmann::json json = nlohmann::json::parse(text);
            for (const auto& capture : json.at("captures").items()){
                std::vector<LayoutBox>& boxes = captures[capture.key()];
                for (const nlohmann::json& element : capture.value()){
                    LayoutBox box;
                    box.id = element.at("id").get<std::string>();
                    box.tag = element.value("tag", "");
                    box.x = element.at("x").get<float>();
                    box.y = element.at("y").get<float>();
                    box.w = element.at("w").get<float>();
                    box.h = element.at("h").get<float>();
                    box.transformed = element.value("transformed", false);
                    boxes.push_back(std::move(box));
                }
            }
        }catch (const std::exception& exception){
            if (error)
                *error = path + ": " + exception.what();
            return false;
        }
        return true;
    }

    // every reference element that is not transformed against the engine box of the same id, within `tolerance`
    std::vector<std::string> compareBoxes(const std::vector<LayoutBox>& engine, const std::vector<LayoutBox>& reference, float tolerance){
        std::vector<std::string> differences;
        for (const LayoutBox& expected : reference){
            if (expected.transformed)
                continue;
            const auto found = std::find_if(engine.begin(), engine.end(), [&](const LayoutBox& box){ return box.id == expected.id; });
            if (found == engine.end()){
                differences.push_back("#" + expected.id + ": no such element in the engine document");
                continue;
            }
            const std::pair<char, std::pair<float, float>> fields[] = {
                {'x', {found->x, expected.x}}, {'y', {found->y, expected.y}}, {'w', {found->w, expected.w}}, {'h', {found->h, expected.h}}};
            for (const auto& field : fields){
                if (std::fabs(field.second.first - field.second.second) > tolerance){
                    char buffer[200];
                    std::snprintf(buffer, sizeof(buffer), "#%s %c: engine %.2f, Chromium %.2f (difference %.2f, tolerance %.2f)", expected.id.c_str(),
                                  field.first, field.second.first, field.second.second, field.second.first - field.second.second, tolerance);
                    differences.push_back(buffer);
                }
            }
        }
        return differences;
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

    // a transparent 200x120 image with a solid box (anti-aliased-like 1 px edge in a mid tone)
    Image boxImage(int shift = 0){
        Image image = solid(200, 120, 0, 0, 0, 0);
        fill(image, 40 + shift, 20, 160 + shift, 100, 0x80, 0x40, 0x20);
        fill(image, 39 + shift, 20, 40 + shift, 100, 0x40, 0x20, 0x10, 0x80);
        return image;
    }

}

// ---- the strict metric (no GPU) ---------------------------------------------------------------------

TEST(BrowserCompareStrict, IdenticalAndOnePixelShiftsPass){
    const Image reference = boxImage();
    const StrictMetrics same = compareStrict(reference, reference);
    ASSERT_TRUE(same.comparable);
    EXPECT_EQ(same.badPixels, 0u);
    EXPECT_EQ(same.largestCluster, 0u);
    EXPECT_TRUE(same.passes()) << same.text();
    // a 1 px shift is anti-aliasing and rasterizer placement: the 3x3 neighbourhood accepts it
    const StrictMetrics shifted = compareStrict(boxImage(1), reference);
    EXPECT_EQ(shifted.badPixels, 0u) << shifted.text();
    EXPECT_TRUE(shifted.passes()) << shifted.text();
    // a difference of at most the threshold is no difference
    Image tinted = reference;
    fill(tinted, 40, 20, 160, 100, 0x80 + 24, 0x40, 0x20 - 20);
    EXPECT_EQ(compareStrict(tinted, reference).badPixels, 0u);
}

TEST(BrowserCompareStrict, TwoPixelShiftFails){
    // the box's left and right edges: two 80-pixel columns each, one cluster per column pair
    const StrictMetrics metrics = compareStrict(boxImage(2), boxImage());
    EXPECT_FALSE(metrics.passes()) << metrics.text();
    EXPECT_GT(metrics.largestCluster, 16u) << metrics.text();
    EXPECT_EQ(metrics.clusterMinY, 20);
    EXPECT_EQ(metrics.clusterMaxY, 99);
    const Image map = strictHeatMap(boxImage(2), boxImage());
    ASSERT_EQ(map.width, 200);
    const uint8_t* bad = &map.rgba[(size_t(50) * map.width + 40) * 4];
    EXPECT_GT(bad[0], bad[1] + 60);
}

TEST(BrowserCompareStrict, ClusterAndPercentLimits){
    const Image reference = solid(200, 200, 10, 10, 10);
    // 16 scattered single pixels (0.04 %): passes
    Image speckles = reference;
    for (int i = 0; i < 16; i++)
        fill(speckles, 5 + i * 12, 7 + i * 11, 6 + i * 12, 8 + i * 11, 250, 250, 250);
    const StrictMetrics fine = compareStrict(speckles, reference);
    EXPECT_EQ(fine.badPixels, 16u) << fine.text();
    EXPECT_EQ(fine.largestCluster, 1u) << fine.text();
    EXPECT_TRUE(fine.passes()) << fine.text();
    // one 4x4 blob is a cluster of 16: passes; 4x5 = 20: fails although it is 0.05 % of the image
    Image blob = reference;
    fill(blob, 50, 50, 54, 54, 250, 0, 0);
    EXPECT_TRUE(compareStrict(blob, reference).passes()) << compareStrict(blob, reference).text();
    fill(blob, 50, 54, 54, 55, 250, 0, 0);
    const StrictMetrics big = compareStrict(blob, reference);
    EXPECT_EQ(big.largestCluster, 20u) << big.text();
    EXPECT_FALSE(big.passes()) << big.text();
    // 0.2 % of single pixels (80 of 40000, no cluster above 1): fails on the share
    Image many = reference;
    for (int i = 0; i < 80; i++)
        fill(many, (i % 10) * 20 + 5, (i / 10) * 20 + 5, (i % 10) * 20 + 6, (i / 10) * 20 + 6, 250, 250, 250);
    const StrictMetrics share = compareStrict(many, reference);
    EXPECT_GT(share.badPercent, 0.1) << share.text();
    EXPECT_FALSE(share.passes()) << share.text();
    // a gate override
    StrictGate loose;
    loose.maxBadPercent = 1.0;
    loose.maxCluster = 32;
    EXPECT_TRUE(compareStrict(many, reference, loose).passes());
}

TEST(BrowserCompareStrict, SizesMustMatch){
    const StrictMetrics metrics = compareStrict(solid(10, 10, 0, 0, 0), solid(12, 10, 0, 0, 0));
    EXPECT_FALSE(metrics.comparable);
    EXPECT_FALSE(metrics.passes());
    EXPECT_NE(metrics.error.find("10x10"), std::string::npos) << metrics.error;
    EXPECT_FALSE(compareStrict(Image(), solid(2, 2, 0, 0, 0)).comparable);
}

// ---- the committed data ----------------------------------------------------------------------------

namespace {

    // The tables of a TrueType font: tag -> bytes.
    std::map<std::string, std::string> fontTables(const std::string& font){
        std::map<std::string, std::string> result;
        if (font.size() < 12)
            return result;
        auto u16 = [&](size_t at){ return (uint32_t(uint8_t(font[at])) << 8) | uint8_t(font[at + 1]); };
        auto u32 = [&](size_t at){ return (u16(at) << 16) | u16(at + 2); };
        const uint32_t count = u16(4);
        for (uint32_t i = 0; i < count && 12 + 16 * (i + 1) <= font.size(); i++){
            const size_t record = 12 + 16 * i;
            const uint32_t offset = u32(record + 8), length = u32(record + 12);
            if (size_t(offset) + length <= font.size())
                result[font.substr(record, 4)] = font.substr(offset, length);
        }
        return result;
    }

}

// The fixtures' Roboto is the engine's built-in face (DefaultFont.h) with a 'post' table: Chromium's font sanitizer
// rejects a TrueType font without one (and draws another face), so the fixtures load this copy.
TEST(BrowserFixtures, FontsAreTheEnginesFaces){
    const std::string roboto(reinterpret_cast<const char*>(roboto_regular_ttf), size_t(roboto_regular_ttf_len));
    std::string ahem;
    ASSERT_TRUE(readText(htmluiTestSourceDirectory() + "/fonts/Ahem.ttf", ahem));
    EXPECT_EQ(ahem.size(), 12480u);

    // the copy Chromium loads: the same tables, plus 'post' (and the head checksum adjustment)
    std::string post;
    ASSERT_TRUE(readText(htmluiTestSourceDirectory() + "/fonts/Roboto-Regular-subset-post.ttf", post));
    std::map<std::string, std::string> original = fontTables(roboto), copy = fontTables(post);
    ASSERT_FALSE(original.empty());
    ASSERT_EQ(copy.count("post"), 1u);
    EXPECT_EQ(original.count("post"), 0u);
    copy.erase("post");
    ASSERT_EQ(original.size(), copy.size());
    for (auto& entry : original){
        ASSERT_EQ(copy.count(entry.first), 1u) << entry.first;
        if (entry.first == "head"){
            // (checkSumAdjustment, bytes 8-11, differs)
            entry.second.replace(8, 4, 4, '\0');
            copy[entry.first].replace(8, 4, 4, '\0');
        }
        EXPECT_EQ(entry.second, copy[entry.first]) << entry.first;
    }
}

// Every local font a Chromium-rendered fixture loads (browser-parity, Taffy and compat pages) exists and has the
// tables Chromium's sanitizer requires ('post' among them): a font Chromium rejects makes it draw another face, and
// the reference no longer shows the engine's text.
TEST(BrowserFixtures, FontFacesLoadInChromium){
    namespace fs = std::filesystem;
    const fs::path fixtures = fs::path(htmluiTestSourceDirectory()) / "fixtures";
    size_t checked = 0;
    for (const char* area : {"p3", "taffy", "compat/pages"}){
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(fixtures / area, ec); it != fs::recursive_directory_iterator(); it.increment(ec)){
            const fs::path file = it->path();
            const std::string extension = file.extension().string();
            if (ec || !it->is_regular_file() || (extension != ".html" && extension != ".css"))
                continue;
            std::string text;
            ASSERT_TRUE(readText(file.string(), text)) << file;
            // url("...ttf"), url('...otf'), url(...woff2)
            size_t at = 0;
            while ((at = text.find("url(", at)) != std::string::npos){
                at += 4;
                size_t end = text.find(')', at);
                if (end == std::string::npos)
                    break;
                std::string url = text.substr(at, end - at);
                url.erase(std::remove(url.begin(), url.end(), '"'), url.end());
                url.erase(std::remove(url.begin(), url.end(), '\''), url.end());
                const std::string lower = [&]{ std::string l = url; for (char& c : l) c = char(std::tolower((unsigned char)c)); return l; }();
                auto endsWith = [&](const char* suffix){
                    const size_t n = std::strlen(suffix);
                    return lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0;
                };
                if (!(endsWith(".ttf") || endsWith(".otf") || endsWith(".woff") || endsWith(".woff2")) || lower.find("://") != std::string::npos ||
                    lower.rfind("data:", 0) == 0)
                    continue;
                const fs::path font = (file.parent_path() / url).lexically_normal();
                SCOPED_TRACE(file.string() + ": " + url);
                std::string bytes;
                ASSERT_TRUE(readText(font.string(), bytes)) << font << " is missing";
                if (endsWith(".ttf") || endsWith(".otf")){
                    const std::map<std::string, std::string> tables = fontTables(bytes);
                    for (const char* required : {"cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post"})
                        EXPECT_EQ(tables.count(required), 1u) << font << " has no '" << required << "' table";
                }
                checked++;
            }
        }
    }
    EXPECT_GT(checked, 20u);
}

// manifest.json names every area and its owner; every fixture's sidecar loads, overrides its gate only
// with a comment, uses selfTest only in testkit, and has its references (one image per capture, boxes
// for each capture, ids that the HTML has); known failures name fixtures and phase-3 owners.
TEST(BrowserFixtures, ManifestAndReferencesAreConsistent){
    std::string manifestText;
    ASSERT_TRUE(readText(browserFixturesDirectory() + "/manifest.json", manifestText));
    const nlohmann::json manifest = nlohmann::json::parse(manifestText);
    std::set<std::string> areas;
    for (const auto& area : manifest.at("areas").items()){
        areas.insert(area.key());
        EXPECT_TRUE(isPhase3Package(area.value().value("owner", ""))) << "area " << area.key() << ": owner";
    }
    const nlohmann::json& defaults = manifest.at("defaults").at("tolerance");
    EXPECT_EQ(defaults.value("channel", 0), kStrictGate.channelThreshold);
    EXPECT_EQ(defaults.value("badPercent", 0.0), kStrictGate.maxBadPercent);
    EXPECT_EQ(defaults.value("cluster", size_t(0)), kStrictGate.maxCluster);
    EXPECT_EQ(defaults.value("box", 0.0f), 1.0f);
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(browserFixturesDirectory(), ec)){
        if (entry.is_directory()){
            EXPECT_TRUE(areas.count(entry.path().filename().string())) << "fixtures/p3/" << entry.path().filename().string()
                                                                       << " is not an area of manifest.json";
        }
    }

    const std::vector<BrowserFixture> fixtures = listBrowserFixtures();
    ASSERT_GE(fixtures.size(), 4u) << "the testkit fixtures";
    std::string version;
    EXPECT_TRUE(readText(browserReferenceDirectory() + "/VERSION", version));
    EXPECT_NE(version.find("Chromium 153.0.8010.12"), std::string::npos) << version;
    for (const BrowserFixture& fixture : fixtures){
        SCOPED_TRACE(fixture.id());
        FixtureSidecar sidecar;
        std::string error;
        ASSERT_TRUE(loadSidecar(browserFixturesDirectory() + "/" + fixture.area + "/" + fixture.name + ".json", sidecar, &error)) << error;
        if (sidecar.toleranceOverridden){
            EXPECT_FALSE(sidecar.comment.empty()) << "a tolerance override needs a comment (spec 5.2)";
        }
        if (sidecar.sampleCount != 4){
            EXPECT_FALSE(sidecar.comment.empty()) << "\"samples\" needs a comment";
        }
        // a text-heavy fixture (its full image reported only) keeps a strict image gate without its text
        if (sidecar.boxes){
            EXPECT_TRUE(sidecar.noText) << "\"boxes\": true (text-heavy) needs \"noText\": true";
        }
        if (!sidecar.referenceCss.empty() || sidecar.expectMismatch){
            EXPECT_EQ(fixture.area, "testkit") << "selfTest is for the harness's own fixtures";
        }
        std::map<std::string, std::vector<LayoutBox>> boxes;
        ASSERT_TRUE(loadReferenceBoxes(browserReferenceDirectory() + "/" + fixture.area + "/" + fixture.name + ".boxes.json", boxes, &error))
            << error << ": node tools/htmlui/render-reference.mjs --fixtures " << fixture.id();
        std::string html;
        ASSERT_TRUE(readText(browserFixturesDirectory() + "/" + fixture.area + "/" + fixture.name + ".html", html));
        for (const std::string& suffix : sidecar.captures()){
            Image image;
            EXPECT_TRUE(loadPng(browserReferenceDirectory() + "/" + captureFile(fixture, suffix), image)) << captureFile(fixture, suffix);
            EXPECT_EQ(image.width, sidecar.width);
            EXPECT_EQ(image.height, sidecar.height);
            if (sidecar.noText){
                const std::string noTextFile = captureFile(fixture, suffix);
                Image noText;
                EXPECT_TRUE(loadPng(browserReferenceDirectory() + "/" + noTextFile.substr(0, noTextFile.size() - 4) + ".notext.png", noText))
                    << noTextFile << " without text";
                EXPECT_EQ(noText.width, sidecar.width);
            }
            ASSERT_TRUE(boxes.count(suffix)) << "capture '" << suffix << "' has no boxes";
            for (const LayoutBox& box : boxes[suffix])
                EXPECT_NE(html.find("id=\"" + box.id + "\""), std::string::npos) << box.id;
        }
        EXPECT_EQ(boxes.size(), sidecar.captures().size());
    }
    const FixtureList& known = browserKnownFailures();
    for (const std::string& error : known.errors)
        ADD_FAILURE() << error;
    for (const ListedFixture& entry : known.entries){
        EXPECT_TRUE(std::any_of(fixtures.begin(), fixtures.end(), [&](const BrowserFixture& f){ return f.id() == entry.id; }))
            << "known-failures.txt:" << entry.line << ": no fixture " << entry.id;
        EXPECT_TRUE(isPhase3Package(entry.owner)) << "known-failures.txt:" << entry.line << ": owner '" << entry.owner << "'";
    }
}

// ---- the fixtures ----------------------------------------------------------------------------------

class BrowserParity : public ::testing::TestWithParam<BrowserFixture> {};

TEST_P(BrowserParity, MatchesChromium){
    const BrowserFixture fixture = GetParam();
    const std::string artifacts = browserArtifactDirectory() + "/" + fixture.area;
    FixtureSidecar sidecar;
    std::string error;
    ASSERT_TRUE(loadSidecar(browserFixturesDirectory() + "/" + fixture.area + "/" + fixture.name + ".json", sidecar, &error)) << error;
    std::map<std::string, std::vector<LayoutBox>> referenceBoxes;
    ASSERT_TRUE(loadReferenceBoxes(browserReferenceDirectory() + "/" + fixture.area + "/" + fixture.name + ".boxes.json", referenceBoxes, &error))
        << error;

    FixtureRunner runner(sidecar.width, sidecar.height, sidecar.sampleCount);
    if (!runner.valid())
        GTEST_SKIP() << "no headless GPU context";

    // every difference, gated or reported only
    std::vector<std::string> failures;
    std::ostringstream metrics;
    metrics << fixture.id() << "\n";
    std::vector<FixtureRunner::Capture> captures;
    if (!runner.load(fixture.assetPath(), &error) || !runner.run(sidecar, captures, &error)){
        failures.push_back(error);
    }else{
        writeText(artifacts + "/" + fixture.name + ".diag.txt", runner.diagnosticsText());
        if (const std::shared_ptr<const hc::CompatDocument> compiled = runner.compiled()){
            std::string rcss;
            for (const hc::StylesheetOutput& sheet : compiled->stylesheets)
                rcss += "/* " + (sheet.virtualPath.empty() ? std::string("inline <style>") : sheet.virtualPath) + " */\n" + sheet.rcss + "\n";
            writeText(artifacts + "/" + fixture.name + ".rml", compiled->rml);
            writeText(artifacts + "/" + fixture.name + ".rcss", rcss);
        }
        for (const FixtureRunner::Capture& capture : captures){
            const std::string file = captureFile(fixture, capture.suffix);
            const std::string base = browserArtifactDirectory() + "/" + file.substr(0, file.size() - 4);
            savePng(base + ".png", capture.image);
            Image reference;
            if (!loadPng(browserReferenceDirectory() + "/" + file, reference)){
                failures.push_back(file + ": no reference image");
                continue;
            }
            const StrictMetrics image = compareStrict(capture.image, reference, sidecar.gate);
            savePng(base + ".diff.png", strictHeatMap(capture.image, reference, sidecar.gate));
            metrics << file << ": " << image.text() << (sidecar.boxes ? " (reported only: text-heavy)" : "") << "\n";
            if (!image.passes() && !sidecar.boxes)
                failures.push_back(file + " image: " + image.text() + "; heat map " + base + ".diff.png");
            if (sidecar.noText){
                // the image without text: gated at the sidecar's gate, also for a text-heavy fixture
                const std::string noTextFile = file.substr(0, file.size() - 4) + ".notext.png";
                Image noTextReference;
                savePng(base + ".notext.png", capture.noTextImage);
                if (!loadPng(browserReferenceDirectory() + "/" + noTextFile, noTextReference)){
                    failures.push_back(noTextFile + ": no reference image");
                }else{
                    const StrictMetrics noText = compareStrict(capture.noTextImage, noTextReference, sidecar.gate);
                    savePng(base + ".notext.diff.png", strictHeatMap(capture.noTextImage, noTextReference, sidecar.gate));
                    metrics << noTextFile << ": " << noText.text() << "\n";
                    if (!noText.passes())
                        failures.push_back(noTextFile + " image: " + noText.text() + "; heat map " + base + ".notext.diff.png");
                }
            }
            const std::vector<std::string> boxes = compareBoxes(capture.boxes, referenceBoxes[capture.suffix], sidecar.boxTolerance);
            metrics << file << " boxes: " << boxes.size() << " differences\n";
            for (const std::string& difference : boxes){
                metrics << "  " << difference << "\n";
                failures.push_back(file + " boxes: " + difference);
            }
        }
    }
    writeText(artifacts + "/" + fixture.name + ".metrics.txt", metrics.str());
    std::cout << metrics.str();

    std::string list;
    for (const std::string& failure : failures)
        list += "\n  " + failure;

    // the harness's own fixtures: the comparison must say "different" for an injected difference
    if (sidecar.expectMismatch){
        EXPECT_FALSE(failures.empty()) << fixture.id() << ": the reference differs on purpose (" << sidecar.referenceCss
                                       << ") and the comparison did not report it";
        if (!failures.empty())
            std::cout << "[ REPORTED ] " << fixture.id() << " (expected):" << list << "\n";
        return;
    }

    const ListedFixture* known = browserKnownFailures().find(fixture.id());
    if (failures.empty()){
        if (known)
            ADD_FAILURE() << fixture.id() << " passes now: remove its line from fixtures/p3/known-failures.txt (owner " << known->owner << ", reason "
                          << known->reason << ")";
        return;
    }
    if (known){
        std::cout << "[ KNOWN    ] " << fixture.id() << " (owner " << known->owner << ", " << known->reason << "):" << list << "\n";
        return;
    }
    ADD_FAILURE() << fixture.id() << " differs from Chromium:" << list;
}

INSTANTIATE_TEST_SUITE_P(P3, BrowserParity, ::testing::ValuesIn(listBrowserFixtures()),
                         [](const ::testing::TestParamInfo<BrowserFixture>& info){
                             std::string name;
                             for (char c : info.param.id())
                                 name += std::isalnum((unsigned char)c) ? c : '_';
                             return name;
                         });
