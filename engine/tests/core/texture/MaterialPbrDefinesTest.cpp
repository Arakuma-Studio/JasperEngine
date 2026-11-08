// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T3 R54: the built-in Lit's define bits are 64 wide (AC48).

#include "component/MeshComponent.h"
#include "texture/MaterialPbr.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <type_traits>
#include <vector>

using namespace jasperengine;

TEST(MaterialPbrDefines, BitsAreSixtyFourWide){
    static_assert(std::is_same_v<decltype(MaterialPbr::getDefineBit(std::string())), uint64_t>);
    static_assert(std::is_same_v<decltype(MaterialPbr::getDefineBits(std::vector<std::string>())), uint64_t>);
    static_assert(std::is_same_v<decltype(Submesh::litDefineBits), uint64_t>);

    // every entry round-trips through the three functions; the codes a variant is named by do not change
    int entries = 0;
    for (int bit = 0; bit < 64; bit++){
        const std::vector<std::string> defines = MaterialPbr::getDefinesFromBits(uint64_t(1) << bit);
        if (defines.empty()){
            continue;
        }
        entries++;
        ASSERT_EQ(defines.size(), 1u);
        EXPECT_EQ(MaterialPbr::getDefineBit(defines[0]), uint64_t(1) << bit) << defines[0];
        EXPECT_EQ(MaterialPbr::getDefineBits(defines), uint64_t(1) << bit) << defines[0];
        EXPECT_TRUE(MaterialPbr::isBuiltinLitDefine(defines[0]));
    }
    EXPECT_GE(entries, 18);

    // all together
    uint64_t all = 0;
    for (int bit = 0; bit < entries; bit++){
        all |= uint64_t(1) << bit;
    }
    std::vector<std::string> defines = MaterialPbr::getDefinesFromBits(all);
    EXPECT_EQ(static_cast<int>(defines.size()), entries);
    EXPECT_EQ(MaterialPbr::getDefineBits(defines), all);
    EXPECT_TRUE(std::is_sorted(defines.begin(), defines.end()));
    EXPECT_EQ(MaterialPbr::getDefineBit("NOT_A_DEFINE"), 0u);
}
