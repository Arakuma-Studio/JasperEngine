// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/S82-title-designer.md R59, AC26: WOFF and WOFF2 to sfnt. The fixtures are S80's
// Oswald test font (OFL) re-encoded by fontTools 4.68.0 (editor/tests/fixtures/titleart/fonts);
// the reference is the TTF itself. WOFF: every table byte for byte. WOFF2: every table but
// glyf / loca byte for byte, glyf / loca (rebuilt by the decoder) compared glyph by glyph through
// S80's shaper and HarfBuzz hb-draw, at the default and at two weights (gvar reads the rebuilt
// points). Then the refusals: truncated, a bad checksum, a damaged Brotli stream, over 64 MB.

#include <gtest/gtest.h>

#include "titleart/fonts/FontTestSupport.h"

#include "titleart/fonts/FontTables.h"
#include "titleart/fonts/WebFontDecode.h"

#include <algorithm>

using namespace jasperengine::editor::titlefonts;
using namespace jasperengine::editor::titlefonts::test;

namespace {

    const std::string ALL_TEXT =
        "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
        "\xC3\x80\xC3\x89\xC3\x91\xC3\x96\xC3\x9C\xC3\x9F\xC3\xA0\xC3\xA9\xC3\xB1\xC3\xB6\xC3\xBC";   // ÀÉÑÖÜßàéñöü

    std::vector<uint8_t> reference() { return readBytes(oswaldPath()); }
    std::vector<uint8_t> woff() { return readBytes(webFontFixturesDir() / "Oswald-wght.woff"); }
    std::vector<uint8_t> woff2() { return readBytes(webFontFixturesDir() / "Oswald-wght.woff2"); }

    // head without checkSumAdjustment (8..11, the whole file's), flags (16..17: WOFF2 sets bit 11
    // for transformed tables) and indexToLocFormat (50..51: the decoder picks loca's format)
    std::vector<uint8_t> comparableHead(std::vector<uint8_t> head, bool woff2Encoded) {
        if (head.size() < 54) return head;
        for (size_t i = 8; i < 12; i++) head[i] = 0;
        if (woff2Encoded) {
            head[16] = head[17] = 0;
            head[50] = head[51] = 0;
        }
        return head;
    }

    void expectSameOutlines(const std::vector<uint8_t>& decoded, const jasperengine::titleart::Json& variations) {
        MemoryFonts fonts;
        fonts.add("reference", reference());
        fonts.add("decoded", decoded);
        const auto expected = glyphOutlines(fonts, "reference", ALL_TEXT, variations);
        const auto actual = glyphOutlines(fonts, "decoded", ALL_TEXT, variations);
        ASSERT_EQ(actual.size(), expected.size());
        ASSERT_GT(expected.size(), 90u);
        size_t inked = 0;
        for (size_t i = 0; i < expected.size(); i++) {
            inked += expected[i].empty() ? 0 : 1;
            EXPECT_EQ(outlineDifference(expected[i], actual[i]), "") << "glyph " << i << " of the text, variations " << variations.dump();
        }
        EXPECT_GT(inked, 90u) << "the comparison saw real outlines";
    }

}

TEST(WebFontDecodeTest, SniffsFormats) {
    EXPECT_EQ(sniffFontFormat(woff().data(), woff().size()), FontFileFormat::Woff);
    EXPECT_EQ(sniffFontFormat(woff2().data(), woff2().size()), FontFileFormat::Woff2);
    EXPECT_EQ(sniffFontFormat(reference().data(), reference().size()), FontFileFormat::TrueType);
    const std::vector<uint8_t> cff = readBytes(sourceSansPath());
    EXPECT_EQ(sniffFontFormat(cff.data(), cff.size()), FontFileFormat::OpenTypeCff);
    EXPECT_EQ(sfntExtension(cff.data(), cff.size()), ".otf");
    EXPECT_EQ(sfntExtension(reference().data(), reference().size()), ".ttf");
    EXPECT_TRUE(isWebFontFileName("Brand.WOFF2"));
    EXPECT_TRUE(isWebFontFileName("brand.woff"));
    EXPECT_FALSE(isWebFontFileName("brand.ttf"));
}

