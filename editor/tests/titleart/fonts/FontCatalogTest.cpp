// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/S82-title-designer.md R54, R55, R57, R60 (AC24 model half, AC37 Google offline): the
// font picker's model with a fake project, a fake system list and a fake Google Fonts (HTTP
// answered in process, no network): sections and .ttc faces, search, Covers My Text, the
// missing count, categories, variable axes and the axes rule of a font change, the 7-day
// metadata cache, the offline message and at most 2 preview downloads at once.

#include <gtest/gtest.h>

#include "titleart/fonts/FontTestSupport.h"

#include "titleart/fonts/FontCatalog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

using namespace jasperengine::editor::titlefonts;
using namespace jasperengine::editor::titlefonts::test;
using TJson = jasperengine::titleart::Json;

namespace {

    constexpr int64_t NOW = 2000000000;

    const char* METADATA = ")]}'\n"
                           "{\"familyMetadataList\":["
                           "{\"family\":\"Dela Gothic One\",\"category\":\"Display\",\"subsets\":[\"menu\",\"japanese\",\"latin\",\"latin-ext\"],"
                           "\"fonts\":{\"400\":{}},\"axes\":[]},"
                           "{\"family\":\"Oswald\",\"category\":\"Sans Serif\",\"subsets\":[\"menu\",\"cyrillic\",\"latin\",\"latin-ext\",\"vietnamese\"],"
                           "\"fonts\":{\"200\":{},\"400\":{},\"700\":{}},\"axes\":[{\"tag\":\"wght\",\"min\":200,\"max\":700,\"defaultValue\":400}]},"
                           "{\"family\":\"Press Start 2P\",\"category\":\"Display\",\"subsets\":[\"latin\"],\"fonts\":{\"400\":{}},\"axes\":[]}"
                           "]}";

    // fonts.google.com and fonts.googleapis.com / fonts.gstatic.com, answered in process
    struct FakeGoogle {
        std::atomic<bool> offline{false};
        std::atomic<int> metadataCalls{0};
        std::atomic<int> fontCalls{0};
        std::atomic<int> running{0};
        std::atomic<int> maxRunning{0};
        std::string font;   // every family's Regular is Oswald here

        FontCatalog::HttpResult get(const std::string& url) {
            FontCatalog::HttpResult result;
            if (offline) {
                result.error = "Could not resolve host";
                return result;
            }
            if (url.find("fonts.google.com/metadata/fonts") != std::string::npos) {
                metadataCalls++;
                result.ok = true;
                result.body = METADATA;
                return result;
            }
            if (url.find("fonts.googleapis.com/css2") != std::string::npos) {
                const size_t start = url.find("family=") + 7;
                std::string family = url.substr(start, url.find(':', start) - start);
                std::replace(family.begin(), family.end(), '+', ' ');
                std::string file = family;
                file.erase(std::remove(file.begin(), file.end(), ' '), file.end());
                result.ok = true;
                result.body = "@font-face {\n  font-family: '" + family + "';\n  font-style: normal;\n  font-weight: 400;\n  src: url(https://fonts.gstatic.com/s/" +
                              file + "/v1/" + file + "-400.ttf) format('truetype');\n}\n";
                return result;
            }
            if (url.find("fonts.gstatic.com/") != std::string::npos) {
                fontCalls++;
                const int now = ++running;
                int seen = maxRunning.load();
                while (now > seen && !maxRunning.compare_exchange_weak(seen, now)) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                running--;
                result.ok = true;
                result.body = font;
                return result;
            }
            result.error = "HTTP 404";
            return result;
        }
    };

    struct LibraryGuard {
        explicit LibraryGuard(LibraryFontProvider provider) { setLibraryFontProvider(std::move(provider)); }
        ~LibraryGuard() { setLibraryFontProvider(nullptr); }
    };

    // a project with Oswald.ttf, Pair.ttc (Dela Gothic One + Oswald) and a .woff2; the system has
    // Source Sans 3 (CFF), Stick and Dela Gothic One
    class FontCatalogTest : public ::testing::Test {
    protected:
        ScratchDir scratch{"catalog"};
        std::shared_ptr<FakeGoogle> google = std::make_shared<FakeGoogle>();

