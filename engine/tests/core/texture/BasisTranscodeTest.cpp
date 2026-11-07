// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The trimmed Basis Universal transcoder of the web runtime (spec S14 R21, deviation 23): the
// ETC1S -> BC1 tables it computes at init are upstream's compiled-in tables entry for entry. The
// transcodes themselves are compared with upstream's full transcoder by the editor's
// TextureWebTranscode tests (they need the encoder for their input).

#include "texture/BasisTranscode.h"
#include "jasper_transcoder_tables.h"

#include <gtest/gtest.h>

#include <cstring>

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
