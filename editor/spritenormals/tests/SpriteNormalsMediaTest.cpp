// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// jasperengine-spritenormals-tests: the sources of the generator (SpriteMedia). GIFs and APNGs are
// built in the test (an LZW encoder; PNG chunks with stored deflate) so every disposal, blend,
// timing and limit case is exact; the upstream fixtures check the atlases of real files.

#include "spritenormals/SpriteMedia.h"
#include "object/ui/Image.h"

#include "json.hpp"
#include "stb_image_write.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <unordered_map>
#include <random>

using namespace jasperengine::editor::spritenormals;
namespace fs = std::filesystem;
namespace texsynth = jasperengine::editor::texsynth;

namespace {

    const fs::path kFixtures = SPRITENORMALS_FIXTURES;

    using Bytes = std::vector<uint8_t>;

    // ---------------------------------------------------------------- a GIF writer

    // LZW codes, LSB first, in sub-blocks of at most 255 bytes. The table grows to 12 bits; when
    // full the encoder emits a clear code, or (deferClear) keeps coding with the full table
    Bytes lzw(const std::vector<uint8_t>& indices, int minCodeSize, bool deferClear = false) {
        Bytes packed;
        uint32_t bits = 0;
        int count = 0;
        int codeSize = minCodeSize + 1;
        auto emit = [&](int code) {
            bits |= static_cast<uint32_t>(code) << count;
            count += codeSize;
            while (count >= 8) {
                packed.push_back(static_cast<uint8_t>(bits));
                bits >>= 8;
                count -= 8;
            }
        };
        const int clear = 1 << minCodeSize, end = clear + 1;
        std::unordered_map<uint32_t, int> table;   // (prefix code << 8 | index) -> code
        int next = end + 1;
        emit(clear);
        if (!indices.empty()) {
            int prefix = indices[0];
            for (size_t i = 1; i < indices.size(); i++) {
                const int c = indices[i];
                const auto found = table.find(static_cast<uint32_t>(prefix) << 8 | static_cast<uint32_t>(c));
                if (found != table.end()) {
                    prefix = found->second;
                    continue;
                }
                emit(prefix);
                if (next < 4096) {
                    table[static_cast<uint32_t>(prefix) << 8 | static_cast<uint32_t>(c)] = next++;
                    if (next > (1 << codeSize) && codeSize < 12) codeSize++;
                } else if (!deferClear) {
                    emit(clear);
                    table.clear();
                    next = end + 1;
                    codeSize = minCodeSize + 1;
                }
                prefix = c;
            }
            emit(prefix);
        }
        emit(end);
        if (count > 0) packed.push_back(static_cast<uint8_t>(bits));
        Bytes out;
        out.push_back(static_cast<uint8_t>(minCodeSize));
        for (size_t i = 0; i < packed.size(); i += 255) {
            const size_t n = std::min<size_t>(255, packed.size() - i);
            out.push_back(static_cast<uint8_t>(n));
            out.insert(out.end(), packed.begin() + static_cast<std::ptrdiff_t>(i), packed.begin() + static_cast<std::ptrdiff_t>(i + n));
        }
        out.push_back(0);
        return out;
    }

    void le16(Bytes& b, int v) {
        b.push_back(static_cast<uint8_t>(v));
        b.push_back(static_cast<uint8_t>(v >> 8));
    }

    struct GifFrame {
        int left = 0, top = 0, width = 0, height = 0;
        std::vector<uint8_t> indices;          // row-major (in interlaced order when interlaced)
        int disposal = 0;
        int delayCs = 0;
        int transparentIndex = -1;
        bool interlaced = false;
        std::vector<std::array<uint8_t, 3>> localPalette;
        bool deferClear = false;
        Bytes lzwData;                         // when set: the frame's LZW data as is
    };

    std::vector<std::array<uint8_t, 3>> basePalette() {
        // 0 black, 1 red, 2 green, 3 blue, then greys
        std::vector<std::array<uint8_t, 3>> p = {{0, 0, 0}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
        for (int i = 4; i < 256; i++) p.push_back({static_cast<uint8_t>(i), static_cast<uint8_t>(i), static_cast<uint8_t>(i)});
        return p;
    }

    void palette(Bytes& b, const std::vector<std::array<uint8_t, 3>>& colors) {
        for (const auto& c : colors) b.insert(b.end(), c.begin(), c.end());
    }

    // loop < 0: no NETSCAPE extension
    Bytes gif(int width, int height, const std::vector<GifFrame>& frames, int loop = 0, bool trailer = true,
              const std::vector<std::array<uint8_t, 3>>& global = basePalette()) {
        Bytes b = {'G', 'I', 'F', '8', '9', 'a'};
        le16(b, width);
        le16(b, height);
        int bitsMinus1 = 0;
        while ((2 << bitsMinus1) < static_cast<int>(global.size())) bitsMinus1++;
        b.push_back(static_cast<uint8_t>(global.empty() ? 0 : (0x80 | 0x70 | bitsMinus1)));
        b.push_back(0);
        b.push_back(0);
        if (!global.empty()) {
            std::vector<std::array<uint8_t, 3>> padded = global;
            padded.resize(static_cast<size_t>(2) << bitsMinus1);
            palette(b, padded);
        }
        if (loop >= 0) {
            const char app[] = "NETSCAPE2.0";
            b.push_back(0x21);
            b.push_back(0xFF);
            b.push_back(11);
            b.insert(b.end(), app, app + 11);
            b.push_back(3);
            b.push_back(1);
            le16(b, loop);
            b.push_back(0);
        }
        for (const GifFrame& f : frames) {
            b.push_back(0x21);
            b.push_back(0xF9);
            b.push_back(4);
            b.push_back(static_cast<uint8_t>((f.disposal << 2) | (f.transparentIndex >= 0 ? 1 : 0)));
            le16(b, f.delayCs);
            b.push_back(static_cast<uint8_t>(std::max(f.transparentIndex, 0)));
            b.push_back(0);
            b.push_back(0x2C);
            le16(b, f.left);
            le16(b, f.top);
            le16(b, f.width);
            le16(b, f.height);
            uint8_t packed = f.interlaced ? 0x40 : 0;
            std::vector<std::array<uint8_t, 3>> local = f.localPalette;
            int localBits = 0;
            if (!local.empty()) {
                while ((2 << localBits) < static_cast<int>(local.size())) localBits++;
                local.resize(static_cast<size_t>(2) << localBits);
                packed |= static_cast<uint8_t>(0x80 | localBits);
            }
            b.push_back(packed);
            if (!local.empty()) palette(b, local);
            const Bytes data = f.lzwData.empty() ? lzw(f.indices, 8, f.deferClear) : f.lzwData;
            b.insert(b.end(), data.begin(), data.end());
        }
        if (trailer) b.push_back(0x3B);
        return b;
    }

    GifFrame fill(int left, int top, int w, int h, uint8_t index, int disposal, int delayCs) {
        GifFrame f;
        f.left = left;
        f.top = top;
        f.width = w;
        f.height = h;
        f.indices.assign(static_cast<size_t>(w * h), index);
        f.disposal = disposal;
        f.delayCs = delayCs;
        return f;
    }

    // ---------------------------------------------------------------- an APNG writer

    uint32_t crc(const uint8_t* data, size_t n, uint32_t c = 0) {
        c = ~c;
        for (size_t i = 0; i < n; i++) {
            c ^= data[i];
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        return ~c;
    }

    void be32(Bytes& b, uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s));
    }

