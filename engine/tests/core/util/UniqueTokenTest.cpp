// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/UniqueToken.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

using namespace jasperengine;

TEST(UniqueToken, GetNeverRepeats){
    std::set<std::string> tokens;
    for (int i = 0; i < 1000; i++)
        tokens.insert(UniqueToken::get());
    EXPECT_EQ(tokens.size(), 1000u);
}
