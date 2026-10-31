// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "util/Sha256.h"
#include "htmlui/compat/Hash.h"
#include "TempDir.h"
#include "support/AllocationCounter.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {
    // FIPS 180-4 examples and the NIST long-message vector
    struct Vector {
        std::string message;
        const char* digest;
    };
}

TEST(Sha256, KnownVectors) {
    const Vector vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
        {std::string(1000000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
    };
    for (const Vector& vector : vectors) {
        EXPECT_EQ(sha256Hex(vector.message), vector.digest) << vector.message.size() << " bytes";
    }
}

// every split of a message into updates gives the one-shot digest (block boundaries)
TEST(Sha256, StreamingMatchesOneShot) {
    std::mt19937 random(30);
    std::string message(777, '\0');
    for (char& c : message) c = static_cast<char>(random());
    const std::string expected = sha256Hex(message);
    for (size_t first = 0; first <= message.size(); first += 13) {
        for (size_t second : {size_t(1), size_t(63), size_t(64), size_t(65), size_t(200)}) {
            Sha256 hash;
            hash.update(message.data(), std::min(first, message.size()));
            size_t at = std::min(first, message.size());
            while (at < message.size()) {
                const size_t take = std::min(second, message.size() - at);
                hash.update(message.data() + at, take);
                at += take;
            }
            ASSERT_EQ(hash.finishHex(), expected) << first << " then " << second;
        }
    }
}

// the hardware path (when this CPU has it) and the portable one agree on every length
TEST(Sha256, EveryLengthUpTo300MatchesTheCompatLayer) {
    std::string message;
    for (int length = 0; length <= 300; length++) {
        EXPECT_EQ(sha256Hex(message), htmlui::compat::sha256Hex(message)) << length;
        message += static_cast<char>('a' + length % 26);
    }
}

TEST(Sha256, FileDigestAndSize) {
    test::TempDir dir;
    const std::string content(3 * 1024 * 1024 + 17, 'x');
    const auto path = dir.write("big.bin", content);
    std::string hex;
    uint64_t size = 0;
    ASSERT_TRUE(sha256File(path, hex, nullptr, &size));
    EXPECT_EQ(hex, sha256Hex(content));
    EXPECT_EQ(size, content.size());
    std::string error;
    EXPECT_FALSE(sha256File(dir.path() / "missing.bin", hex, &error));
    EXPECT_NE(error.find("missing.bin"), std::string::npos);
}

// R28: publish verify hashes at >= 200 MB/s on one core (enforced with JASPER_TEST_PERF_SLACK,
// like the other wall-clock budgets of this binary)
TEST(Sha256, ThroughputBudget) {
    std::vector<uint8_t> data(64 * 1024 * 1024, 0x5a);
    Sha256 hash;
    const auto start = std::chrono::steady_clock::now();
    hash.update(data.data(), data.size());
    const std::string digest = hash.finishHex();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double mbPerSecond = (data.size() / (1024.0 * 1024.0)) / seconds;
    RecordProperty("sha256_mb_per_s", std::to_string(mbPerSecond));
    std::printf("SHA-256: %.0f MB/s (%s path)\n", mbPerSecond, Sha256::hardwareAccelerated() ? "SHA-NI" : "portable");
    EXPECT_EQ(digest.size(), 64u);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0) {
        EXPECT_GE(mbPerSecond * slack, 200.0);
    }
}
