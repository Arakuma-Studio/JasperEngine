// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// S81 R50, R54, R56, R57, AC15: the export's font subsetter on the OFL fixture fonts
// (engine/tests/core/titleart/fonts): a Latin subset keeps its code points and the vert closure
// and shapes like the full font, variable axes survive, the fsType rules (Restricted needs the
// acknowledgement, No Subsetting ships whole, Bitmap Only fails), an OFL font with a Reserved Font
// Name is renamed in every platform record, the cache, the timing budget and the credits text.

#include <gtest/gtest.h>

#include "support/TestTempDir.h"

#include "titleart/runtime/TitleCharacters.h"
#include "titleart/runtime/TitleFontSubsetter.h"

#include "hb.h"
#include "hb-ot.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using jasperengine::test::TestTempDir;

namespace {

    std::vector<uint8_t> fixture(const std::string& name) {
        const fs::path path = fs::path(JASPER_TEST_REPO_DIR) / "engine" / "tests" / "core" / "titleart" / "fonts" / name;
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        EXPECT_FALSE(bytes.empty()) << path;
        return bytes;
    }

    uint16_t u16(const std::vector<uint8_t>& b, size_t at) { return static_cast<uint16_t>((b[at] << 8) | b[at + 1]); }
    uint32_t u32(const std::vector<uint8_t>& b, size_t at) { return (uint32_t(u16(b, at)) << 16) | u16(b, at + 2); }

    // the offset of a table in a single-face sfnt, 0 when absent
    size_t tableOffset(const std::vector<uint8_t>& font, const char* tag) {
        const uint16_t count = u16(font, 4);
        for (size_t i = 0; i < count; i++) {
            const size_t record = 12 + 16 * i;
            if (std::equal(tag, tag + 4, font.begin() + (long)record)) return u32(font, record + 8);
        }
        return 0;
    }

    // a copy of the font with another OS/2 fsType (the table checksum goes stale: fonts are read
    // without checking it)
    std::vector<uint8_t> withFsType(std::vector<uint8_t> font, uint16_t fsType) {
        const size_t os2 = tableOffset(font, "OS/2");
        EXPECT_NE(os2, 0u);
        font[os2 + 8] = static_cast<uint8_t>(fsType >> 8);
        font[os2 + 9] = static_cast<uint8_t>(fsType & 0xff);
        return font;
    }

    struct NameRecord {
        uint16_t platform, encoding, language, id;
        std::string text;       // UTF-16BE records as ASCII where they are, else raw
    };

    std::vector<NameRecord> nameRecords(const std::vector<uint8_t>& font) {
        std::vector<NameRecord> out;
        const size_t name = tableOffset(font, "name");
        if (!name) return out;
        const uint16_t count = u16(font, name + 2), storage = u16(font, name + 4);
        for (size_t i = 0; i < count; i++) {
            const size_t r = name + 6 + 12 * i;
            NameRecord record{u16(font, r), u16(font, r + 2), u16(font, r + 4), u16(font, r + 6), {}};
            const size_t start = name + storage + u16(font, r + 10), length = u16(font, r + 8);
            const bool wide = record.platform == 0 || record.platform == 3;
            for (size_t k = 0; k < length; k += wide ? 2 : 1) {
                const uint32_t c = wide ? u16(font, start + k) : font[start + k];
                record.text += c < 0x80 ? static_cast<char>(c) : '?';
            }
            out.push_back(record);
        }
        return out;
    }

    // a copy with the same-length `from` replaced by `to` in every name string, UTF-16BE and Mac Roman
    std::vector<uint8_t> withNameText(std::vector<uint8_t> font, const std::string& from, const std::string& to) {
        EXPECT_EQ(from.size(), to.size());
        int replaced = 0;
        auto replaceAll = [&](const std::string& f, const std::string& t) {
            for (auto it = std::search(font.begin(), font.end(), f.begin(), f.end()); it != font.end();
                 it = std::search(it + (long)f.size(), font.end(), f.begin(), f.end())) {
                std::copy(t.begin(), t.end(), it);
                replaced++;
            }
        };
        auto wide = [](const std::string& s) {
            std::string out;
            for (char c : s) { out.push_back('\0'); out.push_back(c); }
            return out;
        };
        replaceAll(wide(from), wide(to));
        replaceAll(from, to);
        EXPECT_GT(replaced, 0) << from;
        return font;
    }