    void chunk(Bytes& b, const char* type, const Bytes& data) {
        be32(b, static_cast<uint32_t>(data.size()));
        const size_t start = b.size();
        b.insert(b.end(), type, type + 4);
        b.insert(b.end(), data.begin(), data.end());
        be32(b, crc(b.data() + start, data.size() + 4));
    }

    // zlib with stored (uncompressed) deflate blocks
    Bytes zlibStored(const Bytes& raw) {
        Bytes z = {0x78, 0x01};
        size_t i = 0;
        do {
            const size_t n = std::min<size_t>(65535, raw.size() - i);
            z.push_back(i + n == raw.size() ? 1 : 0);
            z.push_back(static_cast<uint8_t>(n));
            z.push_back(static_cast<uint8_t>(n >> 8));
            z.push_back(static_cast<uint8_t>(~n));
            z.push_back(static_cast<uint8_t>(~n >> 8));
            z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(i), raw.begin() + static_cast<std::ptrdiff_t>(i + n));
            i += n;
        } while (i < raw.size());
        uint32_t a = 1, bsum = 0;
        for (uint8_t v : raw) {
            a = (a + v) % 65521;
            bsum = (bsum + a) % 65521;
        }
        be32(z, (bsum << 16) | a);
        return z;
    }

    // Scanlines (filter 0) of samples: `bytesPerPixel` bytes per pixel, as given
    Bytes scanlines(int w, int h, const Bytes& samples, int bytesPerPixel) {
        Bytes raw;
        for (int y = 0; y < h; y++) {
            raw.push_back(0);
            raw.insert(raw.end(), samples.begin() + static_cast<std::ptrdiff_t>(y * w * bytesPerPixel),
                       samples.begin() + static_cast<std::ptrdiff_t>((y + 1) * w * bytesPerPixel));
        }
        return zlibStored(raw);
    }

    Bytes rgbaSamples(int w, int h, std::array<uint8_t, 4> c) {
        Bytes s;
        for (int i = 0; i < w * h; i++) s.insert(s.end(), c.begin(), c.end());
        return s;
    }

    struct ApngFrame {
        int x = 0, y = 0, w = 0, h = 0;
        Bytes samples;                 // in the file's colour type and depth
        uint16_t delayNum = 1, delayDen = 10;
        uint8_t dispose = 0, blend = 0;
        Bytes zlibData;                // when set: the frame's compressed data as is
    };

    struct ApngOptions {
        int bitDepth = 8;
        int colorType = 6;
        int bytesPerPixel = 4;
        Bytes plte, trns;
        bool defaultIsFrame = true;    // fcTL before IDAT
        Bytes defaultSamples;          // the IDAT image when it is not a frame
        uint32_t plays = 0;
        bool badCrc = false;
        bool decreasingSequence = false;
        int declaredFrames = -1;       // acTL num_frames (-1: the frame count)
    };

    Bytes apng(int width, int height, const std::vector<ApngFrame>& frames, const ApngOptions& o = ApngOptions()) {
        Bytes b = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        Bytes ihdr;
        be32(ihdr, static_cast<uint32_t>(width));
        be32(ihdr, static_cast<uint32_t>(height));
        ihdr.push_back(static_cast<uint8_t>(o.bitDepth));
        ihdr.push_back(static_cast<uint8_t>(o.colorType));
        ihdr.push_back(0);
        ihdr.push_back(0);
        ihdr.push_back(0);
        chunk(b, "IHDR", ihdr);
        Bytes actl;
        be32(actl, static_cast<uint32_t>(o.declaredFrames >= 0 ? static_cast<size_t>(o.declaredFrames) : frames.size()));
        be32(actl, o.plays);
        chunk(b, "acTL", actl);
        if (!o.plte.empty()) chunk(b, "PLTE", o.plte);
        if (!o.trns.empty()) chunk(b, "tRNS", o.trns);
        uint32_t sequence = 0;
        auto fctl = [&](const ApngFrame& f) {
            Bytes d;
            be32(d, sequence++);
            be32(d, static_cast<uint32_t>(f.w));
            be32(d, static_cast<uint32_t>(f.h));
            be32(d, static_cast<uint32_t>(f.x));
            be32(d, static_cast<uint32_t>(f.y));
            d.push_back(static_cast<uint8_t>(f.delayNum >> 8));
            d.push_back(static_cast<uint8_t>(f.delayNum));
            d.push_back(static_cast<uint8_t>(f.delayDen >> 8));
            d.push_back(static_cast<uint8_t>(f.delayDen));
            d.push_back(f.dispose);
            d.push_back(f.blend);
            chunk(b, "fcTL", d);
        };
        for (size_t i = 0; i < frames.size(); i++) {
            const ApngFrame& f = frames[i];
            const Bytes data = f.zlibData.empty() ? scanlines(f.w, f.h, f.samples, o.bytesPerPixel) : f.zlibData;
            if (i == 0) {
                if (o.defaultIsFrame) {
                    fctl(f);
                    chunk(b, "IDAT", data);
                    continue;
                }
                chunk(b, "IDAT", scanlines(width, height, o.defaultSamples, o.bytesPerPixel));
            }
            fctl(f);
            Bytes d;
            if (o.decreasingSequence && i == frames.size() - 1) sequence -= 3;
            be32(d, sequence++);
            d.insert(d.end(), data.begin(), data.end());
            chunk(b, "fdAT", d);
        }
        chunk(b, "IEND", {});
        if (o.badCrc) b[b.size() - 13] ^= 0xFF;   // the last fdAT's CRC (IEND is 12 bytes)
        return b;
    }

    Source load(const Bytes& bytes, std::vector<Problem>& problems) {
        Source source;
        EXPECT_TRUE(loadSourceFromMemory(bytes.data(), bytes.size(), source, problems)) << (problems.empty() ? "" : problems[0].message);
        return source;
    }

    Source load(const Bytes& bytes) {
        std::vector<Problem> problems;
        return load(bytes, problems);
    }

    Problem refused(const Bytes& bytes) {
        Source source;
        std::vector<Problem> problems;
        EXPECT_FALSE(loadSourceFromMemory(bytes.data(), bytes.size(), source, problems));
        return problems.empty() ? Problem{} : problems[0];
    }

    std::array<uint8_t, 4> px(const Image& image, int x, int y) {
        std::array<uint8_t, 4> c;
        std::memcpy(c.data(), image.pixel(x, y), 4);
        return c;
    }

    using C = std::array<uint8_t, 4>;
    const C kClear = {0, 0, 0, 0};
    const C kRed = {255, 0, 0, 255};
    const C kGreen = {0, 255, 0, 255};
    const C kBlue = {0, 0, 255, 255};

    fs::path tempDir() {
        const fs::path dir = fs::temp_directory_path() / ("spritenormals-media-" + std::to_string(std::random_device()()));
        fs::create_directories(dir);
        return dir;
    }

    void writeFile(const fs::path& path, const Bytes& bytes) {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // frame i of a source's atlas, as bytes
    Bytes crop3(const Source& s, int i) {
        const Cell& c = s.cells.at(static_cast<size_t>(i));
        Bytes out;
        for (int y = 0; y < c.height; y++) out.insert(out.end(), s.color.pixel(c.x, c.y + y), s.color.pixel(c.x, c.y + y) + c.width * 4);
        return out;
    }

    void appendBytes(void* context, void* data, int size) {
        Bytes* b = static_cast<Bytes*>(context);
        b->insert(b->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    }
}

// ---------------------------------------------------------------- GIF

// upstream tests/media.rs: offsets, disposal 3, timing and the blank fourth atlas cell
TEST(SpriteNormalsGif, DisposalOffsetsTimingAndBlankCells) {
    const Source s = load(gif(16, 16, {fill(0, 0, 16, 16, 1, 0, 5), fill(4, 4, 4, 4, 2, 3, 9), fill(10, 10, 2, 2, 3, 0, 12)}));
    EXPECT_EQ(s.kind, SourceKind::Gif);
    ASSERT_EQ(s.frameCount, 3);
    EXPECT_EQ(s.color.width, 32);
    EXPECT_EQ(s.color.height, 32);
    EXPECT_EQ(s.columns, 2);
    EXPECT_EQ(s.rows, 2);
    ASSERT_EQ(s.delays.size(), 3u);
    EXPECT_EQ(s.delays[0].numerator, 50u);
    EXPECT_EQ(s.delays[1].numerator, 90u);
    EXPECT_EQ(s.delays[2].numerator, 120u);
    EXPECT_EQ(s.delays[2].denominator, 1u);
    EXPECT_EQ(px(s.color, 20, 4), kGreen);    // frame 2 (cell 16,0) at (4,4)
    EXPECT_EQ(px(s.color, 4, 20), kRed);      // frame 3: frame 2 restored away
    EXPECT_EQ(px(s.color, 10, 26), kBlue);
    EXPECT_EQ(px(s.color, 24, 24)[3], 0);     // no fourth frame
    EXPECT_EQ(s.cells, (std::vector<Cell>{{0, 0, 16, 16}, {16, 0, 16, 16}, {0, 16, 16, 16}}));
    EXPECT_EQ(s.frameWidth, 16);
    EXPECT_EQ(s.fileChannels, 4);
    EXPECT_EQ(s.loopCount, 0);
    EXPECT_EQ(s.sha1.size(), 40u);
}

TEST(SpriteNormalsGif, DisposalTwoClearsToTransparent) {
    const Source s = load(gif(8, 8, {fill(0, 0, 8, 8, 1, 2, 10), fill(2, 2, 2, 2, 2, 1, 10), fill(6, 6, 1, 1, 3, 0, 10)}));
    ASSERT_EQ(s.frameCount, 3);
    EXPECT_EQ(px(s.color, 8 + 0, 0), kClear);   // frame 2: frame 1 cleared
    EXPECT_EQ(px(s.color, 8 + 2, 2), kGreen);
    EXPECT_EQ(px(s.color, 0 + 2, 8 + 2), kGreen);   // frame 3: disposal 1 kept frame 2
    EXPECT_EQ(px(s.color, 0 + 6, 8 + 6), kBlue);
    EXPECT_EQ(px(s.color, 0, 8), kClear);
}

TEST(SpriteNormalsGif, InterlaceLocalPaletteTransparencyAndOutOfRangeIndices) {
    // 4 x 10 frame, row r has index r + 4 (grey r + 4), stored in interlaced order
    GifFrame f = fill(0, 0, 4, 10, 0, 1, 10);
    f.interlaced = true;
    f.indices.clear();
    for (auto [start, step] : {std::pair{0, 8}, std::pair{4, 8}, std::pair{2, 4}, std::pair{1, 2}})
        for (int y = start; y < 10; y += step)
            for (int x = 0; x < 4; x++) f.indices.push_back(static_cast<uint8_t>(y + 4));
    // second frame: a local palette (index 1 = yellow), index 0 transparent, index 9 beyond the
    // 2-colour... 4-entry local table (out of range: transparent)
    GifFrame g = fill(0, 0, 4, 10, 1, 0, 10);
    g.localPalette = {{0, 0, 0}, {255, 255, 0}, {1, 2, 3}};
    g.transparentIndex = 0;
    g.indices[0] = 0;
    g.indices[1] = 9;
    const Source s = load(gif(4, 10, {f, g}));
    ASSERT_EQ(s.frameCount, 2);
    for (int y = 0; y < 10; y++) EXPECT_EQ(px(s.color, 1, y), (C{static_cast<uint8_t>(y + 4), static_cast<uint8_t>(y + 4), static_cast<uint8_t>(y + 4), 255})) << y;
    EXPECT_EQ(px(s.color, 4 + 0, 0), (C{4, 4, 4, 255}));      // transparent index: frame 1 shows
    EXPECT_EQ(px(s.color, 4 + 1, 0), (C{4, 4, 4, 255}));      // beyond the table: transparent
    EXPECT_EQ(px(s.color, 4 + 2, 0), (C{255, 255, 0, 255}));  // local palette
}

TEST(SpriteNormalsGif, LoopCount) {
    EXPECT_EQ(load(gif(2, 2, {fill(0, 0, 2, 2, 1, 0, 1)}, 0)).loopCount, 0);
    EXPECT_EQ(load(gif(2, 2, {fill(0, 0, 2, 2, 1, 0, 1)}, 3)).loopCount, 3);
    EXPECT_EQ(load(gif(2, 2, {fill(0, 0, 2, 2, 1, 0, 1)}, -1)).loopCount, 1);
}

TEST(SpriteNormalsGif, LzwGrowthClearAndDeferredClear) {
    std::mt19937 rng(7);
    GifFrame f = fill(0, 0, 64, 64, 0, 0, 10);
    for (uint8_t& i : f.indices) i = static_cast<uint8_t>(rng() % 256);   // the table fills: clear codes
    GifFrame d = f;
    d.deferClear = true;   // the table stays full, codes keep 12 bits
    GifFrame runs = fill(0, 0, 64, 64, 0, 0, 10);
    for (size_t i = 0; i < runs.indices.size(); i++) runs.indices[i] = static_cast<uint8_t>((i / 37) % 7 + 4);   // long strings
    for (const GifFrame& frame : {f, d, runs}) {
        const Source s = load(gif(64, 64, {frame}));
        ASSERT_EQ(s.frameCount, 1);
        for (int i = 0; i < 64 * 64; i++) {
            const uint8_t v = frame.indices[static_cast<size_t>(i)];
            const C expected = v < 4 ? (std::array<C, 4>{C{0, 0, 0, 255}, kRed, kGreen, kBlue})[v] : C{v, v, v, 255};
            ASSERT_EQ(px(s.color, i % 64, i / 64), expected) << i;
        }
    }
}

TEST(SpriteNormalsGif, MalformedAndOversized) {
    // truncated LZW data: invalid_source naming the frame and an offset
    Bytes cut = gif(16, 16, {fill(0, 0, 16, 16, 1, 0, 5), fill(0, 0, 16, 16, 2, 0, 5)});
    GifFrame noise = fill(0, 0, 16, 16, 0, 0, 1);
    std::mt19937 rng(3);
    for (uint8_t& i : noise.indices) i = static_cast<uint8_t>(rng() % 256);
    Bytes truncated = gif(16, 16, {fill(0, 0, 16, 16, 1, 0, 5), noise}, 0, false);
    truncated.resize(truncated.size() - 120);
    truncated.push_back(0);   // the sub-blocks end: the LZW stream stops early
    Problem p = refused(truncated);
    EXPECT_EQ(p.kind, "invalid_source");
    EXPECT_NE(p.message.find("LZW data ends early in frame 2 (offset "), std::string::npos) << p.message;
    // a missing trailer after complete frames is accepted
    EXPECT_EQ(load(gif(4, 4, {fill(0, 0, 4, 4, 1, 0, 5)}, 0, false)).frameCount, 1);
    // not an image
    const Bytes text = {'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e'};
    EXPECT_EQ(refused(text).kind, "invalid_source");
    // a 65535x65535 screen: refused from the header
    Bytes huge = gif(16, 16, {fill(0, 0, 16, 16, 1, 0, 5)});
    huge[6] = huge[7] = huge[8] = huge[9] = 0xFF;
    p = refused(huge);
    EXPECT_EQ(p.kind, "too_large");
    EXPECT_NE(p.message.find("65535x65535"), std::string::npos) << p.message;
    EXPECT_NE(p.message.find("16777216"), std::string::npos);
    // 300 frames: too_large at frame 257, which is garbage and never decoded
    std::vector<GifFrame> many(300, fill(0, 0, 2, 2, 1, 0, 1));
    Bytes long_ = gif(2, 2, many);
    // corrupt every LZW code size byte after frame 256 (2..8 are valid; 0 would be refused if decoded)
    int seen = 0;
    for (size_t i = 0; i + 10 < long_.size(); i++) {
        if (long_[i] == 0x2C && long_[i + 10] == 8 && ++seen > 256) long_[i + 10] = 0;
    }
    p = refused(long_);
    EXPECT_EQ(p.kind, "too_large");
    EXPECT_NE(p.message.find("frame 257"), std::string::npos) << p.message;
    // decoded pixels: 2 frames of 3000x3000 exceed 16777216
    Bytes big = gif(3000, 3000, {fill(0, 0, 1, 1, 1, 0, 1), fill(0, 0, 1, 1, 1, 0, 1)});
    p = refused(big);
    EXPECT_EQ(p.kind, "too_large");
    EXPECT_NE(p.message.find("18000000"), std::string::npos) << p.message;
}

TEST(SpriteNormalsGif, ProbeCountsFramesWithoutDecoding) {
    std::vector<GifFrame> frames(5, fill(0, 0, 4, 4, 1, 0, 1));
    Bytes bytes = gif(4, 4, frames);
    // frames 3..5 have garbage LZW (a code size of 0) in valid sub-blocks
    int seen = 0;
    for (size_t i = 0; i + 10 < bytes.size(); i++) {
        if (bytes[i] == 0x2C && bytes[i + 10] == 8 && ++seen > 2) bytes[i + 10] = 0;
    }
    const fs::path dir = tempDir();
    writeFile(dir / "five.gif", bytes);
    SourceInfo info;
    std::vector<Problem> problems;
    ASSERT_TRUE(probeSource(dir / "five.gif", info, problems));
    EXPECT_EQ(info.kind, SourceKind::Gif);
    EXPECT_EQ(info.frameCount, 5);
    EXPECT_EQ(info.width, 4);
    Source s;
    EXPECT_FALSE(loadSource(dir / "five.gif", s, problems));
    ASSERT_FALSE(problems.empty());
    EXPECT_EQ(problems.back().message.rfind("five.gif: ", 0), 0u) << problems.back().message;
    fs::remove_all(dir);
}

// ---------------------------------------------------------------- APNG

namespace {
    ApngFrame rect(int x, int y, int w, int h, C c, uint8_t dispose = 0, uint8_t blend = 0, uint16_t num = 1, uint16_t den = 10) {
        ApngFrame f;
        f.x = x;
        f.y = y;
        f.w = w;
        f.h = h;
        f.samples = rgbaSamples(w, h, c);
        f.dispose = dispose;
        f.blend = blend;
        f.delayNum = num;
        f.delayDen = den;
        return f;
    }
}

TEST(SpriteNormalsApng, DefaultImageAsFrameOrNot) {
    const Source a = load(apng(4, 4, {rect(0, 0, 4, 4, kRed), rect(1, 1, 2, 2, kGreen)}));
    EXPECT_EQ(a.kind, SourceKind::Apng);
    ASSERT_EQ(a.frameCount, 2);
    EXPECT_EQ(px(a.color, 0, 0), kRed);
    EXPECT_EQ(px(a.color, 4 + 1, 1), kGreen);
    EXPECT_EQ(px(a.color, 4 + 0, 0), kRed);
    ApngOptions o;
    o.defaultIsFrame = false;
    o.defaultSamples = rgbaSamples(4, 4, kBlue);
    const Source b = load(apng(4, 4, {rect(0, 0, 4, 4, kRed), rect(1, 1, 2, 2, kGreen)}, o));
    ASSERT_EQ(b.frameCount, 2);   // the blue default image is not part of the animation
    EXPECT_EQ(px(b.color, 0, 0), kRed);
    EXPECT_EQ(px(b.color, 5, 1), kGreen);
}

TEST(SpriteNormalsApng, DisposeOps) {
    // frame 2 (green patch) with dispose 0 / 1 / 2, then frame 3 a blue pixel at (3, 3)
    for (uint8_t dispose : {0, 1, 2}) {
        const Source s = load(apng(4, 4, {rect(0, 0, 4, 4, kRed), rect(1, 1, 2, 2, kGreen, dispose), rect(3, 3, 1, 1, kBlue)}));
        ASSERT_EQ(s.frameCount, 3);
        const C at = px(s.color, 0 + 1, 4 + 1);   // frame 3 at (1, 1)
        EXPECT_EQ(at, dispose == 0 ? kGreen : (dispose == 1 ? kClear : kRed)) << int(dispose);
        EXPECT_EQ(px(s.color, 3, 4 + 3), kBlue);
    }
    // dispose 2 on the first frame counts as 1 (cleared)
    const Source first = load(apng(2, 2, {rect(0, 0, 2, 2, kRed, 2), rect(0, 0, 1, 1, kGreen)}));
    EXPECT_EQ(px(first.color, 2 + 1, 1), kClear);
    EXPECT_EQ(px(first.color, 2 + 0, 0), kGreen);
}

TEST(SpriteNormalsApng, BlendOverWithHalfTransparency) {
    // destination red (255, 0, 0, 255) then blue with alpha 128, blended over: alpha 255,
    // colour (0*128 + 255*255*127/255) / 255 = 127 red, 128 blue
    const Source s = load(apng(2, 1, {rect(0, 0, 2, 1, kRed), rect(0, 0, 1, 1, {0, 0, 255, 128}, 0, 1)}));
    EXPECT_EQ(px(s.color, 2, 0), (C{127, 0, 128, 255}));
    EXPECT_EQ(px(s.color, 3, 0), kRed);
    // over a half-transparent destination: outA = 128 + 128 * 127 / 255 = 191.75 -> 192;
    // red = (255 * 128 * 127 / 255) / 191.75 = 84.78 -> 85, blue = 255 * 128 / 191.75 = 170.2 -> 170
    const Source h = load(apng(1, 1, {rect(0, 0, 1, 1, {255, 0, 0, 128}), rect(0, 0, 1, 1, {0, 0, 255, 128}, 0, 1)}));
    EXPECT_EQ(px(h.color, 1, 0), (C{85, 0, 170, 192}));
    // source: replace, alpha included
    const Source r = load(apng(1, 1, {rect(0, 0, 1, 1, kRed), rect(0, 0, 1, 1, {0, 0, 255, 128}, 0, 0)}));
    EXPECT_EQ(px(r.color, 1, 0), (C{0, 0, 255, 128}));
    // over with a fully transparent result
    const Source z = load(apng(1, 1, {rect(0, 0, 1, 1, {9, 9, 9, 0}), rect(0, 0, 1, 1, {7, 7, 7, 0}, 0, 1)}));
    EXPECT_EQ(px(z.color, 1, 0), kClear);
}

TEST(SpriteNormalsApng, DelaysLoopAndMalformed) {
    ApngOptions o;
    o.plays = 2;
    const Source s = load(apng(1, 1, {rect(0, 0, 1, 1, kRed, 0, 0, 1, 12), rect(0, 0, 1, 1, kGreen, 0, 0, 3, 0)}, o));
    ASSERT_EQ(s.delays.size(), 2u);
    EXPECT_EQ(s.delays[0].numerator, 250u);   // 1/12 s = 1000/12 ms reduced
    EXPECT_EQ(s.delays[0].denominator, 3u);
    EXPECT_NEAR(s.delays[0].ms(), 83.333, 1e-3);
    EXPECT_EQ(s.delays[0].playbackMs(), 83);
    EXPECT_EQ(s.delays[1].numerator, 30u);    // den 0 means 1/100 s
    EXPECT_EQ(s.delays[1].denominator, 1u);
    EXPECT_EQ(s.loopCount, 2);
    EXPECT_EQ((Delay{0, 1}).playbackMs(), 100);
    EXPECT_EQ((Delay{9, 1}).playbackMs(), 100);
    EXPECT_EQ((Delay{10, 1}).playbackMs(), 10);
    ApngOptions bad;
    bad.badCrc = true;
    Problem p = refused(apng(2, 2, {rect(0, 0, 2, 2, kRed), rect(0, 0, 1, 1, kGreen)}, bad));
    EXPECT_EQ(p.kind, "invalid_source");
    EXPECT_NE(p.message.find("CRC"), std::string::npos) << p.message;
    ApngOptions seq;
    seq.decreasingSequence = true;
    p = refused(apng(2, 2, {rect(0, 0, 2, 2, kRed), rect(0, 0, 1, 1, kGreen), rect(1, 1, 1, 1, kBlue)}, seq));
    EXPECT_EQ(p.kind, "invalid_source");
    EXPECT_NE(p.message.find("sequence"), std::string::npos) << p.message;
    // a frame outside the canvas, a first frame not covering it
    EXPECT_EQ(refused(apng(2, 2, {rect(0, 0, 2, 2, kRed), rect(1, 1, 2, 2, kGreen)})).kind, "invalid_source");
    EXPECT_EQ(refused(apng(2, 2, {rect(0, 0, 1, 1, kRed)})).kind, "invalid_source");
    // too many frames: refused from acTL
    std::vector<ApngFrame> many(257, rect(0, 0, 1, 1, kRed));
    p = refused(apng(1, 1, many));
    EXPECT_EQ(p.kind, "too_large");
    EXPECT_NE(p.message.find("257"), std::string::npos) << p.message;
}

TEST(SpriteNormalsApng, SixteenBitAndPalette) {
    // 16-bit RGBA: 0x8080 -> 128 ((v + 128) / 257), 0xFFFF -> 255
    ApngOptions o16;
    o16.bitDepth = 16;
    o16.bytesPerPixel = 8;
    ApngFrame f = rect(0, 0, 1, 1, kRed);
    f.samples = {0x80, 0x80, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    ApngFrame g = f;
    g.samples = {0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xFF, 0xFF};
    const Source s = load(apng(1, 1, {f, g}, o16));
    EXPECT_EQ(px(s.color, 0, 0), (C{128, 0, 255, 255}));
    EXPECT_EQ(px(s.color, 1, 0), (C{0, 64, 0, 255}));
    EXPECT_EQ(s.fileBitDepth, 16);
    // palette with tRNS: index 1 half-transparent green
    ApngOptions op;
    op.colorType = 3;
    op.bytesPerPixel = 1;
    op.plte = {255, 0, 0, 0, 255, 0};
    op.trns = {255, 100};
    ApngFrame a = rect(0, 0, 2, 1, kRed);
    a.samples = {0, 1};
    ApngFrame b = rect(1, 0, 1, 1, kRed);
    b.samples = {0};
    const Source p = load(apng(2, 1, {a, b}, op));
    EXPECT_EQ(px(p.color, 0, 0), kRed);
    EXPECT_EQ(px(p.color, 1, 0), (C{0, 255, 0, 100}));
    EXPECT_EQ(px(p.color, 3, 0), kRed);
}

// ---------------------------------------------------------------- stills

TEST(SpriteNormalsStill, FormatsBecomeRgba8) {
    // 16-bit RGBA PNG
    texsynth::Image wide = texsynth::Image::make(2, 1, 4, 16);
    const uint16_t samples[8] = {0xFFFF, 0x8080, 0, 0xFFFF, 0x0101, 0, 0, 0x7F7F};
    std::memcpy(wide.data.data(), samples, sizeof(samples));
    Bytes png;
    ASSERT_TRUE(texsynth::encodePng(wide, png));
    Source s = load(png);
    EXPECT_EQ(s.kind, SourceKind::Image);
    EXPECT_EQ(px(s.color, 0, 0), (C{255, 128, 0, 255}));
    EXPECT_EQ(px(s.color, 1, 0), (C{1, 0, 0, 127}));
    EXPECT_EQ(s.fileBitDepth, 16);
    EXPECT_EQ(s.fileChannels, 4);
    EXPECT_TRUE(s.cells.empty());
    EXPECT_EQ(s.fileWidth, 2);
    // palette PNG with tRNS (built by hand)
    Bytes pal = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    Bytes ihdr;
    be32(ihdr, 2);
    be32(ihdr, 1);
    ihdr.insert(ihdr.end(), {8, 3, 0, 0, 0});
    chunk(pal, "IHDR", ihdr);
    chunk(pal, "PLTE", {10, 20, 30, 40, 50, 60});
    chunk(pal, "tRNS", {255, 0});
    chunk(pal, "IDAT", scanlines(2, 1, {0, 1}, 1));
    chunk(pal, "IEND", {});
    s = load(pal);
    EXPECT_EQ(px(s.color, 0, 0), (C{10, 20, 30, 255}));
    EXPECT_EQ(px(s.color, 1, 0), (C{40, 50, 60, 0}));
    // grey + alpha
    const uint8_t ga[4] = {77, 200, 3, 4};
    png.clear();
    stbi_write_png_to_func(appendBytes, &png, 2, 1, 2, ga, 4);
    s = load(png);
    EXPECT_EQ(px(s.color, 0, 0), (C{77, 77, 77, 200}));
    EXPECT_EQ(s.fileChannels, 2);
    // JPEG: opaque
    std::vector<uint8_t> rgb(16 * 16 * 3, 120);
    png.clear();
    stbi_write_jpg_to_func(appendBytes, &png, 16, 16, 3, rgb.data(), 95);
    s = load(png);
    EXPECT_EQ(s.color.width, 16);
    EXPECT_EQ(px(s.color, 5, 5)[3], 255);
    EXPECT_NEAR(px(s.color, 5, 5)[0], 120, 3);
    // HDR refused
    std::vector<float> hdr(4 * 3, 0.5f);
    png.clear();
    stbi_write_hdr_to_func(appendBytes, &png, 2, 2, 3, hdr.data());
    const Problem p = refused(png);
    EXPECT_EQ(p.kind, "invalid_source");
    EXPECT_EQ(p.fix, "Use a PNG (or JPEG/TGA/BMP) image, a GIF or an APNG.");
}

TEST(SpriteNormalsStill, FileLimitsAndProbe) {
    const fs::path dir = tempDir();
    // over 64 MiB: refused before reading (a sparse file)
    {
        std::ofstream(dir / "huge.png").put('x');
    }
    fs::resize_file(dir / "huge.png", static_cast<uintmax_t>(kMaxInputBytes) + 1);
    Source s;
    std::vector<Problem> problems;
    EXPECT_FALSE(loadSource(dir / "huge.png", s, problems));
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0].kind, "too_large");
    EXPECT_NE(problems[0].message.find("67108865"), std::string::npos) << problems[0].message;
    SourceInfo info;
    problems.clear();
    EXPECT_FALSE(probeSource(dir / "huge.png", info, problems));
    EXPECT_EQ(problems.at(0).kind, "too_large");
    // missing
    problems.clear();
    EXPECT_FALSE(loadSource(dir / "missing.png", s, problems));
    EXPECT_EQ(problems.at(0).kind, "invalid_source");
    // probe a still and an APNG, and a still too large from its header
    problems.clear();
    ASSERT_TRUE(probeSource(kFixtures / "inputs" / "sheet.png", info, problems));
    EXPECT_EQ(info.kind, SourceKind::Image);
    EXPECT_EQ(info.width, 128);
    EXPECT_EQ(info.frameCount, 1);
    ASSERT_TRUE(probeSource(kFixtures / "inputs" / "twoframes.png", info, problems));
    EXPECT_EQ(info.kind, SourceKind::Apng);
    EXPECT_EQ(info.frameCount, 2);
    ASSERT_TRUE(probeSource(kFixtures / "inputs" / "walk.gif", info, problems));
    EXPECT_EQ(info.frameCount, 8);
    Bytes big = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    Bytes ihdr;
    be32(ihdr, 5000);
    be32(ihdr, 5000);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(big, "IHDR", ihdr);
    chunk(big, "IDAT", zlibStored({0}));
    chunk(big, "IEND", {});
    writeFile(dir / "big.png", big);
    problems.clear();
    EXPECT_FALSE(probeSource(dir / "big.png", info, problems));
    EXPECT_EQ(problems.at(0).kind, "too_large");
    problems.clear();
    EXPECT_FALSE(loadSource(dir / "big.png", s, problems));
    EXPECT_EQ(problems.at(0).kind, "too_large");
    EXPECT_NE(problems.at(0).message.find("25000000"), std::string::npos);
    fs::remove_all(dir);
}