// Red control: writing the inflated tables without their WOFF order sorted (or dropping one)
// fails the table comparison; skipping the inflate (copying the compressed bytes) fails it too
TEST(WebFontDecodeTest, WoffTablesEqualTheReference) {
    const std::vector<uint8_t> source = woff();
    const DecodedFont decoded = decodeWebFont(source.data(), source.size());
    ASSERT_TRUE(decoded.ok) << decoded.error;
    EXPECT_EQ(decoded.extension, ".ttf");
    const auto expected = sfntTableMap(reference());
    const auto actual = sfntTableMap(decoded.bytes);
    ASSERT_EQ(actual.size(), expected.size());
    for (const auto& [tag, bytes] : expected) {
        ASSERT_TRUE(actual.count(tag)) << tag;
        if (tag == "head") EXPECT_EQ(comparableHead(actual.at(tag), false), comparableHead(bytes, false));
        else EXPECT_EQ(actual.at(tag), bytes) << tag;
    }
    // the whole file sums to the magic number with the new checkSumAdjustment
    EXPECT_EQ(sfntChecksum(decoded.bytes.data(), decoded.bytes.size()), 0xB1B0AFBAu);
    EXPECT_EQ(listFontFaces(decoded.bytes.data(), decoded.bytes.size()).front().family, "Oswald");
}

// Red control: skipping the glyf transform (libs/woff2/src/woff2_dec.cc ReconstructFont writing
// the transformed glyf stream as the glyf table) fails every outline comparison here
TEST(WebFontDecodeTest, Woff2TablesEqualTheReferenceAndOutlinesMatch) {
    const std::vector<uint8_t> source = woff2();
    const DecodedFont decoded = decodeWebFont(source.data(), source.size());
    ASSERT_TRUE(decoded.ok) << decoded.error;
    EXPECT_EQ(decoded.extension, ".ttf");
    const auto expected = sfntTableMap(reference());
    const auto actual = sfntTableMap(decoded.bytes);
    for (const auto& [tag, bytes] : expected) {
        ASSERT_TRUE(actual.count(tag)) << tag;
        if (tag == "glyf" || tag == "loca") continue;
        if (tag == "head") EXPECT_EQ(comparableHead(actual.at(tag), true), comparableHead(bytes, true));
        else EXPECT_EQ(actual.at(tag), bytes) << tag;
    }
    expectSameOutlines(decoded.bytes, jasperengine::titleart::Json::object());
    expectSameOutlines(decoded.bytes, jasperengine::titleart::Json{{"wght", 200}});
    expectSameOutlines(decoded.bytes, jasperengine::titleart::Json{{"wght", 700}});
}

TEST(WebFontDecodeTest, TruncatedFilesAreRefused) {
    for (const std::vector<uint8_t>& source : {woff(), woff2()}) {
        const std::vector<uint8_t> cut(source.begin(), source.begin() + (std::ptrdiff_t)(source.size() / 2));
        const DecodedFont decoded = decodeWebFont(cut.data(), cut.size());
        EXPECT_FALSE(decoded.ok);
        EXPECT_NE(decoded.error.find("truncated"), std::string::npos) << decoded.error;
        const DecodedFont header = decodeWebFont(source.data(), 20);
        EXPECT_FALSE(header.ok);
        EXPECT_NE(header.error.find("truncated"), std::string::npos) << header.error;
    }
}

