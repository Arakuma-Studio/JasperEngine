// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The trimmed Basis Universal transcoder of the web runtime (spec S14 R21, deviation 23): the
// ETC1S -> BC1 tables it computes at init are upstream's compiled-in tables entry for entry. The
// transcodes themselves are compared with upstream's full transcoder by the editor's
// TextureWebTranscode tests (they need the encoder for their input).

#include "texture/BasisTranscode.h"
#include "texture/Ktx2.h"

#include "basisu_transcoder.h"
#include "basisu_transcoder_uastc.h"
#include "jasper_transcoder_tables.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    struct Etc1sToBc1Solution {
        uint8_t lo;
        uint8_t hi;
        uint16_t err;
    };

    // upstream's tables, as basisu_transcoder.cpp compiles them in without
    // JASPER_BASISD_COMPUTE_DXT1_TABLES
    const Etc1sToBc1Solution kUpstreamBc1Table5[] = {
#include "basisu_transcoder_tables_dxt1_5.inc"
    };
    const Etc1sToBc1Solution kUpstreamBc1Table6[] = {
#include "basisu_transcoder_tables_dxt1_6.inc"
    };

    static_assert(sizeof(kUpstreamBc1Table5) == 15360 * sizeof(Etc1sToBc1Solution), "[inten 8][g 32][ranges 6][mappings 10]");
    static_assert(sizeof(kUpstreamBc1Table6) == sizeof(kUpstreamBc1Table5));

}

TEST(TextureBasisTranscode, Etc1sToBc1TablesMatchUpstream){
    ASSERT_TRUE(BasisTranscode::isAvailable());
    BasisTranscode::warmUp();

    for (uint32_t bits : {5u, 6u}){
        SCOPED_TRACE(bits);
        const Etc1sToBc1Solution* upstream = bits == 5 ? kUpstreamBc1Table5 : kUpstreamBc1Table6;
        uint32_t bytes = 0;
        const auto* computed = static_cast<const Etc1sToBc1Solution*>(basist::jasper_etc1_to_dxt1_table(bits, bytes));
        ASSERT_NE(computed, nullptr);
        ASSERT_EQ(bytes, sizeof(kUpstreamBc1Table5));
        int different = 0;
        for (size_t i = 0; i < 15360; i++){
            if (std::memcmp(&computed[i], &upstream[i], sizeof(Etc1sToBc1Solution)) != 0 && different++ < 5){
                ADD_FAILURE() << "entry " << i << ": computed {" << (int)computed[i].lo << "," << (int)computed[i].hi << ","
                              << computed[i].err << "}, upstream {" << (int)upstream[i].lo << "," << (int)upstream[i].hi << ","
                              << upstream[i].err << "}";
            }
        }
        EXPECT_EQ(different, 0);
    }
}

// R19's web row measured natively (the web figures come from the texture-formats project's
// ?transcode_bench=1, docs/textures.md): the bench's 2048² UASTC chain, tiled from its 512² cook,
// to BC7 and ASTC 4x4, and the stages of a UASTC -> BC7 block. Printed, not gated.
TEST(TextureBasisTranscodeBench, Uastc2048Chain){
    ASSERT_TRUE(BasisTranscode::isAvailable());
    const std::string path = std::string(JASPER_TEST_SOURCE_DIR) + "/../../../tools/test-projects/texture-formats/assets/bench/uastc-photo-512.ktx2";
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good()) << path;
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Ktx2MemorySource memory(bytes);
    Ktx2Image source;
    std::string error;
    ASSERT_TRUE(Ktx2::read(memory, 0, source, error)) << error;
    ASSERT_EQ(source.info.codec, Ktx2Codec::UASTC);
    const uint32_t sourceBlocks = source.info.width / 4;

    Ktx2Image chain = source;
    chain.info.width = chain.info.height = 2048;
    chain.info.levelCount = 12;
    chain.levels.clear();
    for (uint32_t size = 2048; size > source.info.width; size /= 2){
        const uint32_t blocks = size / 4;
        std::vector<uint8_t> level((size_t)blocks * blocks * 16);
        for (uint32_t y = 0; y < blocks; y++)
            for (uint32_t x = 0; x < blocks; x++)
                std::memcpy(&level[((size_t)y * blocks + x) * 16], &source.levels[0][((size_t)(y % sourceBlocks) * sourceBlocks + x % sourceBlocks) * 16], 16);
        chain.levels.push_back(std::move(level));
    }
    for (const std::vector<uint8_t>& level : source.levels)
        chain.levels.push_back(level);
    chain.info.levels.assign(chain.levels.size(), Ktx2LevelIndex());
    const std::vector<uint8_t> level0 = chain.levels[0];

    BasisTranscode::Session session;
    ASSERT_TRUE(session.open(std::move(chain), error)) << error;
    auto medianMs = [](auto&& work){
        std::vector<double> runs;
        for (int run = 0; run < 7; run++){
            const auto start = std::chrono::steady_clock::now();
            work();
            runs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        std::sort(runs.begin(), runs.end());
        return runs[runs.size() / 2];
    };
    std::vector<uint8_t> out;
    for (GpuTextureFormat target : {GpuTextureFormat::BC7, GpuTextureFormat::ASTC_4x4}){
        const double ms = medianMs([&]{
            for (uint32_t level = 0; level < session.levelCount(); level++){
                EXPECT_TRUE(session.transcodeLevel(level, target, TextureContent::ColorOpaque, out, error)) << error;
            }
        });
        std::printf("[bench] native UASTC 2048 chain -> %s %.2f ms\n", gpuFormatName(target), ms);
    }

    // level 0's blocks, a stage at a time
    const size_t blocks = level0.size() / 16;
    const auto* uastc = reinterpret_cast<const basist::uastc_block*>(level0.data());
    std::vector<basist::unpacked_uastc_block> unpacked(blocks);
    std::vector<basist::bc7_optimization_results> results(blocks);
    std::vector<uint8_t> bc7(blocks * 16);
    const double unpackMs = medianMs([&]{
        for (size_t b = 0; b < blocks; b++)
            basist::unpack_uastc(uastc[b], unpacked[b], false, false);
    });
    const double convertMs = medianMs([&]{
        for (size_t b = 0; b < blocks; b++)
            basist::transcode_uastc_to_bc7(unpacked[b], results[b]);
    });
    const double encodeMs = medianMs([&]{
        for (size_t b = 0; b < blocks; b++)
            basist::encode_bc7_block(&bc7[b * 16], &results[b]);
    });
    std::printf("[bench] native UASTC 2048 level 0 -> BC7 stages: unpack %.2f ms, to BC7 %.2f ms, BC7 pack %.2f ms\n",
                unpackMs, convertMs, encodeMs);
}
