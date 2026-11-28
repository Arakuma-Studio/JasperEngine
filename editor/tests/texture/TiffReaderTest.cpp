// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor's baseline TIFF reader (docs/specs/S94-rendering-anim-gaps.md F50, AC47). The
// fixtures are written here by a small TIFF encoder (both byte orders, strips and tiles, chunky
// and planar, every supported compression, Predictor 2): each decodes to the pixels it was
// written from. What the reader refuses names the supported set; malformed files fail, never crash.
// TextureEditorCodecs: once the editor registers its codecs, a .tif and a .psb (its flattened
// image) load through TextureData as stb_image's colour images do, so the cook reads them.

#include <gtest/gtest.h>

#include "TempDir.h"

#include "texture/TextureData.h"
#include "texture/TextureEncoders.h"
#include "texture/TiffReader.h"
#include "titleart/interchange/psd/PsdReader.h"
#include "titleart/interchange/psd/PsdWriter.h"

#include "libs/miniz/miniz.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine::editor;

namespace {

    struct TiffSpec {
        bool little = true;
        uint16_t compression = 1;
        int bits = 8;
        int colours = 1;                // 1 grey, 3 RGB
        bool alpha = false;
        uint16_t extraSample = 2;       // ExtraSamples: 0 unspecified, 1 associated, 2 unassociated
        uint16_t photometric = 0xffff;  // default: 1 (grey) or 2 (RGB)
        bool tiled = false;
        int rowsPerStrip = 5;
        int tile = 16;
        bool predictor = false;
        int planar = 1;
        int width = 37, height = 23;
        bool noise = false;             // random samples (LZW past its 4,094 table entries)
        uint16_t sampleFormat = 1;
    };

    int samplesOf(const TiffSpec& s) { return s.colours + (s.alpha ? 1 : 0); }

    uint32_t maxOf(const TiffSpec& s) { return s.bits == 16 ? 65535u : 255u; }