        void SetUp() override {
            const std::vector<uint8_t> oswald = readBytes(oswaldPath());
            google->font.assign(oswald.begin(), oswald.end());
            fs::create_directories(assets() / "Fonts");
            fs::copy_file(oswaldPath(), assets() / "Fonts" / "Oswald.ttf");
            writeBytes(assets() / "Fonts" / "Pair.ttc", buildCollection({readBytes(delaGothicPath()), readBytes(oswaldPath())}));
            fs::create_directories(assets() / "Web");
            fs::copy_file(webFontFixturesDir() / "Oswald-wght.woff2", assets() / "Web" / "Brand.woff2");
        }

        fs::path assets() const { return scratch.path() / "assets"; }
        fs::path cache() const { return scratch.path() / "cache"; }

        FontCatalog::Services services() {
            FontCatalog::Services s;
            const fs::path root = assets();
            s.projectFonts = [root]() {
                return std::vector<FontCatalog::ProjectFontFile>{{(root / "Fonts" / "Oswald.ttf").string(), "Fonts/Oswald.ttf", "0123456789abcdef0123456789abcdef"},
                                                                 {(root / "Fonts" / "Pair.ttc").string(), "Fonts/Pair.ttc", "fedcba9876543210fedcba9876543210"}};
            };
            s.projectWebFonts = [root]() {
                return std::vector<FontCatalog::ProjectFontFile>{{(root / "Web" / "Brand.woff2").string(), "Web/Brand.woff2", ""}};
            };
            s.systemFonts = []() {
                std::vector<SystemFontFace> faces(3);
                faces[0] = {sourceSansPath().string(), 0, "Source Sans 3", "Regular", SystemFontFormat::Cff};
                faces[1] = {stickPath().string(), 0, "Stick", "Regular", SystemFontFormat::TrueType};
                faces[2] = {delaGothicPath().string(), 0, "Dela Gothic One", "Regular", SystemFontFormat::TrueType};
                return faces;
            };
            std::shared_ptr<FakeGoogle> fake = google;
            s.http = [fake](const std::string& url) { return fake->get(url); };
            s.cacheDir = cache();
            s.now = []() { return NOW; };
            return s;
        }

        static std::vector<const FontEntry*> settled(FontCatalog& catalog, const FontCatalog::Query& query) {
            catalog.filter(query);   // starts the reads the query needs
            catalog.waitIdle();
            return catalog.filter(query);
        }

        static std::vector<std::string> keys(const std::vector<const FontEntry*>& entries) {
            std::vector<std::string> out;
            for (const FontEntry* entry : entries) out.push_back(entry->key);
            return out;
        }

        static bool hasKey(const std::vector<const FontEntry*>& entries, const std::string& key) {
            return std::any_of(entries.begin(), entries.end(), [&key](const FontEntry* e) { return e->key == key; });
        }
    };

}

// Red control: listing a .ttc as one entry (face 0 only) or dropping the web font fails the
// Project half; a Library provider never asked fails the Library row
TEST_F(FontCatalogTest, SectionsAndCollectionFaces) {
    LibraryGuard library([]() {
        LibraryFont font;
        font.id = "oswald";
        font.family = "Oswald";
        font.style = "Regular";
        font.path = oswaldPath().string();
        return std::vector<LibraryFont>{font};
    });
    FontCatalog catalog(services());
    catalog.open();
    catalog.waitIdle();
    ASSERT_TRUE(catalog.projectReady());
    ASSERT_TRUE(catalog.systemReady());
    ASSERT_EQ(catalog.googleState(), FontCatalog::GoogleState::Ready);
    EXPECT_EQ(catalog.googleMessage(), "");

    std::map<FontSection, int> counts;
    FontSection previous = FontSection::Project;
    for (const FontEntry& entry : catalog.entries()) {
        counts[entry.section]++;
        EXPECT_GE((int)entry.section, (int)previous) << "sections in order: " << entry.key;
        previous = entry.section;
    }
    EXPECT_EQ(counts[FontSection::Project], 4);
    EXPECT_EQ(counts[FontSection::Library], 1);
    EXPECT_EQ(counts[FontSection::Google], 3);
    EXPECT_EQ(counts[FontSection::System], 3);

    const FontEntry* dela = catalog.find("project:Fonts/Pair.ttc#0");
    const FontEntry* pairOswald = catalog.find("project:Fonts/Pair.ttc#1");
    ASSERT_NE(dela, nullptr);
    ASSERT_NE(pairOswald, nullptr);
    EXPECT_EQ(dela->family, "Dela Gothic One");
    EXPECT_EQ(pairOswald->family, "Oswald");
    EXPECT_EQ(pairOswald->faceIndex, 1);
    EXPECT_TRUE(dela->collectionFace);
    EXPECT_EQ(dela->guid, "fedcba9876543210fedcba9876543210");
    const FontEntry* single = catalog.find("project:Fonts/Oswald.ttf#0");
    ASSERT_NE(single, nullptr);
    EXPECT_FALSE(single->collectionFace);
    const FontEntry* web = catalog.find("project-web:Web/Brand.woff2");
    ASSERT_NE(web, nullptr);
    EXPECT_TRUE(web->webFont) << "listed with the Convert badge";
    EXPECT_EQ(web->family, "Oswald") << "named from the decoded font";
    ASSERT_NE(catalog.find("library:oswald"), nullptr);
    const FontEntry* googleDela = catalog.find("google:Dela Gothic One");
    ASSERT_NE(googleDela, nullptr);
    EXPECT_EQ(googleDela->category, FontCategory::Display);
    EXPECT_TRUE(googleDela->path.empty()) << "not downloaded yet";
    const FontEntry* googleOswald = catalog.find("google:Oswald");
    ASSERT_NE(googleOswald, nullptr);
    EXPECT_TRUE(googleOswald->googleVariable);
    EXPECT_EQ(googleOswald->googleWeights, (std::vector<int>{200, 400, 700}));
}