    struct Font {
        hb_blob_t* blob = nullptr;
        hb_face_t* face = nullptr;
        hb_font_t* font = nullptr;
        explicit Font(const std::vector<uint8_t>& bytes) {
            blob = hb_blob_create(reinterpret_cast<const char*>(bytes.data()), (unsigned)bytes.size(), HB_MEMORY_MODE_DUPLICATE, nullptr, nullptr);
            face = hb_face_create(blob, 0);
            font = hb_font_create(face);
        }
        ~Font() {
            hb_font_destroy(font);
            hb_face_destroy(face);
            hb_blob_destroy(blob);
        }
    };

    struct Shaped {
        hb_codepoint_t glyph;
        uint32_t cluster;
        hb_position_t xAdvance, yAdvance, xOffset, yOffset;
        hb_glyph_extents_t extents;
    };

    std::vector<Shaped> shape(Font& f, const std::string& text, hb_direction_t direction, const char* variations = nullptr) {
        if (variations) {
            hb_variation_t v;
            EXPECT_TRUE(hb_variation_from_string(variations, -1, &v));
            hb_font_set_variations(f.font, &v, 1);
        }
        hb_buffer_t* buffer = hb_buffer_create();
        hb_buffer_add_utf8(buffer, text.c_str(), (int)text.size(), 0, -1);
        hb_buffer_guess_segment_properties(buffer);
        hb_buffer_set_direction(buffer, direction);
        hb_buffer_set_language(buffer, hb_language_from_string("ja", -1));
        hb_shape(f.font, buffer, nullptr, 0);
        unsigned count = 0;
        const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &count);
        const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buffer, &count);
        std::vector<Shaped> out;
        for (unsigned i = 0; i < count; i++) {
            Shaped s{info[i].codepoint, info[i].cluster, pos[i].x_advance, pos[i].y_advance, pos[i].x_offset, pos[i].y_offset, {}};
            hb_font_get_glyph_extents(f.font, s.glyph, &s.extents);
            out.push_back(s);
        }
        hb_buffer_destroy(buffer);
        return out;
    }

    // the same glyphs in the same places: glyph ids differ between the fonts, so each glyph is
    // compared by its outline's extents, its advance and its offset
    void expectSameShaping(const std::vector<Shaped>& full, const std::vector<Shaped>& subset, const std::string& what) {
        ASSERT_EQ(full.size(), subset.size()) << what;
        for (size_t i = 0; i < full.size(); i++) {
            EXPECT_NE(subset[i].glyph, 0u) << what << ": glyph " << i << " is .notdef in the subset";
            EXPECT_EQ(full[i].cluster, subset[i].cluster) << what << " " << i;
            EXPECT_EQ(full[i].xAdvance, subset[i].xAdvance) << what << " " << i;
            EXPECT_EQ(full[i].yAdvance, subset[i].yAdvance) << what << " " << i;
            EXPECT_EQ(full[i].xOffset, subset[i].xOffset) << what << " " << i;
            EXPECT_EQ(full[i].yOffset, subset[i].yOffset) << what << " " << i;
            EXPECT_EQ(full[i].extents.x_bearing, subset[i].extents.x_bearing) << what << " " << i;
            EXPECT_EQ(full[i].extents.y_bearing, subset[i].extents.y_bearing) << what << " " << i;
            EXPECT_EQ(full[i].extents.width, subset[i].extents.width) << what << " " << i;
            EXPECT_EQ(full[i].extents.height, subset[i].extents.height) << what << " " << i;
        }
    }

    std::vector<uint32_t> codepointsOf(const TitleCharacterSet& set) { return set.list(); }

    TitleCharacterSet latinPlus(const std::string& extra) {
        TitleCharacterSet set;
        EXPECT_TRUE(titleCharacterPreset("latin", set));
        set.addUtf8(extra);
        return set;
    }

    TitleSubsetResult subsetOf(const std::vector<uint8_t>& bytes, const TitleCharacterSet& set, bool rename = true) {
        TitleFontSubsetter subsetter{fs::path()};
        TitleSubsetRequest request;
        request.bytes = bytes;
        request.codepoints = codepointsOf(set);
        request.renameReserved = rename;
        return subsetter.subset(request);
    }

    std::set<uint32_t> unicodesOf(Font& f) {
        hb_set_t* set = hb_set_create();
        hb_face_collect_unicodes(f.face, set);
        std::set<uint32_t> out;
        for (hb_codepoint_t cp = HB_SET_VALUE_INVALID; hb_set_next(set, &cp);) out.insert(cp);
        hb_set_destroy(set);
        return out;
    }

}