// ---------------------------------------------------------------- atlas, encoding, upstream fixtures

TEST(SpriteNormalsAtlas, PackLayouts) {
    for (auto [n, columns, rows] : {std::tuple{1, 1, 1}, std::tuple{2, 2, 1}, std::tuple{3, 2, 2}, std::tuple{5, 3, 2}, std::tuple{10, 4, 3}}) {
        std::vector<Image> frames;
        for (int i = 0; i < n; i++) {
            Image f = Image::make(3, 2, 4, 8);
            std::fill(f.data.begin(), f.data.end(), static_cast<uint8_t>(i + 1));
            frames.push_back(f);
        }
        Image atlas;
        int c = 0, r = 0;
        std::vector<Cell> cells;
        std::string error;
        ASSERT_TRUE(packAtlas(frames, atlas, c, r, cells, error)) << error;
        EXPECT_EQ(c, columns) << n;
        EXPECT_EQ(r, rows) << n;
        EXPECT_EQ(atlas.width, 3 * columns);
        EXPECT_EQ(atlas.height, 2 * rows);
        ASSERT_EQ(cells.size(), static_cast<size_t>(n));
        for (int i = 0; i < n; i++) {
            EXPECT_EQ(cells[static_cast<size_t>(i)], (Cell{(i % columns) * 3, (i / columns) * 2, 3, 2}));
            EXPECT_EQ(atlas.pixel(cells[static_cast<size_t>(i)].x + 2, cells[static_cast<size_t>(i)].y + 1)[0], i + 1);
        }
        if (n < columns * rows) {
            EXPECT_EQ(atlas.pixel(atlas.width - 1, atlas.height - 1)[3], 0);   // unused cells transparent
        }
    }
    Image atlas;
    int c, r;
    std::vector<Cell> cells;
    std::string error;
    EXPECT_FALSE(packAtlas({}, atlas, c, r, cells, error));
    EXPECT_FALSE(packAtlas(std::vector<Image>(5, Image::make(2048, 2048, 4, 8)), atlas, c, r, cells, error));   // 6144x4096
    EXPECT_NE(error.find("16777216"), std::string::npos);
}