TEST_F(FontCatalogTest, SearchesFamilyStyleAndFileNames) {
    FontCatalog catalog(services());
    catalog.open();
    catalog.waitIdle();
    FontCatalog::Query query;
    query.search = "OSWALD";
    const std::vector<std::string> oswald = keys(catalog.filter(query));
    // (three Oswald Regular faces in Project: in key order)
    EXPECT_EQ(oswald, (std::vector<std::string>{"project-web:Web/Brand.woff2", "project:Fonts/Oswald.ttf#0", "project:Fonts/Pair.ttc#1", "google:Oswald"}));
    query.search = "sans 3";
    EXPECT_EQ(keys(catalog.filter(query)), (std::vector<std::string>{"system:" + sourceSansPath().string() + "#0"}));
    query.search = "stick-regular";
    EXPECT_EQ(keys(catalog.filter(query)), (std::vector<std::string>{"system:" + stickPath().string() + "#0"}));
    query.search = "pair.ttc";
    EXPECT_EQ(catalog.filter(query).size(), 2u) << "both faces by their file name";
}

// AC24's model half. Red control: ignoring the fallback fonts in missingCodepoints keeps 2 for
// Oswald + Dela Gothic One; counting occurrences instead of code points gives 3 for "A激激"
TEST_F(FontCatalogTest, CoversMyTextAndTheMissingCount) {
    FontCatalog catalog(services());
    catalog.open();
    catalog.waitIdle();
    const std::vector<uint32_t> text = utf8CodePoints("\xE6\xBF\x80\xE7\x86\xB1");   // 激熱
    EXPECT_TRUE(coversMyTextByDefault("\xE6\xBF\x80\xE7\x86\xB1"));
    EXPECT_FALSE(coversMyTextByDefault("Title 0123"));

    FontCatalog::Query query;
    query.coversText = true;
    query.text = text;
    const std::vector<const FontEntry*> kept = settled(catalog, query);
    EXPECT_TRUE(hasKey(kept, "project:Fonts/Pair.ttc#0")) << "Dela Gothic One covers 激熱";
    EXPECT_TRUE(hasKey(kept, "system:" + delaGothicPath().string() + "#0"));
    EXPECT_TRUE(hasKey(kept, "system:" + stickPath().string() + "#0"));
    EXPECT_TRUE(hasKey(kept, "google:Dela Gothic One")) << "its japanese subset, before any download";
    EXPECT_FALSE(hasKey(kept, "project:Fonts/Oswald.ttf#0"));
    EXPECT_FALSE(hasKey(kept, "project:Fonts/Pair.ttc#1"));
    EXPECT_FALSE(hasKey(kept, "system:" + sourceSansPath().string() + "#0"));
    EXPECT_FALSE(hasKey(kept, "google:Oswald"));
    EXPECT_FALSE(hasKey(kept, "google:Press Start 2P"));
    EXPECT_TRUE(hasKey(kept, "project-web:Web/Brand.woff2")) << "a web font is listed to be converted, its coverage unknown";

    const FontEntry* oswald = catalog.find("project:Fonts/Oswald.ttf#0");
    ASSERT_NE(oswald, nullptr);
    std::vector<uint32_t> missing;
    ASSERT_TRUE(catalog.missingCodepoints(*oswald, text, {}, missing));
    EXPECT_EQ(missing, (std::vector<uint32_t>{0x6FC0, 0x71B1})) << "the badge says 2 missing";
    const std::vector<uint32_t> repeated = utf8CodePoints("A\xE6\xBF\x80\xE6\xBF\x80 ");
    ASSERT_TRUE(catalog.missingCodepoints(*oswald, repeated, {}, missing));
    EXPECT_EQ(missing.size(), 1u);

    // with Dela Gothic One as the layer's Fallback Font nothing is missing
    const std::vector<FontFileRef> fallbacks = {{delaGothicPath().string(), 0}};
    if (!catalog.missingCodepoints(*oswald, text, fallbacks, missing)) {
        catalog.waitIdle();
        ASSERT_TRUE(catalog.missingCodepoints(*oswald, text, fallbacks, missing));
    }
    EXPECT_TRUE(missing.empty());
    query.fallbacks = fallbacks;
    EXPECT_TRUE(hasKey(settled(catalog, query), "project:Fonts/Oswald.ttf#0"));

    // Add Fallback Font: the project fonts that have both, first
    std::vector<const FontEntry*> candidates;
    if (!catalog.fallbackCandidates(text, candidates)) {
        catalog.waitIdle();
        ASSERT_TRUE(catalog.fallbackCandidates(text, candidates));
    }
    EXPECT_EQ(keys(candidates), (std::vector<std::string>{"project:Fonts/Pair.ttc#0"}));

    EXPECT_EQ(previewTextFor("\xE6\xBF\x80\xE7\x86\xB1\nsecond line"), "\xE6\xBF\x80\xE7\x86\xB1");
    EXPECT_EQ(previewTextFor(""), DEFAULT_PREVIEW_TEXT);
    EXPECT_EQ(utf8CodePoints(previewTextFor("abcdefghijklmnopqrstuvwxyz0123")).size(), 24u);
}