// Red control: not comparing the computed checksum with the directory's lets the first case pass
TEST(WebFontDecodeTest, BadChecksumAndDamagedStreamsAreRefused) {
    // WOFF: the first table's recorded checksum (directory entry 0, bytes 16..19)
    std::vector<uint8_t> badChecksum = woff();
    put32(badChecksum, 44 + 16, be32(badChecksum, 44 + 16) ^ 0x00010000u);
    DecodedFont decoded = decodeWebFont(badChecksum.data(), badChecksum.size());
    EXPECT_FALSE(decoded.ok);
    EXPECT_NE(decoded.error.find("checksum"), std::string::npos) << decoded.error;

    // WOFF: a compressed table's zlib stream
    std::vector<uint8_t> badZlib = woff();
    const uint32_t offset = be32(badZlib, 44 + 4), compLength = be32(badZlib, 44 + 8), origLength = be32(badZlib, 44 + 12);
    ASSERT_LT(compLength, origLength) << "the first table is compressed";
    for (uint32_t i = 2; i < std::min<uint32_t>(compLength, 40); i++) badZlib[offset + i] ^= 0x5A;
    decoded = decodeWebFont(badZlib.data(), badZlib.size());
    EXPECT_FALSE(decoded.ok);
    EXPECT_FALSE(decoded.error.empty());

    // WOFF2: the Brotli stream after the header and the table directory
    std::vector<uint8_t> badBrotli = woff2();
    for (size_t i = badBrotli.size() / 3; i < badBrotli.size() / 3 + 64; i++) badBrotli[i] ^= 0xA5;
    decoded = decodeWebFont(badBrotli.data(), badBrotli.size());
    EXPECT_FALSE(decoded.ok);
    EXPECT_NE(decoded.error.find("WOFF2"), std::string::npos) << decoded.error;
}

// Red control: dropping the size checks makes these decode (the header's size is a lie, the
// tables are small) and pass ok
TEST(WebFontDecodeTest, MoreThan64MegabytesIsRefused) {
    constexpr uint32_t over = 65u * 1024 * 1024;
    // the header's totalSfntSize (offset 16 in both formats)
    for (std::vector<uint8_t> source : {woff(), woff2()}) {
        put32(source, 16, over);
        const DecodedFont decoded = decodeWebFont(source.data(), source.size());
        EXPECT_FALSE(decoded.ok);
        EXPECT_NE(decoded.error.find("64 MB"), std::string::npos) << decoded.error;
    }
    // WOFF: a table directory whose decoded lengths add up to more, with an honest header
    std::vector<uint8_t> source = woff();
    put32(source, 44 + 12, over);
    const DecodedFont decoded = decodeWebFont(source.data(), source.size());
    EXPECT_FALSE(decoded.ok);
    EXPECT_NE(decoded.error.find("64 MB"), std::string::npos) << decoded.error;
    // the same limit given smaller: the real font is over it while decoding
    const std::vector<uint8_t> real = woff2();
    const DecodedFont limited = decodeWoff2(real.data(), real.size(), 64 * 1024);
    EXPECT_FALSE(limited.ok);
}

TEST(WebFontDecodeTest, PlainFontsAreNotWebFonts) {
    const std::vector<uint8_t> ttf = reference();
    const DecodedFont decoded = decodeWebFont(ttf.data(), ttf.size());
    EXPECT_FALSE(decoded.ok);
    EXPECT_NE(decoded.error.find("not a web font"), std::string::npos) << decoded.error;
    const std::vector<uint8_t> junk(100, 7);
    EXPECT_FALSE(decodeWebFont(junk.data(), junk.size()).ok);
}

// Red control: reading only the first face of a collection lists one face and fails
TEST(WebFontDecodeTest, ListsTheFacesOfACollection) {
    const std::vector<uint8_t> ttc = buildCollection({readBytes(oswaldPath()), readBytes(sourceSansPath())});
    EXPECT_EQ(sniffFontFormat(ttc.data(), ttc.size()), FontFileFormat::Collection);
    EXPECT_EQ(sfntExtension(ttc.data(), ttc.size()), ".ttc");
    const std::vector<FontFaceListing> faces = listFontFaces(ttc.data(), ttc.size());
    ASSERT_EQ(faces.size(), 2u);
    EXPECT_EQ(faces[0].index, 0);
    EXPECT_EQ(faces[0].family, "Oswald");
    EXPECT_EQ(faces[0].style, "Regular");
    EXPECT_EQ(faces[1].index, 1);
    EXPECT_EQ(faces[1].family, "Source Sans 3");
    const std::vector<uint8_t> single = reference();
    EXPECT_EQ(listFontFaces(single.data(), single.size()).size(), 1u);
}