TEST(SpriteNormalsAtlas, EncodePngWithoutAncillaryChunks) {
    Image rgba = Image::make(3, 2, 4, 8);
    for (size_t i = 0; i < rgba.data.size(); i++) rgba.data[i] = static_cast<uint8_t>(i * 11);
    Image grey = Image::make(3, 2, 1, 16);
    for (size_t i = 0; i < grey.data.size(); i++) grey.data[i] = static_cast<uint8_t>(i * 23);
    for (const Image& image : {rgba, grey}) {
        Bytes png;
        std::string error;
        ASSERT_TRUE(encodePng(image, png, error)) << error;
        for (const char* ancillary : {"gAMA", "sRGB", "iCCP"}) EXPECT_EQ(std::search(png.begin(), png.end(), ancillary, ancillary + 4), png.end());
        texsynth::Image back;
        ASSERT_TRUE(texsynth::loadImageFromMemory(png.data(), png.size(), back, error)) << error;
        EXPECT_EQ(back.channels, image.channels);
        EXPECT_EQ(back.bitDepth, image.bitDepth);
        EXPECT_EQ(back.data, image.data);
    }
    Bytes png;
    std::string error;
    EXPECT_FALSE(encodePng(Image::make(2, 2, 3, 8), png, error));
    EXPECT_FALSE(error.empty());
}

