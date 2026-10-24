// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/Base64.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;

namespace {
    std::string encode(const std::string& text){
        return Base64::encode(reinterpret_cast<const unsigned char*>(text.data()), text.size());
    }

    std::string decode(const std::string& encoded){
        const std::vector<unsigned char> bytes = Base64::decode(encoded);
        return std::string(bytes.begin(), bytes.end());
    }
}

TEST(Base64, EncodesTheRfc4648Vectors){
    EXPECT_EQ(encode(""), "");
    EXPECT_EQ(encode("f"), "Zg==");
    EXPECT_EQ(encode("fo"), "Zm8=");
    EXPECT_EQ(encode("foo"), "Zm9v");
    EXPECT_EQ(encode("foob"), "Zm9vYg==");
    EXPECT_EQ(encode("fooba"), "Zm9vYmE=");
    EXPECT_EQ(encode("foobar"), "Zm9vYmFy");
}

TEST(Base64, DecodesTheRfc4648Vectors){
    EXPECT_EQ(decode("Zg=="), "f");
    EXPECT_EQ(decode("Zm8="), "fo");
    EXPECT_EQ(decode("Zm9vYmFy"), "foobar");
    EXPECT_EQ(decode(""), "");
}

TEST(Base64, RoundTripsEveryByteValue){
    std::vector<unsigned char> bytes;
    for (int i = 0; i < 256; i++)
        bytes.push_back(static_cast<unsigned char>(i));
    const std::string encoded = Base64::encode(bytes.data(), bytes.size());
    EXPECT_EQ(Base64::decode(encoded), bytes);
}

TEST(Base64, UsesThePlusAndSlashAlphabet){
    const unsigned char bytes[] = {0xFB, 0xFF, 0xBF};
    EXPECT_EQ(Base64::encode(bytes, sizeof(bytes)), "+/+/");
}

TEST(Base64, DecodingStopsAtTheFirstNonAlphabetCharacter){
    EXPECT_EQ(decode("Zm9v!YmFy"), "foo");
}