// AC15: a Latin subset of a CJK OFL font keeps the 95 code points and the vert closure (the
// vertical forms of what it keeps), and shapes vertical text as the full font does
TEST(TitleFontSubset, LatinKeepsCodepointsAndVertClosure) {
    const std::vector<uint8_t> bytes = fixture("Stick-Regular.ttf");
    const std::string vertical = "\xe3\x83\xbc\xe3\x80\x8c\xe3\x80\x8d\xef\xbc\x88\xef\xbc\x89";   // ー「」（）
    const TitleCharacterSet set = latinPlus(vertical);
    const TitleSubsetResult result = subsetOf(bytes, set);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.whole);
    EXPECT_FALSE(result.renamed) << "Stick declares no Reserved Font Name";
    EXPECT_EQ(result.family, "Stick");
    EXPECT_LT(result.bytes.size(), bytes.size() / 10) << "a 2.2 MB CJK font, 100 characters kept";

    Font full(bytes), subset(result.bytes);
    const std::set<uint32_t> kept = unicodesOf(subset);
    size_t latin = 0;
    for (uint32_t cp = 0x20; cp <= 0x7e; cp++) latin += kept.count(cp);
    EXPECT_EQ(latin, 95u);
    EXPECT_EQ(kept.size(), set.size()) << "only the set's code points";

    const std::vector<Shaped> fullVertical = shape(full, vertical, HB_DIRECTION_TTB);
    const std::vector<Shaped> subsetVertical = shape(subset, vertical, HB_DIRECTION_TTB);
    expectSameShaping(fullVertical, subsetVertical, "vertical");
    const std::vector<Shaped> subsetHorizontal = shape(subset, vertical, HB_DIRECTION_LTR);
    ASSERT_EQ(subsetHorizontal.size(), subsetVertical.size());
    EXPECT_NE(subsetHorizontal[0].glyph, subsetVertical[0].glyph) << "the subset has ー's vert form";
}

// AC15: shaping a subset gives the full font's glyphs and positions (ligatures and kerning included)
// the library's pack fonts declare a Reserved Font Name in three different ways; all must rename
TEST(TitleFontSubset, ReservedNamesOfThePackFonts) {
    const fs::path fonts = fs::path(JASPER_TEST_REPO_DIR) / "editor" / "resources" / "titleart" / "fonts";
    for (const fs::path& file : {fonts / "monoton" / "Monoton-Regular.ttf", fonts / "audiowide" / "Audiowide-Regular.ttf", fonts / "medievalsharp" / "MedievalSharp.ttf",
                                 fonts / "russo-one" / "RussoOne-Regular.ttf"}) {
        std::ifstream in(file, std::ios::binary);
        ASSERT_TRUE(in.good()) << file;
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
        TitleFontInfo info;
        std::string error;
        ASSERT_TRUE(readTitleFontInfo(bytes, 0, info, error)) << file << ": " << error;
        EXPECT_TRUE(info.ofl) << file;
        EXPECT_TRUE(info.reservedFontName) << file;
    }
}