    // the source: the samples of pixel (x, y)
    uint32_t sampleAt(const TiffSpec& s, int x, int y, int c) {
        uint32_t v;
        if (s.noise) {
            uint32_t h = ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u) ^ ((uint32_t)c * 83492791u);
            h ^= h >> 13;
            h *= 0x5bd1e995u;
            v = h ^ (h >> 15);
        } else {
            v = (uint32_t)(x * 977 + y * 1531 + c * 4099) * (s.bits == 16 ? 37u : 1u);
        }
        return v & maxOf(s);
    }

    // a byte stream in the file's byte order
    struct Out {
        bool little = true;
        std::vector<uint8_t> bytes;
        void u16(uint32_t v) {
            if (little) bytes.insert(bytes.end(), {(uint8_t)v, (uint8_t)(v >> 8)});
            else bytes.insert(bytes.end(), {(uint8_t)(v >> 8), (uint8_t)v});
        }
        void u32(uint32_t v) {
            if (little) bytes.insert(bytes.end(), {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)});
            else bytes.insert(bytes.end(), {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v});
        }
        void patch32(size_t at, uint32_t v) {
            Out o;
            o.little = little;
            o.u32(v);
            std::memcpy(bytes.data() + at, o.bytes.data(), 4);
        }
    };

    // TIFF LZW as libtiff writes it: MSB-first codes, the width growing when the next free code
    // passes the current maximum, a Clear when the table reaches 4,094
    std::vector<uint8_t> lzwEncode(const std::vector<uint8_t>& in) {
        std::vector<uint8_t> out;
        uint32_t acc = 0;
        int accBits = 0, nbits = 9, freeEnt = 258;
        auto put = [&](int code) {
            acc = (acc << nbits) | (uint32_t)code;
            accBits += nbits;
            while (accBits >= 8) {
                out.push_back((uint8_t)(acc >> (accBits - 8)));
                accBits -= 8;
            }
            acc &= (1u << accBits) - 1;
        };
        std::map<std::pair<int, int>, int> table;
        auto added = [&]() {
            freeEnt++;
            if (freeEnt == 4094) {
                put(256);
                table.clear();
                freeEnt = 258;
                nbits = 9;
            } else if (freeEnt > (1 << nbits) - 1) {
                nbits++;
            }
        };
        put(256);
        int prefix = in.empty() ? -1 : in[0];
        for (size_t i = 1; i < in.size(); i++) {
            const auto found = table.find({prefix, in[i]});
            if (found != table.end()) {
                prefix = found->second;
                continue;
            }
            put(prefix);
            table[{prefix, in[i]}] = freeEnt;
            added();
            prefix = in[i];
        }
        if (prefix >= 0) {
            put(prefix);
            added();
        }
        put(257);
        if (accBits > 0) out.push_back((uint8_t)(acc << (8 - accBits)));
        return out;
    }

    std::vector<uint8_t> compress(const TiffSpec& s, const std::vector<uint8_t>& raw, size_t rowBytes) {
        switch (s.compression) {
            case 5: return lzwEncode(raw);
            case 8: case 32946: {
                mz_ulong length = mz_compressBound((mz_ulong)raw.size());
                std::vector<uint8_t> packed(length);
                mz_compress2(packed.data(), &length, raw.data(), (mz_ulong)raw.size(), 6);
                packed.resize(length);
                return packed;
            }
            case 32773: {
                // each row packed alone, as the specification asks
                std::vector<uint8_t> packed;
                for (size_t at = 0; at < raw.size(); at += rowBytes) psd::packBits(raw.data() + at, rowBytes, packed);
                return packed;
            }
            default: return raw;
        }
    }

    // one strip or tile: `cw` x `ch` pixels at (x0, y0), one plane (planar) or every sample
    std::vector<uint8_t> chunkBytes(const TiffSpec& s, int x0, int y0, int cw, int ch, int plane) {
        const int perPixel = s.planar == 2 ? 1 : samplesOf(s);
        const int bps = s.bits / 8;
        Out o;
        o.little = s.little;
        std::vector<uint32_t> row((size_t)cw * perPixel);
        for (int y = 0; y < ch; y++) {
            for (int x = 0; x < cw; x++) {
                for (int c = 0; c < perPixel; c++) {
                    const int px = x0 + x, py = y0 + y;
                    // a tile's part past the image is padding
                    row[(size_t)x * perPixel + c] = px < s.width && py < s.height ? sampleAt(s, px, py, s.planar == 2 ? plane : c) : 0;
                }
            }
            if (s.predictor) {
                for (size_t i = row.size(); i-- > (size_t)perPixel;) row[i] = (row[i] - row[i - perPixel]) & maxOf(s);
            }
            for (uint32_t v : row) {
                if (bps == 1) o.bytes.push_back((uint8_t)v);
                else o.u16(v);
            }
        }
        return compress(s, o.bytes, (size_t)cw * perPixel * bps);
    }

    struct Entry {
        uint16_t tag, type;
        std::vector<uint32_t> values;
    };

    std::vector<uint8_t> writeTiff(const TiffSpec& s) {
        Out o;
        o.little = s.little;
        o.bytes = s.little ? std::vector<uint8_t>{'I', 'I'} : std::vector<uint8_t>{'M', 'M'};
        o.u16(42);
        o.u32(0);       // the IFD's offset, patched
        const int samples = samplesOf(s);
        const int cw = s.tiled ? s.tile : s.width;
        const int ch = s.tiled ? s.tile : std::min(s.rowsPerStrip, s.height);
        const int across = (s.width + cw - 1) / cw, down = (s.height + ch - 1) / ch;
        std::vector<uint32_t> offsets, counts;
        for (int plane = 0; plane < (s.planar == 2 ? samples : 1); plane++) {
            for (int cy = 0; cy < down; cy++) {
                for (int cx = 0; cx < across; cx++) {
                    const int rows = s.tiled ? ch : std::min(ch, s.height - cy * ch);
                    const std::vector<uint8_t> chunk = chunkBytes(s, cx * cw, cy * ch, cw, rows, plane);
                    offsets.push_back((uint32_t)o.bytes.size());
                    counts.push_back((uint32_t)chunk.size());
                    o.bytes.insert(o.bytes.end(), chunk.begin(), chunk.end());
                }
            }
        }
        const uint16_t photometric = s.photometric != 0xffff ? s.photometric : (s.colours == 3 ? 2 : 1);
        std::vector<Entry> entries = {
            {256, 3, {(uint32_t)s.width}},
            {257, 3, {(uint32_t)s.height}},
            {258, 3, std::vector<uint32_t>((size_t)samples, (uint32_t)s.bits)},
            {259, 3, {s.compression}},
            {262, 3, {photometric}},
            {277, 3, {(uint32_t)samples}},
            {284, 3, {(uint32_t)s.planar}},
            {339, 3, std::vector<uint32_t>((size_t)samples, s.sampleFormat)},
        };
        if (s.tiled) {
            entries.push_back({322, 3, {(uint32_t)cw}});
            entries.push_back({323, 3, {(uint32_t)ch}});
            entries.push_back({324, 4, offsets});
            entries.push_back({325, 4, counts});
        } else {
            entries.push_back({273, 4, offsets});
            entries.push_back({278, 3, {(uint32_t)ch}});
            entries.push_back({279, 4, counts});
        }
        if (s.predictor) entries.push_back({317, 3, {2}});
        if (s.alpha) entries.push_back({338, 3, {s.extraSample}});
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.tag < b.tag; });
        // values that do not fit in 4 bytes go before the IFD
        std::vector<uint32_t> valueAt(entries.size(), 0);
        for (size_t i = 0; i < entries.size(); i++) {
            const size_t width = entries[i].type == 3 ? 2 : 4;
            if (entries[i].values.size() * width <= 4) continue;
            if (o.bytes.size() % 2) o.bytes.push_back(0);
            valueAt[i] = (uint32_t)o.bytes.size();
            for (uint32_t v : entries[i].values) {
                if (width == 2) o.u16(v);
                else o.u32(v);
            }
        }
        if (o.bytes.size() % 2) o.bytes.push_back(0);
        o.patch32(4, (uint32_t)o.bytes.size());
        o.u16((uint32_t)entries.size());
        for (size_t i = 0; i < entries.size(); i++) {
            const Entry& e = entries[i];
            o.u16(e.tag);
            o.u16(e.type);
            o.u32((uint32_t)e.values.size());
            const size_t width = e.type == 3 ? 2 : 4;
            if (e.values.size() * width > 4) {
                o.u32(valueAt[i]);
            } else {
                // inline, left-justified
                const size_t start = o.bytes.size();
                for (uint32_t v : e.values) {
                    if (width == 2) o.u16(v);
                    else o.u32(v);
                }
                while (o.bytes.size() < start + 4) o.bytes.push_back(0);
            }
        }
        o.u32(0);
        return o.bytes;
    }

    // the straight RGBA the reader should give
    std::vector<uint16_t> expectedRgba(const TiffSpec& s) {
        std::vector<uint16_t> out((size_t)s.width * s.height * 4);
        const uint32_t maxValue = maxOf(s);
        for (int y = 0; y < s.height; y++) {
            for (int x = 0; x < s.width; x++) {
                uint16_t* p = &out[((size_t)y * s.width + x) * 4];
                for (int c = 0; c < 3; c++) {
                    uint32_t v = sampleAt(s, x, y, s.colours == 3 ? c : 0);
                    if (s.colours == 1 && s.photometric == 0) v = maxValue - v;
                    p[c] = (uint16_t)v;
                }
                p[3] = (uint16_t)(s.alpha ? sampleAt(s, x, y, s.colours) : maxValue);
            }
        }
        return out;
    }

    uint16_t decodedSample(const TiffImage& image, size_t i) {
        if (image.bitDepth == 8) return image.rgba[i];
        uint16_t v;
        std::memcpy(&v, image.rgba.data() + i * 2, 2);
        return v;
    }

    std::string describe(const TiffSpec& s) {
        return std::string(s.little ? "II" : "MM") + " compression " + std::to_string(s.compression) + " " + std::to_string(s.bits) + "-bit " +
               (s.colours == 3 ? "RGB" : "grey") + (s.alpha ? "+alpha" : "") + (s.tiled ? " tiles" : " strips") + (s.predictor ? " predictor 2" : "") +
               (s.planar == 2 ? " planar" : "");
    }

    void expectDecodes(const TiffSpec& s) {
        SCOPED_TRACE(describe(s));
        const std::vector<uint8_t> file = writeTiff(s);
        TiffImage image;
        std::string error;
        ASSERT_TRUE(readTiff(file.data(), file.size(), image, error)) << error;
        ASSERT_EQ(image.width, s.width);
        ASSERT_EQ(image.height, s.height);
        ASSERT_EQ(image.bitDepth, s.bits);
        const std::vector<uint16_t> expected = expectedRgba(s);
        ASSERT_EQ(image.rgba.size(), expected.size() * (size_t)(s.bits / 8));
        size_t mismatches = 0, first = 0;
        for (size_t i = 0; i < expected.size(); i++) {
            if (decodedSample(image, i) != expected[i] && mismatches++ == 0) first = i;
        }
        EXPECT_EQ(mismatches, 0u) << "first at sample " << first << ": " << decodedSample(image, first) << " instead of " << expected[first];
    }

    void expectRefused(const TiffSpec& s, const std::string& naming) {
        const std::vector<uint8_t> file = writeTiff(s);
        TiffImage image;
        std::string error;
        EXPECT_FALSE(readTiff(file.data(), file.size(), image, error)) << naming;
        EXPECT_NE(error.find(naming), std::string::npos) << error;
        EXPECT_NE(error.find(TIFF_SUPPORTED_SET), std::string::npos) << "the message names the supported set: " << error;
    }

}