// The upstream binary's colour atlases and frames JSON of the GIF/APNG fixtures
TEST(SpriteNormalsAtlas, MatchesUpstreamAtlases) {
    for (const char* name : {"walk", "spin", "twoframes"}) {
        SCOPED_TRACE(name);
        const std::string input = std::string(name) + (std::string(name) == "twoframes" ? ".png" : ".gif");
        Source s;
        std::vector<Problem> problems;
        ASSERT_TRUE(loadSource(kFixtures / "inputs" / input, s, problems)) << (problems.empty() ? "" : problems[0].message);
        texsynth::Image expected;
        std::string error;
        ASSERT_TRUE(texsynth::loadImage(kFixtures / "expected" / (std::string(name) + "_n.color.png"), expected, error)) << error;
        EXPECT_EQ(s.color.data, texsynth::toRgba8(expected).data);
        std::ifstream in(kFixtures / "expected" / (std::string(name) + "_n.frames.json"));
        const nlohmann::json frames = nlohmann::json::parse(in)["animation"];
        EXPECT_EQ(s.columns, frames["columns"].get<int>());
        EXPECT_EQ(s.frameCount, frames["frame_count"].get<int>());
        EXPECT_EQ(s.frameWidth, frames["frame_width"].get<int>());
        ASSERT_EQ(s.delays.size(), frames["delays_ms"].size());
        for (size_t i = 0; i < s.delays.size(); i++) {
            const uint64_t n = frames["delays_ms"][i][0], d = frames["delays_ms"][i][1];
            EXPECT_EQ(static_cast<uint64_t>(s.delays[i].numerator) * d, n * s.delays[i].denominator) << i;
        }
    }
}