TEST(TitleFontSubset, ShapesLikeTheFullFont) {
    for (const char* name : {"SourceSans3-Regular.otf", "DelaGothicOne-Regular.ttf"}) {
        const std::vector<uint8_t> bytes = fixture(name);
        const TitleSubsetResult result = subsetOf(bytes, latinPlus(""));
        ASSERT_TRUE(result.ok) << name << ": " << result.error;
        Font full(bytes), subset(result.bytes);
        for (const char* text : {"Office fit ffi! AVATAR Ty.", "Hello, World: 0123456789 $%&*@", "~{[(<quoted \"text\">)]}|"}) {
            const std::vector<Shaped> a = shape(full, text, HB_DIRECTION_LTR), b = shape(subset, text, HB_DIRECTION_LTR);
            expectSameShaping(a, b, std::string(name) + " \"" + text + "\"");
        }
        const std::vector<Shaped> office = shape(subset, "Office", HB_DIRECTION_LTR);
        if (std::string(name) == "SourceSans3-Regular.otf") { EXPECT_LT(office.size(), 6u) << "the ffi ligature survived"; }
    }
}

// AC15: a variable font keeps its axes and its instances (TrueType gvar and CFF2)
TEST(TitleFontSubset, VariableAxesKept) {
    for (const char* name : {"Oswald-wght.ttf", "SourceSans3VF-Upright.otf"}) {
        const std::vector<uint8_t> bytes = fixture(name);
        TitleFontInfo info;
        std::string error;
        ASSERT_TRUE(readTitleFontInfo(bytes, 0, info, error)) << error;
        EXPECT_TRUE(info.variable) << name;
        const TitleSubsetResult result = subsetOf(bytes, latinPlus(""));
        ASSERT_TRUE(result.ok) << name << ": " << result.error;
        Font full(bytes), subset(result.bytes);
        ASSERT_EQ(hb_ot_var_get_axis_count(full.face), hb_ot_var_get_axis_count(subset.face)) << name;
        ASSERT_GE(hb_ot_var_get_axis_count(subset.face), 1u) << name;
        hb_ot_var_axis_info_t a, b;
        unsigned one = 1;
        hb_ot_var_get_axis_infos(full.face, 0, &one, &a);
        one = 1;
        hb_ot_var_get_axis_infos(subset.face, 0, &one, &b);
        EXPECT_EQ(a.tag, b.tag);
        EXPECT_EQ(a.min_value, b.min_value);
        EXPECT_EQ(a.default_value, b.default_value);
        EXPECT_EQ(a.max_value, b.max_value);
        const std::vector<Shaped> heavy = shape(subset, "Wordmark", HB_DIRECTION_LTR, "wght=700");
        expectSameShaping(shape(full, "Wordmark", HB_DIRECTION_LTR, "wght=700"), heavy, std::string(name) + " wght 700");
        const std::vector<Shaped> light = shape(subset, "Wordmark", HB_DIRECTION_LTR, "wght=300");
        EXPECT_NE(heavy[0].xAdvance + heavy[1].xAdvance, light[0].xAdvance + light[1].xAdvance) << name << ": the axis still varies";
    }
}

// R54 budget: Latin from a ~400 KB font <= 200 ms (Release, server A); the CJK case of R54 (Joyo +
// Kana from an 8 MB OTF <= 2 s) is measured on the largest CJK fixture, a 2.5 MB TTF, and logged
TEST(TitleFontSubset, Timing) {
    const std::vector<uint8_t> latinFont = fixture("SourceSans3-Regular.otf");
    const TitleSubsetResult latin = subsetOf(latinFont, latinPlus(""));
    ASSERT_TRUE(latin.ok) << latin.error;
    TitleCharacterSet cjk, kana;
    ASSERT_TRUE(titleCharacterPreset("joyo_kanji", cjk));
    ASSERT_TRUE(titleCharacterPreset("kana", kana));
    cjk.merge(kana);
    const TitleSubsetResult japanese = subsetOf(fixture("DelaGothicOne-Regular.ttf"), cjk);
    ASSERT_TRUE(japanese.ok) << japanese.error;
    std::printf("[ subset   ] Latin from SourceSans3-Regular.otf (%zu KB): %.1f ms, %zu KB, %d glyphs\n", latinFont.size() / 1024,
                latin.ms, latin.bytes.size() / 1024, latin.glyphs);
    std::printf("[ subset   ] Joyo + Kana from DelaGothicOne-Regular.ttf (2.5 MB): %.1f ms, %zu KB, %d glyphs\n", japanese.ms,
                japanese.bytes.size() / 1024, japanese.glyphs);
#ifdef NDEBUG
    EXPECT_LE(latin.ms, 200.0);
    EXPECT_LE(japanese.ms, 2000.0);
#endif
}

