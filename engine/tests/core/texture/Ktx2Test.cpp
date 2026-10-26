// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The KTX2 container of cooked textures (spec S14 AC7): write/read round trips of every
// GpuTextureFormat, cubemaps and key/values, ranged reads that never touch skipped levels, and
// malformed files failing cleanly (a 10k-file mutation fuzz, run under ASan in CI).

#include "texture/Ktx2.h"
#include "texture/TextureFormat.h"

#include "zstd.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace {

    bool zstdCompress(std::span<const uint8_t> in, std::vector<uint8_t>& out, int level, void*){
        out.resize(ZSTD_compressBound(in.size()));
        const size_t written = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), level);
        if (ZSTD_isError(written))
            return false;
        out.resize(written);
        return true;
    }

    // deterministic bytes for every level of a width x height x faces texture of `format`
    std::vector<std::vector<uint8_t>> makeLevels(GpuTextureFormat format, int width, int height, int faces, int levels, uint32_t seed){
        std::mt19937 random(seed);
        std::vector<std::vector<uint8_t>> out(levels);
        for (int level = 0; level < levels; level++){
            out[level].resize(gpuFormatLevelBytes(format, mipDimension(width, level), mipDimension(height, level)) * faces);
            for (uint8_t& b : out[level])
                b = (uint8_t)(random() & 0xFF);
        }
        return out;
    }

    Ktx2::WriteDesc describe(GpuTextureFormat format, int width, int height, int faces, const std::vector<std::vector<uint8_t>>& levels){
        Ktx2::WriteDesc desc;
        desc.format = format;
        desc.srgb = format == GpuTextureFormat::RGBA8 || format == GpuTextureFormat::BC7;
        desc.width = (uint32_t)width;
        desc.height = (uint32_t)height;
        desc.faces = (uint32_t)faces;
        for (const auto& level : levels)
            desc.levels.push_back(level);
        desc.keyValues["JE.logical"] = std::to_string(width) + "," + std::to_string(height);
        desc.keyValues["JE.type"] = "default";
        return desc;
    }

    // records every byte range a read asks for
    class SpySource : public Ktx2ByteSource {
    public:
        explicit SpySource(const std::vector<uint8_t>& bytes): memory(bytes){}
        uint64_t size() const override { return memory.size(); }
        bool read(uint64_t offset, size_t size, std::vector<uint8_t>& out) override{
            ranges.emplace_back(offset, offset + size);
            return memory.read(offset, size, out);
        }
        Ktx2MemorySource memory;
        std::vector<std::pair<uint64_t, uint64_t>> ranges;
    };

}

TEST(Ktx2, RoundTripsEveryFormat){
    for (size_t f = 0; f < kGpuTextureFormatCount; f++){
        const GpuTextureFormat format = (GpuTextureFormat)f;
        for (int zstd : {0, 3}){
            SCOPED_TRACE(std::string(gpuFormatName(format)) + (zstd ? " zstd" : " raw"));
            // odd sizes: partial blocks on every level
            const int width = 37, height = 21;
            const int levels = fullMipCount(width, height);
            const auto bytes = makeLevels(format, width, height, 1, levels, (uint32_t)f + 1);
            std::vector<uint8_t> file;
            std::string error;
            ASSERT_TRUE(Ktx2::write(describe(format, width, height, 1, bytes), file, error, zstd, zstdCompress)) << error;

            Ktx2MemorySource source(file);
            Ktx2Image image;
            ASSERT_TRUE(Ktx2::read(source, 0, image, error)) << error;
            EXPECT_EQ(image.info.codec, Ktx2Codec::Raw);
            EXPECT_EQ(image.info.format, format);
            EXPECT_EQ(image.info.width, (uint32_t)width);
            EXPECT_EQ(image.info.height, (uint32_t)height);
            EXPECT_EQ(image.info.levelCount, (uint32_t)levels);
            EXPECT_EQ(image.info.supercompression, zstd ? Ktx2Supercompression::Zstd : Ktx2Supercompression::None);
            ASSERT_EQ(image.levels.size(), (size_t)levels);
            for (int level = 0; level < levels; level++)
                EXPECT_EQ(image.levels[level], bytes[level]) << "level " << level;
            EXPECT_EQ(image.info.keyValues["JE.logical"], "37,21");
        }
    }
}