// Red control: reading the axis names as their tags fails "Weight"; taking instance coordinates
// from the wrong offset fails Bold's 700
TEST_F(FontCatalogTest, VariableAxesAndTheFontChangeRule) {
    FontDetails oswald;
    std::string error;
    ASSERT_TRUE(loadFontDetails(oswaldPath().string(), 0, oswald, error)) << error;
    ASSERT_EQ(oswald.axes.size(), 1u);
    EXPECT_EQ(oswald.axes[0].tag, "wght");
    EXPECT_EQ(oswald.axes[0].name, "Weight");
    EXPECT_FLOAT_EQ(oswald.axes[0].minValue, 200.0f);
    EXPECT_FLOAT_EQ(oswald.axes[0].defaultValue, 400.0f);
    EXPECT_FLOAT_EQ(oswald.axes[0].maxValue, 700.0f);
    ASSERT_EQ(oswald.instances.size(), 6u);
    EXPECT_EQ(oswald.instances.front().name, "ExtraLight");
    EXPECT_EQ(oswald.instances.back().name, "Bold");
    ASSERT_EQ(oswald.instances.back().coordinates.size(), 1u);
    EXPECT_FLOAT_EQ(oswald.instances.back().coordinates[0], 700.0f);
    EXPECT_EQ(oswald.outlines, OutlineKind::TrueType);
    EXPECT_EQ(oswald.names.family, "Oswald");

    FontDetails cff2;
    ASSERT_TRUE(loadFontDetails(sourceSansVfPath().string(), 0, cff2, error)) << error;
    EXPECT_EQ(cff2.outlines, OutlineKind::Cff2);
    ASSERT_EQ(cff2.axes.size(), 1u);
    EXPECT_FLOAT_EQ(cff2.axes[0].maxValue, 900.0f);

    FontDetails dela;
    ASSERT_TRUE(loadFontDetails(delaGothicPath().string(), 0, dela, error)) << error;
    EXPECT_TRUE(dela.axes.empty());
    EXPECT_TRUE(dela.cmap.covers(0x6FC0));
    EXPECT_FALSE(loadFontDetails(delaGothicPath().string(), 3, dela, error)) << "no face 3";

    // a .ttc's second face through the catalog
    FontCatalog catalog(services());
    catalog.open();
    catalog.waitIdle();
    const FontEntry* pairOswald = catalog.find("project:Fonts/Pair.ttc#1");
    ASSERT_NE(pairOswald, nullptr);
    catalog.details(*pairOswald);
    catalog.waitIdle();
    const FontDetails* details = catalog.details(*pairOswald);
    ASSERT_NE(details, nullptr);
    EXPECT_EQ(details->axes.size(), 1u);

    // R60: keep the axes the new font has (clamped to its range), drop the others
    const TJson variations = {{"wght", 900}, {"wdth", 80}};
    AxesAfterFontChange toOswald = keepAxesOfNewFont(variations, oswald.axes);
    EXPECT_TRUE(toOswald.reset());
    EXPECT_EQ(toOswald.dropped, (std::vector<std::string>{"wdth"}));
    ASSERT_TRUE(toOswald.variations.contains("wght"));
    EXPECT_DOUBLE_EQ(toOswald.variations["wght"].get<double>(), 700.0);
    const AxesAfterFontChange toDela = keepAxesOfNewFont(variations, dela.axes);
    EXPECT_EQ(toDela.dropped.size(), 2u);
    EXPECT_TRUE(toDela.variations.empty());
    const AxesAfterFontChange kept = keepAxesOfNewFont(TJson{{"wght", 500}}, oswald.axes);
    EXPECT_FALSE(kept.reset());
    EXPECT_EQ(changeFontUndoName("Dela Gothic One", toDela.reset()), "Change Font to Dela Gothic One (Axes Reset)");
    EXPECT_EQ(changeFontUndoName("Oswald", kept.reset()), "Change Font to Oswald");
}