// R54: subsets are cached by (font hash, set hash): the second request reads the file
TEST(TitleFontSubset, CacheBySetAndFont) {
    TestTempDir directory("title-subset-cache");
    TitleFontSubsetter subsetter(directory.path() / "subsets");
    TitleSubsetRequest request;
    request.bytes = fixture("Oswald-wght.ttf");
    request.codepoints = codepointsOf(latinPlus(""));
    const TitleSubsetResult first = subsetter.subset(request);
    ASSERT_TRUE(first.ok) << first.error;
    EXPECT_FALSE(first.cached);
    const TitleSubsetResult second = subsetter.subset(request);
    ASSERT_TRUE(second.ok);
    EXPECT_TRUE(second.cached);
    EXPECT_EQ(first.bytes, second.bytes);
    EXPECT_EQ(first.glyphs, second.glyphs);
    EXPECT_EQ(first.family, second.family);
    request.codepoints.push_back(0xc6);
    EXPECT_FALSE(subsetter.subset(request).cached) << "another set";
    EXPECT_EQ(TitleFontSubsetter::projectCacheDir("/p"), fs::path("/p/.jasperengine/cache/titleart/subsets"));
}

// R56: Restricted License fails unless acknowledged; Bitmap Only fails; No Subsetting ships whole;
// Preview & Print, Editable and Installable ship
TEST(TitleFontSubset, EmbeddingPermissions) {
    const std::vector<uint8_t> bytes = fixture("Oswald-wght.ttf");
    TitleFontInfo info;
    std::string error;
    ASSERT_TRUE(readTitleFontInfo(withFsType(bytes, 0x0002), 0, info, error)) << error;
    EXPECT_EQ(info.fsType, 0x0002);
    TitleFontEmbeddingCheck check = checkTitleFontEmbedding(info, false, "fonts/Oswald.ttf");
    EXPECT_FALSE(check.ok);
    EXPECT_EQ(check.code, "E-TITLEART-FONT-RESTRICTED");
    EXPECT_NE(check.message.find("fonts/Oswald.ttf"), std::string::npos) << check.message;
    EXPECT_NE(check.message.find("Baked"), std::string::npos) << check.message;
    EXPECT_NE(check.message.find(TITLE_FONT_ACK_LABEL), std::string::npos) << check.message;
    check = checkTitleFontEmbedding(info, true, "fonts/Oswald.ttf");
    EXPECT_TRUE(check.ok);
    EXPECT_TRUE(check.code.empty());

    ASSERT_TRUE(readTitleFontInfo(withFsType(bytes, 0x0200), 0, info, error));
    check = checkTitleFontEmbedding(info, true, "fonts/Oswald.ttf");
    EXPECT_FALSE(check.ok);
    EXPECT_EQ(check.code, "E-TITLEART-FONT-BITMAP-ONLY");
    EXPECT_NE(check.message.find("Baked"), std::string::npos) << check.message;

    for (uint16_t allowed : {0x0000, 0x0004, 0x0008}) {
        ASSERT_TRUE(readTitleFontInfo(withFsType(bytes, allowed), 0, info, error));
        check = checkTitleFontEmbedding(info, false, "fonts/Oswald.ttf");
        EXPECT_TRUE(check.ok) << allowed;
        EXPECT_TRUE(check.code.empty()) << allowed;
    }

    const std::vector<uint8_t> noSubset = withFsType(bytes, 0x0108);
    ASSERT_TRUE(readTitleFontInfo(noSubset, 0, info, error));
    check = checkTitleFontEmbedding(info, false, "fonts/Oswald.ttf");
    EXPECT_TRUE(check.ok);
    EXPECT_TRUE(check.shipWhole);
    EXPECT_EQ(check.code, "W-TITLEART-FONT-NO-SUBSET");
    const TitleSubsetResult whole = subsetOf(noSubset, latinPlus(""));
    ASSERT_TRUE(whole.ok) << whole.error;
    EXPECT_TRUE(whole.whole);
    EXPECT_EQ(whole.bytes, noSubset) << "shipped as it is";

    EXPECT_FALSE(readTitleFontInfo({'n', 'o', 't', ' ', 'a', ' ', 'f', 'o', 'n', 't'}, 0, info, error));
    EXPECT_FALSE(readTitleFontInfo(bytes, 3, info, error)) << "one face only";
}