// ---------------------------------------------------------------- hostile files (fix round 1)

TEST(SpriteNormalsHostile, GifFrameLargerThanTheLimitIsRefusedBeforeDecoding) {
    // a 1x1 canvas with a 65535x65535 image descriptor: refused from the descriptor
    Bytes bytes = gif(1, 1, {fill(0, 0, 1, 1, 1, 0, 1)});
    for (size_t i = 0; i + 16 < bytes.size(); i++) {
        if (bytes[i] == 0x21 && bytes[i + 1] == 0xF9 && bytes[i + 2] == 4) {   // the GCE, then the descriptor
            ASSERT_EQ(bytes[i + 8], 0x2C);
            bytes[i + 13] = bytes[i + 14] = bytes[i + 15] = bytes[i + 16] = 0xFF;
            break;
        }
    }
    const Problem p = refused(bytes);
    EXPECT_EQ(p.kind, "too_large");
    EXPECT_NE(p.message.find("frame 1 is 65535x65535 (4294836225 pixels)"), std::string::npos) << p.message;
    EXPECT_NE(p.message.find("16777216"), std::string::npos);
}

TEST(SpriteNormalsHostile, GifFramesAreClippedToTheCanvas) {
    // an 8x8 canvas; a 4x4 frame at (6, 6) with a distinct index per pixel: only its top-left
    // 2x2 shows, the codes of the rest are consumed
    GifFrame f = fill(6, 6, 4, 4, 0, 1, 1);
    for (int i = 0; i < 16; i++) f.indices[static_cast<size_t>(i)] = static_cast<uint8_t>(10 + i);
    GifFrame inter = f;   // the same, interlaced (rows 0, 2, 1, 3 in the file... passes 0 / 2 / 1,3)
    inter.interlaced = true;
    inter.indices.clear();
    for (int y : {0, 2, 1, 3})
        for (int x = 0; x < 4; x++) inter.indices.push_back(static_cast<uint8_t>(10 + y * 4 + x));
    GifFrame outside = fill(9, 0, 3, 3, 3, 1, 1);   // entirely right of the canvas: draws nothing
    for (const GifFrame& frame : {f, inter}) {
        const Source s = load(gif(8, 8, {fill(0, 0, 8, 8, 1, 1, 1), frame, outside}));
        ASSERT_EQ(s.frameCount, 3);
        const int cx = 8, cy = 0;   // frame 2's atlas cell (2 columns)
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++) {
                const uint8_t v = static_cast<uint8_t>(10 + y * 4 + x);
                EXPECT_EQ(px(s.color, cx + 6 + x, cy + 6 + y), (C{v, v, v, 255})) << x << "," << y;
            }
        EXPECT_EQ(px(s.color, cx + 5, cy + 5), kRed);
        EXPECT_EQ(crop3(s, 1), crop3(s, 2));   // frame 3 = frame 2 (the outside frame drew nothing)
    }
}