TEST_F(FontCatalogTest, Categories) {
    Os2Info os2;
    EXPECT_EQ(categoryOfFont(os2), FontCategory::Any);
    os2.panose[0] = 2;
    os2.panose[1] = 11;
    EXPECT_EQ(categoryOfFont(os2), FontCategory::Sans);
    os2.panose[1] = 2;
    EXPECT_EQ(categoryOfFont(os2), FontCategory::Serif);
    os2.panose[3] = 9;
    EXPECT_EQ(categoryOfFont(os2), FontCategory::Monospace);
    os2 = Os2Info();
    os2.familyClass = 10 << 8;
    EXPECT_EQ(categoryOfFont(os2), FontCategory::Handwriting);
    os2.familyClass = 9 << 8;
    EXPECT_EQ(categoryOfFont(os2), FontCategory::Display);
    os2.familyClass = 8 << 8 | 1;
    EXPECT_EQ(categoryOfFont(os2), FontCategory::Sans);
    EXPECT_EQ(categoryOfGoogle("Sans Serif"), FontCategory::Sans);
    EXPECT_EQ(categoryOfGoogle("Handwriting"), FontCategory::Handwriting);

    FontCatalog catalog(services());
    catalog.open();
    catalog.waitIdle();
    FontCatalog::Query query;
    query.category = FontCategory::Display;
    std::vector<const FontEntry*> display = settled(catalog, query);
    EXPECT_TRUE(hasKey(display, "google:Dela Gothic One"));
    EXPECT_TRUE(hasKey(display, "google:Press Start 2P"));
    EXPECT_FALSE(hasKey(display, "google:Oswald"));
    EXPECT_FALSE(hasKey(display, "system:" + sourceSansPath().string() + "#0"));
    query.category = FontCategory::Sans;
    std::vector<const FontEntry*> sans = settled(catalog, query);
    EXPECT_TRUE(hasKey(sans, "google:Oswald"));
    EXPECT_TRUE(hasKey(sans, "system:" + sourceSansPath().string() + "#0")) << "Source Sans 3's PANOSE says sans";
}

// AC37's model half. Red control: an Offline state that also clears the other sections fails the
// counts; a message other than R57's fails the text
TEST_F(FontCatalogTest, Offline) {
    google->offline = true;
    FontCatalog catalog(services());
    catalog.open();
    catalog.waitIdle();
    EXPECT_EQ(catalog.googleState(), FontCatalog::GoogleState::Offline);
    EXPECT_EQ(catalog.googleMessage(), "Google Fonts needs a connection");
    int project = 0, google = 0, system = 0;
    for (const FontEntry& entry : catalog.entries()) {
        project += entry.section == FontSection::Project;
        google += entry.section == FontSection::Google;
        system += entry.section == FontSection::System;
    }
    EXPECT_EQ(project, 4);
    EXPECT_EQ(google, 0);
    EXPECT_EQ(system, 3);
}