// R56: the acknowledgement lives in the font's .meta; absent (old metas) is false
TEST(TitleFontSubset, AcknowledgementFromTheMeta) {
    TestTempDir directory("title-font-ack");
    const fs::path font = directory.path() / "Face.ttf";
    TestTempDir::write(font, "font");
    EXPECT_FALSE(titleFontEmbeddingAcknowledged(font)) << "no meta";
    TestTempDir::write(fs::path(font.string() + ".meta"), "metaVersion: 1\nguid: 4b1e9c0d2a7f4e31b6c58d0f9e2a7c13\n");
    EXPECT_FALSE(titleFontEmbeddingAcknowledged(font)) << "a meta without the key";
    TestTempDir::write(fs::path(font.string() + ".meta"),
                       "metaVersion: 1\nguid: 4b1e9c0d2a7f4e31b6c58d0f9e2a7c13\nimport:\n  font:\n    embeddingAcknowledged: true\n");
    EXPECT_TRUE(titleFontEmbeddingAcknowledged(font));
    EXPECT_TRUE(titleFontEmbeddingAcknowledged(fs::path(font.string() + ".meta")));
    TestTempDir::write(fs::path(font.string() + ".meta"),
                       "metaVersion: 1\nguid: 4b1e9c0d2a7f4e31b6c58d0f9e2a7c13\nimport:\n  font:\n    embeddingAcknowledged: false\n");
    EXPECT_FALSE(titleFontEmbeddingAcknowledged(font));
}

// R57: an OFL font whose copyright declares a Reserved Font Name is renamed in its subset, in the
// Windows (UTF-16) and Mac (Roman) records alike; without one, or shipped whole, it keeps its names
TEST(TitleFontSubset, ReservedFontNameRenamed) {
    const std::vector<uint8_t> source = fixture("SourceSans3-Regular.otf");
    TitleFontInfo info;
    std::string error;
    ASSERT_TRUE(readTitleFontInfo(source, 0, info, error)) << error;
    EXPECT_TRUE(info.ofl);
    EXPECT_TRUE(info.reservedFontName) << info.copyright;
    EXPECT_EQ(info.family, "Source Sans 3");
    const std::string hash8 = titleFontHash(source).substr(0, 8);
    const TitleSubsetResult renamed = subsetOf(source, latinPlus(""));
    ASSERT_TRUE(renamed.ok) << renamed.error;
    EXPECT_TRUE(renamed.renamed);
    EXPECT_EQ(renamed.family, "TA " + hash8);
    size_t checked = 0;
    for (const NameRecord& r : nameRecords(renamed.bytes)) {
        if (r.id == 1 || r.id == 3 || r.id == 4 || r.id == 16 || r.id == 18 || r.id == 21) {
            EXPECT_EQ(r.text, "TA " + hash8) << "name ID " << r.id << " platform " << r.platform;
            checked++;
        } else if (r.id == 6 || r.id == 25) {
            EXPECT_EQ(r.text, "TA-" + hash8) << "PostScript name ID " << r.id;
            checked++;
        } else if (r.id == 0) {
            EXPECT_NE(r.text.find("Reserved Font Name"), std::string::npos) << "the copyright stays";
        }
    }
    EXPECT_GE(checked, 4u) << "IDs 1, 3, 4 and 6 at least";
    EXPECT_FALSE(subsetOf(source, latinPlus(""), false).renamed) << "renameReserved off";

    // Dela Gothic One declares none: its subset keeps its names
    const std::vector<uint8_t> dela = fixture("DelaGothicOne-Regular.ttf");
    const TitleSubsetResult kept = subsetOf(dela, latinPlus(""));
    ASSERT_TRUE(kept.ok) << kept.error;
    EXPECT_FALSE(kept.renamed);
    EXPECT_EQ(kept.family, "Dela Gothic One");

    // ... until its copyright declares one: renamed in its Mac and Windows records
    const std::vector<uint8_t> reserved = withNameText(dela, "(https://github.com/syakuzen/DelaGothic)",
                                                       "with Reserved Font Name 'Dela Gothic'...");
    ASSERT_TRUE(readTitleFontInfo(reserved, 0, info, error)) << error;
    ASSERT_TRUE(info.reservedFontName) << info.copyright;
    const TitleSubsetResult mac = subsetOf(reserved, latinPlus(""));
    ASSERT_TRUE(mac.ok) << mac.error;
    EXPECT_TRUE(mac.renamed);
    const std::string hashDela = titleFontHash(reserved).substr(0, 8);
    std::set<uint16_t> platforms;
    for (const NameRecord& r : nameRecords(mac.bytes)) {
        if (r.id != 1 && r.id != 4 && r.id != 6) continue;
        platforms.insert(r.platform);
        EXPECT_EQ(r.text, (r.id == 6 ? "TA-" : "TA ") + hashDela) << "name ID " << r.id << " platform " << r.platform;
        EXPECT_EQ(r.text.find("Dela"), std::string::npos);
    }
    EXPECT_EQ(platforms, std::set<uint16_t>({1, 3})) << "Mac and Windows records";

    // the renamed subset still loads and shapes
    Font font(mac.bytes);
    EXPECT_EQ(shape(font, "Logo", HB_DIRECTION_LTR).size(), 4u);
}