TEST(SpriteNormalsHostile, AtlasLimitBeforeTheFramesThatBreakIt) {
    // 2100x2100 canvas: 3 frames decode to 13.2 M pixels, but their 2x2 atlas would be 17.6 M
    const Problem p = refused(gif(2100, 2100, {fill(0, 0, 1, 1, 1, 1, 1), fill(0, 0, 1, 1, 2, 1, 1), fill(0, 0, 1, 1, 3, 1, 1)}));
    EXPECT_EQ(p.kind, "too_large");
    EXPECT_NE(p.message.find("with frame 3 the atlas would hold 17640000 pixels"), std::string::npos) << p.message;
    // APNG: from acTL and IHDR, before any frame is looked at
    const Problem a = refused(apng(2100, 2100, {rect(0, 0, 1, 1, kRed), rect(0, 0, 1, 1, kRed), rect(0, 0, 1, 1, kRed)}));
    EXPECT_EQ(a.kind, "too_large");
    EXPECT_NE(a.message.find("atlas"), std::string::npos) << a.message;
    EXPECT_NE(a.message.find("17640000"), std::string::npos) << a.message;
}

TEST(SpriteNormalsHostile, ApngFrameCountAndDeflateBombs) {
    // more fcTLs than acTL declares
    ApngOptions o;
    o.declaredFrames = 1;
    Problem p = refused(apng(2, 2, {rect(0, 0, 2, 2, kRed), rect(0, 0, 1, 1, kGreen)}, o));
    EXPECT_EQ(p.kind, "invalid_source");
    EXPECT_NE(p.message.find("more fcTL chunks than the 1 frames"), std::string::npos) << p.message;
    // a 1x1 frame whose data inflates to 100000 bytes (5 are needed)
    ApngFrame bomb = rect(0, 0, 1, 1, kGreen);
    bomb.zlibData = zlibStored(Bytes(4 << 20, 0));
    p = refused(apng(1, 1, {rect(0, 0, 1, 1, kRed), bomb}));
    EXPECT_EQ(p.kind, "invalid_source");
    EXPECT_NE(p.message.find("frame 2 of the APNG's image data is corrupt, ends before the 5 bytes its size needs or inflates past 1048581 bytes"),
              std::string::npos)
        << p.message;
    // the same in a still PNG (and in the default image of an APNG frame 0)
    Bytes still = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    Bytes ihdr;
    be32(ihdr, 1);
    be32(ihdr, 1);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(still, "IHDR", ihdr);
    chunk(still, "IDAT", zlibStored(Bytes(4 << 20, 0)));
    chunk(still, "IEND", {});
    p = refused(still);
    EXPECT_EQ(p.kind, "invalid_source");
    EXPECT_NE(p.message.find("inflate"), std::string::npos) << p.message;
    // Adam7 sizes are exact: an interlaced 16-bit still loads
    texsynth::Image wide = texsynth::Image::make(9, 7, 4, 16);
    Bytes png;
    ASSERT_TRUE(texsynth::encodePng(wide, png));
    EXPECT_EQ(load(png).color.width, 9);
}