// AC47: each compression, bit depth, channel layout, chunk kind and byte order decodes equal to its source
TEST(TiffReaderTest, Baseline) {
    for (bool little : {true, false}) {
        for (uint16_t compression : {(uint16_t)1, (uint16_t)5, (uint16_t)8, (uint16_t)32946, (uint16_t)32773}) {
            for (int bits : {8, 16}) {
                for (int layout = 0; layout < 4; layout++) {
                    for (bool tiled : {false, true}) {
                        for (bool predictor : {false, true}) {
                            if (predictor && compression != 5 && compression != 8) continue;
                            TiffSpec s;
                            s.little = little;
                            s.compression = compression;
                            s.bits = bits;
                            s.colours = layout >= 2 ? 3 : 1;
                            s.alpha = layout % 2 == 1;
                            s.tiled = tiled;
                            s.predictor = predictor;
                            expectDecodes(s);
                        }
                    }
                }
            }
        }
    }
}

TEST(TiffReaderTest, PlanarWhiteIsZeroAndLzwTableResets) {
    for (int bits : {8, 16}) {
        TiffSpec planar;
        planar.colours = 3;
        planar.alpha = true;
        planar.planar = 2;
        planar.bits = bits;
        planar.compression = 8;
        planar.predictor = true;
        expectDecodes(planar);
        planar.tiled = true;
        planar.little = false;
        expectDecodes(planar);

        TiffSpec white;
        white.photometric = 0;
        white.bits = bits;
        white.compression = 32773;
        expectDecodes(white);
    }
    // random samples fill the LZW table: the encoder's Clear codes reset it
    TiffSpec noise;
    noise.noise = true;
    noise.colours = 3;
    noise.alpha = true;
    noise.width = 128;
    noise.height = 96;
    noise.rowsPerStrip = 96;
    noise.compression = 5;
    expectDecodes(noise);
    noise.bits = 16;
    noise.little = false;
    expectDecodes(noise);
}