// R57: licenses/THIRD-PARTY-FONTS.txt names each shipped font with its licence, and carries the
// OFL text once when a font is OFL
TEST(TitleFontSubset, CreditsText) {
    TitleFontInfo source, other;
    std::string error;
    ASSERT_TRUE(readTitleFontInfo(fixture("SourceSans3-Regular.otf"), 0, source, error)) << error;
    other.family = "House Face";
    other.copyright = "(c) Our Studio";
    other.license = "Licensed to Our Studio for games";
    const std::string text = titleFontCreditsText({titleFontCredit(source, "assets/fonts/SourceSans3-Regular.otf", true),
                                                   titleFontCredit(other, "assets/fonts/House.ttf", false)}, "Demo");
    EXPECT_EQ(text.rfind("Third-party fonts in Demo\n", 0), 0u);
    const std::vector<std::string> parts = {std::string("Source Sans 3\n"), std::string("  File: assets/fonts/SourceSans3-Regular.otf\n"),
                                    "  Copyright: " + source.copyright + "\n", "  Licence URL: http://scripts.sil.org/OFL\n",
                                    std::string("  Shipped: subset"), std::string("House Face\n"), std::string("  Shipped: whole\n"),
                                    std::string("  Licence URL: (not stated in the font)\n"), std::string("SIL OPEN FONT LICENSE Version 1.1")};
    for (const std::string& part : parts) {
        EXPECT_NE(text.find(part), std::string::npos) << part << "\n---\n" << text;
    }
    EXPECT_NE(text.find("This Font Software is licensed under the SIL Open Font License"), std::string::npos) << "ID 13";
    EXPECT_LT(text.find("House Face"), text.find("Source Sans 3")) << "sorted by file";
    EXPECT_EQ(text.find("SIL OPEN FONT LICENSE Version 1.1"), text.rfind("SIL OPEN FONT LICENSE Version 1.1")) << "once";
    const std::string noOfl = titleFontCreditsText({titleFontCredit(other, "assets/fonts/House.ttf", false)}, "Demo");
    EXPECT_EQ(noOfl.find("SIL OPEN FONT LICENSE"), std::string::npos);
    EXPECT_STREQ(TITLE_FONT_CREDITS_FILE, "THIRD-PARTY-FONTS.txt");
}