TEST(Ktx2, SrgbTravelsInTheDfd){
    const auto bytes = makeLevels(GpuTextureFormat::BC7, 16, 16, 1, 1, 7);
    for (bool srgb : {false, true}){
        Ktx2::WriteDesc desc = describe(GpuTextureFormat::BC7, 16, 16, 1, bytes);
        desc.srgb = srgb;
        std::vector<uint8_t> file;
        std::string error;
        ASSERT_TRUE(Ktx2::write(desc, file, error));
        Ktx2Info info;
        ASSERT_TRUE(Ktx2::parseInfo(file, file.size(), info, error)) << error;
        EXPECT_EQ(info.srgb, srgb);
        EXPECT_EQ(info.vkFormat, gpuFormatToVkFormat(GpuTextureFormat::BC7, srgb));
    }
    // linear data stays linear whatever the flag says
    // (the descriptor's spans point into these: they outlive the write)
    const std::vector<std::vector<uint8_t>> bc5Levels = makeLevels(GpuTextureFormat::BC5, 16, 16, 1, 1, 8);
    Ktx2::WriteDesc desc = describe(GpuTextureFormat::BC5, 16, 16, 1, bc5Levels);
    desc.srgb = true;
    std::vector<uint8_t> file;
    std::string error;
    ASSERT_TRUE(Ktx2::write(desc, file, error));
    Ktx2Info info;
    ASSERT_TRUE(Ktx2::parseInfo(file, file.size(), info, error));
    EXPECT_FALSE(info.srgb);
}

TEST(Ktx2, RoundTripsCubemaps){
    const int size = 32;
    const int levels = fullMipCount(size, size);
    const auto bytes = makeLevels(GpuTextureFormat::ASTC_6x6, size, size, 6, levels, 11);
    std::vector<uint8_t> file;
    std::string error;
    ASSERT_TRUE(Ktx2::write(describe(GpuTextureFormat::ASTC_6x6, size, size, 6, bytes), file, error, 9, zstdCompress)) << error;
    Ktx2MemorySource source(file);
    Ktx2Image image;
    ASSERT_TRUE(Ktx2::read(source, 0, image, error)) << error;
    EXPECT_EQ(image.info.faces, 6u);
    for (int level = 0; level < levels; level++)
        EXPECT_EQ(image.levels[level], bytes[level]);
}

TEST(Ktx2, KeyValuesRoundTripAndRewrite){
    const auto bytes = makeLevels(GpuTextureFormat::RGBA8, 8, 8, 1, 4, 3);
    Ktx2::WriteDesc desc = describe(GpuTextureFormat::RGBA8, 8, 8, 1, bytes);
    desc.keyValues["JE.flags"] = "alpha,coverage";
    desc.keyValues["KTXwriter"] = "JasperEngine";
    std::vector<uint8_t> file;
    std::string error;
    ASSERT_TRUE(Ktx2::write(desc, file, error, 5, zstdCompress));
    Ktx2Info info;
    ASSERT_TRUE(Ktx2::parseInfo(file, file.size(), info, error));
    EXPECT_EQ(info.keyValues["JE.flags"], "alpha,coverage");
    EXPECT_EQ(info.keyValues["KTXwriter"], "JasperEngine");
    EXPECT_EQ(info.keyValues["JE.type"], "default");

    std::vector<uint8_t> rewritten;
    ASSERT_TRUE(Ktx2::rewriteKeyValues(file, {{"JE.flags", "normalRG"}, {"JE.extra", "1"}}, rewritten, error)) << error;
    Ktx2MemorySource source(rewritten);
    Ktx2Image image;
    ASSERT_TRUE(Ktx2::read(source, 0, image, error)) << error;
    EXPECT_EQ(image.info.keyValues["JE.flags"], "normalRG");
    EXPECT_EQ(image.info.keyValues["JE.extra"], "1");
    EXPECT_EQ(image.info.keyValues["KTXwriter"], "JasperEngine");
    for (size_t level = 0; level < bytes.size(); level++)
        EXPECT_EQ(image.levels[level], bytes[level]);
}

TEST(Ktx2, LevelsAreStoredSmallestFirst){
    const auto bytes = makeLevels(GpuTextureFormat::BC1, 64, 64, 1, 7, 5);
    std::vector<uint8_t> file;
    std::string error;
    ASSERT_TRUE(Ktx2::write(describe(GpuTextureFormat::BC1, 64, 64, 1, bytes), file, error));
    Ktx2Info info;
    ASSERT_TRUE(Ktx2::parseInfo(file, file.size(), info, error));
    for (uint32_t level = 1; level < info.levelCount; level++)
        EXPECT_LT(info.levels[level].byteOffset, info.levels[level - 1].byteOffset);
    // the largest level ends the file
    EXPECT_EQ(info.levels[0].byteOffset + info.levels[0].byteLength, file.size());
}