TEST(SpriteNormalsHostile, ChunkCountIsBounded) {
    Bytes b = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    Bytes ihdr;
    be32(ihdr, 1);
    be32(ihdr, 1);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(b, "IHDR", ihdr);
    Bytes empty;
    chunk(empty, "tEXt", {});
    for (int i = 0; i < (1 << 20); i++) b.insert(b.end(), empty.begin(), empty.end());
    chunk(b, "IDAT", scanlines(1, 1, {1, 2, 3, 4}, 4));
    chunk(b, "IEND", {});
    const Problem p = refused(b);
    EXPECT_EQ(p.kind, "too_large");
    EXPECT_NE(p.message.find("1048576 chunks"), std::string::npos) << p.message;
}

TEST(SpriteNormalsHostile, InflatedBytesBeyondTheNeededAreIgnored) {
    // a 1x1 RGBA PNG whose IDAT inflates to its 5 bytes plus 300 trailing ones: stb accepts it, so do we
    Bytes raw = {0, 10, 20, 30, 40};
    raw.resize(305, 7);
    Bytes still = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    Bytes ihdr;
    be32(ihdr, 1);
    be32(ihdr, 1);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(still, "IHDR", ihdr);
    chunk(still, "IDAT", zlibStored(raw));
    chunk(still, "IEND", {});
    EXPECT_EQ(px(load(still).color, 0, 0), (C{10, 20, 30, 40}));
    // the same in an APNG frame
    ApngFrame extra = rect(0, 0, 1, 1, kGreen);
    extra.zlibData = zlibStored(raw);
    EXPECT_EQ(px(load(apng(1, 1, {rect(0, 0, 1, 1, kRed), extra})).color, 1, 0), (C{10, 20, 30, 40}));
    // too short is still an error
    ApngFrame shortFrame = rect(0, 0, 1, 1, kGreen);
    shortFrame.zlibData = zlibStored({0, 1, 2});
    EXPECT_EQ(refused(apng(1, 1, {rect(0, 0, 1, 1, kRed), shortFrame})).kind, "invalid_source");
}

TEST(SpriteNormalsHostile, HugeDescriptorsOnATinyCanvasAreBounded) {
    // a 1x1 canvas, frames with 4096x4096 descriptors (16.7 M pixels of LZW work each, clipped to
    // one pixel): the first two decode, the third passes the cumulative descriptor limit and is
    // refused before its data is looked at
    GifFrame big = fill(0, 0, 4096, 4096, 1, 1, 1);
    big.lzwData = lzw(big.indices, 8);
    big.indices.clear();
    GifFrame garbage = fill(0, 0, 4096, 4096, 1, 1, 1);
    garbage.lzwData = {0, 0};   // a code size of 0: refused if it were decoded
    garbage.indices.clear();
    std::vector<GifFrame> frames = {big, big};
    for (int i = 0; i < 254; i++) frames.push_back(garbage);
    const Bytes bytes = gif(1, 1, frames);
    const auto start = std::chrono::steady_clock::now();
    const Problem p = refused(bytes);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(p.kind, "too_large");
    EXPECT_NE(p.message.find("the image descriptors of frames 1-3 cover 50331648 pixels; at most 33554432 are decoded"), std::string::npos)
        << p.message;
    // the message proves the bound (the third frame refused, the rest never looked at); the clock
    // only catches a runaway (unbounded work takes minutes), with room for a loaded CI host
#ifdef NDEBUG
    EXPECT_LT(seconds, 10.0);
#endif
    // probing applies the same limit without decoding
    const fs::path dir = tempDir();
    writeFile(dir / "tiny.gif", bytes);
    SourceInfo info;
    std::vector<Problem> problems;
    EXPECT_FALSE(probeSource(dir / "tiny.gif", info, problems));
    EXPECT_EQ(problems.at(0).kind, "too_large");
    fs::remove_all(dir);
    // frames slightly past their canvas keep decoding: 3 frames of 10x10 on an 8x8 canvas
    const Source ok = load(gif(8, 8, {fill(0, 0, 10, 10, 1, 1, 1), fill(0, 0, 10, 10, 2, 1, 1), fill(0, 0, 10, 10, 3, 1, 1)}));
    EXPECT_EQ(ok.frameCount, 3);
    EXPECT_EQ(px(ok.color, 8 + 7, 7), kGreen);
}