// associated (premultiplied) alpha comes back straight; unassociated is kept as it is
TEST(TiffReaderTest, AssociatedAlphaIsUnpremultiplied) {
    TiffSpec s;
    s.colours = 3;
    s.alpha = true;
    s.extraSample = 2;
    std::vector<uint8_t> file = writeTiff(s);
    TiffImage straight;
    std::string error;
    ASSERT_TRUE(readTiff(file.data(), file.size(), straight, error)) << error;
    s.extraSample = 1;
    file = writeTiff(s);
    TiffImage associated;
    ASSERT_TRUE(readTiff(file.data(), file.size(), associated, error)) << error;
    size_t checked = 0;
    for (size_t i = 0; i < straight.rgba.size(); i += 4) {
        const uint32_t a = straight.rgba[i + 3];
        EXPECT_EQ(associated.rgba[i + 3], a);
        for (int c = 0; c < 3; c++) {
            const uint32_t premultiplied = straight.rgba[i + c];
            if (a == 0) {
                EXPECT_EQ(associated.rgba[i + c], 0);
            } else if (premultiplied <= a) {
                // the reader's colour times alpha gives the stored (premultiplied) value back
                EXPECT_NEAR(associated.rgba[i + c] * a / 255.0, premultiplied, 0.5 + a / 510.0) << "pixel " << i / 4;
                checked++;
            } else {
                EXPECT_EQ(associated.rgba[i + c], 255) << "a colour over its alpha clamps";
            }
        }
    }
    EXPECT_GT(checked, 100u);
}