TEST(Ktx2, ReadingFromAMipLimitTouchesOnlyTheKeptLevels){
    for (int zstd : {0, 6}){
        const auto bytes = makeLevels(GpuTextureFormat::BC7, 256, 128, 1, 9, 9);
        std::vector<uint8_t> file;
        std::string error;
        ASSERT_TRUE(Ktx2::write(describe(GpuTextureFormat::BC7, 256, 128, 1, bytes), file, error, zstd, zstdCompress));
        Ktx2Info info;
        ASSERT_TRUE(Ktx2::parseInfo(file, file.size(), info, error));

        SpySource spy(file);
        Ktx2Image image;
        const uint32_t firstLevel = 2;
        ASSERT_TRUE(Ktx2::read(spy, firstLevel, image, error)) << error;
        EXPECT_EQ(image.firstLevel, firstLevel);
        ASSERT_EQ(image.levels.size(), info.levelCount - firstLevel);
        for (size_t i = 0; i < image.levels.size(); i++)
            EXPECT_EQ(image.levels[i], bytes[firstLevel + i]);
        // no byte of levels 0 and 1 was asked for
        for (const auto& [begin, end] : spy.ranges){
            for (uint32_t skipped = 0; skipped < firstLevel; skipped++){
                const uint64_t levelBegin = info.levels[skipped].byteOffset;
                const uint64_t levelEnd = levelBegin + info.levels[skipped].byteLength;
                EXPECT_TRUE(end <= levelBegin || begin >= levelEnd)
                    << "read [" << begin << ", " << end << ") overlaps level " << skipped;
            }
        }
    }
}

TEST(Ktx2, PrefixSourceLoadsWhatItHolds){
    const auto bytes = makeLevels(GpuTextureFormat::RGBA8, 128, 128, 1, 8, 13);
    std::vector<uint8_t> file;
    std::string error;
    ASSERT_TRUE(Ktx2::write(describe(GpuTextureFormat::RGBA8, 128, 128, 1, bytes), file, error));
    Ktx2Info info;
    ASSERT_TRUE(Ktx2::parseInfo(file, file.size(), info, error));
    // the bytes up to the end of level 1: levels 1..7 load, level 0 does not
    const uint64_t end = info.levels[1].byteOffset + info.levels[1].byteLength;
    std::vector<uint8_t> prefix(file.begin(), file.begin() + (ptrdiff_t)end);
    Ktx2PrefixSource source(prefix, file.size());
    Ktx2Image image;
    EXPECT_TRUE(Ktx2::read(source, 1, image, error)) << error;
    EXPECT_EQ(image.levels[0], bytes[1]);
    EXPECT_FALSE(Ktx2::read(source, 0, image, error));
}

TEST(Ktx2, MalformedFilesFailCleanly){
    const auto bytes = makeLevels(GpuTextureFormat::BC3, 64, 32, 1, 7, 17);
    std::vector<uint8_t> file;
    std::string error;
    ASSERT_TRUE(Ktx2::write(describe(GpuTextureFormat::BC3, 64, 32, 1, bytes), file, error, 4, zstdCompress));

    auto fails = [](std::vector<uint8_t> broken){
        Ktx2MemorySource source(broken);
        Ktx2Image image;
        std::string why;
        const bool ok = Ktx2::read(source, 0, image, why);
        return !ok && !why.empty();
    };

    // truncated anywhere
    for (size_t length = 0; length < file.size(); length += 7)
        EXPECT_TRUE(fails(std::vector<uint8_t>(file.begin(), file.begin() + (ptrdiff_t)length))) << "length " << length;

    // a level index pointing past the end, an offset + length that overflows, a huge
    // uncompressed size
    auto patch64 = [&](size_t at, uint64_t value){
        std::vector<uint8_t> broken = file;
        for (int i = 0; i < 8; i++)
            broken[at + i] = (uint8_t)(value >> (8 * i));
        return broken;
    };
    const size_t level0 = Ktx2::kHeaderSize;
    EXPECT_TRUE(fails(patch64(level0, file.size() + 16)));
    EXPECT_TRUE(fails(patch64(level0 + 8, ~0ull - 4)));
    EXPECT_TRUE(fails(patch64(level0 + 16, 1ull << 62)));
    EXPECT_TRUE(fails(patch64(level0 + 16, 12)));

    // a DFD length that does not match, six faces of a 2:1 texture, a bad face count, an
    // unknown vkFormat, zlib supercompression
    auto patch32 = [&](size_t at, uint32_t value){
        std::vector<uint8_t> broken = file;
        for (int i = 0; i < 4; i++)
            broken[at + i] = (uint8_t)(value >> (8 * i));
        return broken;
    };
    EXPECT_TRUE(fails(patch32(52, 7)));
    EXPECT_TRUE(fails(patch32(36, 3)));
    EXPECT_TRUE(fails(patch32(12, 999999)));
    EXPECT_TRUE(fails(patch32(44, 3)));
    EXPECT_TRUE(fails(patch32(40, 40)));
    // not a KTX2 file at all
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 0};
    EXPECT_TRUE(fails(png));
}