// Red control: an age check on the wrong side (or none) fetches with a fresh cache, or never
// refetches a stale one; a session cache keyed wrongly fetches again for the second catalog
TEST_F(FontCatalogTest, MetadataIsCachedSevenDaysAndOncePerSession) {
    const fs::path folder = cache() / "google-fonts";
    fs::create_directories(folder);
    std::ofstream(folder / "metadata.json", std::ios::binary) << METADATA;
    std::ofstream(folder / "metadata.time", std::ios::binary) << (NOW - 24 * 3600);
    {
        FontCatalog catalog(services());
        catalog.open();
        catalog.waitIdle();
        EXPECT_EQ(catalog.googleState(), FontCatalog::GoogleState::Ready);
        EXPECT_EQ(google->metadataCalls.load(), 0) << "a day-old list is used as it is";
        EXPECT_NE(catalog.find("google:Oswald"), nullptr);
    }
    {
        FontCatalog again(services());
        again.open();
        again.waitIdle();
        EXPECT_EQ(google->metadataCalls.load(), 0) << "once per session";
        EXPECT_NE(again.find("google:Oswald"), nullptr);
    }

    ScratchDir stale("catalog-stale");
    const fs::path staleFolder = stale.path() / "google-fonts";
    fs::create_directories(staleFolder);
    std::ofstream(staleFolder / "metadata.json", std::ios::binary) << METADATA;
    std::ofstream(staleFolder / "metadata.time", std::ios::binary) << (NOW - 8 * 24 * 3600);
    FontCatalog::Services s = services();
    s.cacheDir = stale.path();
    FontCatalog catalog(s);
    catalog.open();
    catalog.waitIdle();
    EXPECT_EQ(google->metadataCalls.load(), 1) << "an 8-day-old list is fetched again";
    EXPECT_EQ(catalog.googleState(), FontCatalog::GoogleState::Ready);
}

// Red control: starting every queued download at once records 3 running together
TEST_F(FontCatalogTest, GoogleDownloadsAtMostTwoAtOnce) {
    FontCatalog catalog(services());
    catalog.open();
    catalog.waitIdle();
    for (const char* key : {"google:Dela Gothic One", "google:Oswald", "google:Press Start 2P"}) catalog.requestGoogleDownload(key);
    EXPECT_EQ(catalog.googleDownloadState("google:Press Start 2P"), FontCatalog::DownloadState::Queued);
    catalog.waitIdle();
    EXPECT_EQ(google->fontCalls.load(), 3);
    EXPECT_LE(google->maxRunning.load(), FontCatalog::MAX_GOOGLE_DOWNLOADS);
    for (const char* family : {"Dela Gothic One", "Oswald", "Press Start 2P"}) {
        const std::string key = std::string("google:") + family;
        EXPECT_EQ(catalog.googleDownloadState(key), FontCatalog::DownloadState::Ready) << catalog.googleDownloadError(key);
        EXPECT_TRUE(fs::is_regular_file(catalog.googleCachePath(family))) << family;
        const FontEntry* entry = catalog.find(key);
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->path, catalog.googleCachePath(family).string()) << "previews read the cached Regular";
    }
    EXPECT_EQ(catalog.googleCachePath("Press Start 2P"), cache() / "google-fonts" / "PressStart2P-400.ttf");

    // once downloaded, coverage comes from the file (every fake Regular is Oswald: no 激)
    const FontEntry* dela = catalog.find("google:Dela Gothic One");
    ASSERT_NE(dela, nullptr);
    std::vector<uint32_t> missing;
    catalog.missingCodepoints(*dela, utf8CodePoints("\xE6\xBF\x80"), {}, missing);
    catalog.waitIdle();
    dela = catalog.find("google:Dela Gothic One");
    ASSERT_TRUE(catalog.missingCodepoints(*dela, utf8CodePoints("\xE6\xBF\x80"), {}, missing));
    EXPECT_EQ(missing.size(), 1u);

    // a second request of a cached family downloads nothing
    catalog.requestGoogleDownload("google:Oswald");
    catalog.waitIdle();
    EXPECT_EQ(google->fontCalls.load(), 3);
}