TEST(TiffReaderTest, ConvertsSixteenBitToRgba8) {
    TiffSpec s;
    s.bits = 16;
    s.colours = 3;
    const std::vector<uint8_t> file = writeTiff(s);
    TiffImage image;
    std::string error;
    ASSERT_TRUE(readTiff(file.data(), file.size(), image, error)) << error;
    const std::vector<uint8_t> rgba8 = tiffToRgba8(image);
    ASSERT_EQ(rgba8.size(), image.rgba.size() / 2);
    for (size_t i = 0; i < rgba8.size(); i++) EXPECT_EQ(rgba8[i], (uint8_t)((decodedSample(image, i) * 255u + 32767u) / 65535u));
}

// spec §9: what the reader does not read is refused with a message naming the supported set
TEST(TiffReaderTest, RefusesOutsideTheBaselineNamingTheSupportedSet) {
    TiffSpec jpeg;
    jpeg.compression = 7;
    expectRefused(jpeg, "JPEG");
    TiffSpec cmyk;
    cmyk.colours = 3;
    cmyk.alpha = true;          // four samples
    cmyk.photometric = 5;
    expectRefused(cmyk, "CMYK");
    TiffSpec palette;
    palette.photometric = 3;
    expectRefused(palette, "palette");
    TiffSpec floats;
    floats.sampleFormat = 3;
    expectRefused(floats, "floating point");
    TiffSpec four;
    four.bits = 4;
    expectRefused(four, "4-bit");
}