TEST(Ktx2, FuzzedFilesNeverCrash){
    std::vector<std::vector<uint8_t>> seeds;
    std::string error;
    for (GpuTextureFormat format : {GpuTextureFormat::RGBA8, GpuTextureFormat::BC7, GpuTextureFormat::ASTC_8x8, GpuTextureFormat::R16}){
        for (int zstd : {0, 2}){
            for (int faces : {1, 6}){
                const auto bytes = makeLevels(format, 24, 24, faces, fullMipCount(24, 24), (uint32_t)format * 7 + (uint32_t)zstd);
                std::vector<uint8_t> file;
                ASSERT_TRUE(Ktx2::write(describe(format, 24, 24, faces, bytes), file, error, zstd, zstdCompress));
                seeds.push_back(std::move(file));
            }
        }
    }
    std::mt19937 random(1234);
    size_t parsed = 0;
    for (int iteration = 0; iteration < 10000; iteration++){
        std::vector<uint8_t> file = seeds[(size_t)iteration % seeds.size()];
        // a few byte flips, an occasional cut or growth
        const int flips = 1 + (int)(random() % 8);
        for (int i = 0; i < flips; i++){
            const size_t at = random() % file.size();
            // most flips hit the metadata: that is where the parser decides things
            const size_t where = (random() % 4 == 0) ? at : at % std::min<size_t>(file.size(), 256);
            file[where] = (uint8_t)random();
        }
        if (random() % 5 == 0)
            file.resize(random() % (file.size() + 1));
        if (random() % 13 == 0)
            file.resize(file.size() + random() % 64, (uint8_t)random());
        Ktx2MemorySource source(file);
        Ktx2Image image;
        std::string why;
        if (!Ktx2::read(source, random() % 3, image, why)){
            EXPECT_FALSE(why.empty());
            continue;
        }
        parsed++;
        // a file it accepts keeps levels as large as its index says
        for (size_t i = 0; i < image.levels.size(); i++){
            const uint32_t level = image.firstLevel + (uint32_t)i;
            ASSERT_LT(level, image.info.levelCount);
            if (image.info.codec == Ktx2Codec::Raw){
                EXPECT_EQ(image.levels[i].size(), gpuFormatLevelBytes(image.info.format, (int)image.info.levelWidth(level),
                                                                      (int)image.info.levelHeight(level)) * image.info.faces);
            }
        }
    }
    // the fuzz kept some files intact enough to load (it is not only rejecting garbage)
    EXPECT_GT(parsed, 0u);
}

TEST(Ktx2, WritesUastcFilesTheReaderRecognises){
    // UASTC LDR blocks: 16 bytes per 4x4 block, vkFormat UNDEFINED, the UASTC DFD model
    const int width = 20, height = 12;
    std::vector<std::vector<uint8_t>> levels = makeLevels(GpuTextureFormat::ASTC_4x4, width, height, 1, fullMipCount(width, height), 21);
    Ktx2::WriteDesc desc;
    desc.codec = Ktx2Codec::UASTC;
    desc.uastcChannels = Ktx2::UastcChannels::RG;
    desc.width = width;
    desc.height = height;
    for (const auto& level : levels)
        desc.levels.push_back(level);
    std::vector<uint8_t> file;
    std::string error;
    ASSERT_TRUE(Ktx2::write(desc, file, error, 9, zstdCompress)) << error;
    Ktx2MemorySource source(file);
    Ktx2Image image;
    ASSERT_TRUE(Ktx2::read(source, 0, image, error)) << error;
    EXPECT_EQ(image.info.codec, Ktx2Codec::UASTC);
    EXPECT_EQ(image.info.vkFormat, 0u);
    EXPECT_EQ(image.info.dfdColorModel, 166u);
    for (size_t level = 0; level < levels.size(); level++)
        EXPECT_EQ(image.levels[level], levels[level]);
    // the DFD names the RG channels (the transcoder's normal-map cue)
    const std::vector<uint8_t> dfd = Ktx2::makeUastcDfd(Ktx2::UastcChannels::RG, false, true);
    EXPECT_EQ(dfd[4 + 24 + 3], 6);
    EXPECT_EQ(dfd[4 + 16], 0);      // bytesPlane0: supercompressed
}