TEST(TiffReaderTest, MalformedFilesFailWithoutCrashing) {
    TiffSpec s;
    s.colours = 3;
    s.compression = 5;
    const std::vector<uint8_t> good = writeTiff(s);
    TiffImage image;
    std::string error;
    // every truncation: the IFD is at the end, so each one loses it or its values (the reader
    // never follows the next-IFD link, its last 4 bytes)
    for (size_t n = 0; n + 4 < good.size(); n++) {
        EXPECT_FALSE(readTiff(good.data(), n, image, error)) << "truncated to " << n << " bytes";
        EXPECT_FALSE(error.empty());
    }
    // a strip offset past the end of the file
    {
        TiffSpec one = s;
        one.rowsPerStrip = s.height;    // one strip: its offset is inline in the IFD entry
        std::vector<uint8_t> bad = writeTiff(one);
        const uint32_t ifd = bad[4] | bad[5] << 8 | bad[6] << 16 | (uint32_t)bad[7] << 24;
        const uint16_t count = (uint16_t)(bad[ifd] | bad[ifd + 1] << 8);
        bool patched = false;
        for (uint16_t i = 0; i < count; i++) {
            uint8_t* e = &bad[ifd + 2 + (size_t)i * 12];
            if ((e[0] | e[1] << 8) == 273) {
                e[8] = e[9] = e[10] = 0xff;
                e[11] = 0x7f;
                patched = true;
            }
        }
        ASSERT_TRUE(patched);
        EXPECT_FALSE(readTiff(bad.data(), bad.size(), image, error));
        EXPECT_NE(error.find("past the end"), std::string::npos) << error;
    }
    // the first IFD past the end
    {
        std::vector<uint8_t> bad = good;
        bad[4] = bad[5] = bad[6] = 0xff;
        EXPECT_FALSE(readTiff(bad.data(), bad.size(), image, error));
        EXPECT_NE(error.find("past the end"), std::string::npos) << error;
    }
    // corrupted bytes anywhere: a result either way, never a crash
    uint32_t seed = 12345;
    for (int round = 0; round < 3000; round++) {
        std::vector<uint8_t> bad = good;
        for (int k = 0; k < 4; k++) {
            seed = seed * 1664525u + 1013904223u;
            bad[(seed >> 8) % bad.size()] = (uint8_t)(seed >> 24);
        }
        readTiff(bad.data(), bad.size(), image, error);
    }
}

TEST(TextureEditorCodecs, TifAndPsbLoadThroughTextureData) {
    using jasperengine::TextureData;
    registerEditorTextureCodecs();
    jasperengine::test::TempDir dir;

    TiffSpec s;
    s.colours = 3;
    s.alpha = true;
    s.bits = 16;
    s.compression = 8;
    const std::vector<uint8_t> tif = writeTiff(s);
    TiffImage image;
    std::string error;
    ASSERT_TRUE(readTiff(tif.data(), tif.size(), image, error)) << error;
    const std::vector<uint8_t> expectedTif = tiffToRgba8(image);
    const std::string tifPath = dir.write("photo.TIF", std::string(tif.begin(), tif.end())).generic_string();

    // the PSB's merged image; its layers are not what a texture shows
    psd::PsdWriteDocument doc;
    doc.psb = true;
    doc.width = 9;
    doc.height = 7;
    doc.merged.resize((size_t)doc.width * doc.height * 4);
    for (size_t i = 0; i < doc.merged.size(); i++) doc.merged[i] = (uint8_t)(i * 11 + 3);
    std::vector<uint8_t> psb;
    ASSERT_TRUE(psd::writePsd(doc, psb, error)) << error;
    const std::string psbPath = dir.write("art.psb", std::string(psb.begin(), psb.end())).generic_string();

    struct Case {
        std::string path;
        std::vector<uint8_t> bytes, expected;
        int width, height;
    };
    const Case cases[] = {{tifPath, tif, expectedTif, s.width, s.height}, {psbPath, psb, doc.merged, doc.width, doc.height}};
    for (const Case& c : cases) {
        SCOPED_TRACE(c.path);
        for (bool fromBytes : {false, true}) {
            TextureData texture;
            std::vector<uint8_t> bytes = c.bytes;
            ASSERT_TRUE(fromBytes ? texture.loadTextureFromFileData(c.path.c_str(), bytes.data(), (unsigned int)bytes.size())
                                  : texture.loadTextureFromFile(c.path.c_str()));
            EXPECT_EQ(texture.getWidth(), c.width);
            EXPECT_EQ(texture.getHeight(), c.height);
            EXPECT_EQ(texture.getColorFormat(), jasperengine::ColorFormat::RGBA);
            EXPECT_EQ(texture.getChannels(), 4);
            EXPECT_EQ(texture.getGpuFormat(), jasperengine::GpuTextureFormat::RGBA8);
            EXPECT_TRUE(texture.isSrgb());
            ASSERT_EQ(texture.getSize(), c.expected.size());
            EXPECT_EQ(std::memcmp(texture.getData(), c.expected.data(), c.expected.size()), 0);
            texture.releaseImageData();
        }
    }
}
