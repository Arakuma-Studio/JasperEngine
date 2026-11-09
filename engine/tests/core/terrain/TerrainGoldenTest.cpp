// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Determinism (T1 R8, AC5): the op sequences' hashes hold on every platform and thread count.
// JASPER_TEST_UPDATE_GOLDEN=1 rewrites golden/op_hashes.json and the digest the wasm gate compares
// (tools/web/gates/terrain-edit.json); a changed hash there is a changed output, which needs a reason.

#include "TerrainGoldenSequences.h"

#include "io/AssetIOService.h"
#include "jobs/JobSystem.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::terraincore;
using namespace terraingolden;

namespace {

    std::filesystem::path testDir() {
        return std::filesystem::path(__FILE__).parent_path();
    }

    std::filesystem::path hashesPath() {
        return testDir() / "golden" / "op_hashes.json";
    }

    std::filesystem::path gatePath() {
        // engine/tests/core/terrain -> the repository's root
        return testDir().parent_path().parent_path().parent_path().parent_path() / "tools" / "web" / "gates" / "terrain-edit.json";
    }

    std::string hexOf(uint64_t value) {
        char text[32];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
        return text;
    }

    std::string readText(const std::filesystem::path& path) {
        std::ifstream file(path);
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }

    bool updating() {
        const char* update = std::getenv("JASPER_TEST_UPDATE_GOLDEN");
        return update && update[0] == '1';
    }

    std::string gateText(const std::string& digest) {
        return "{\n"
               "  \"description\": \"tools/test-projects/terrain-edit (WEB_PROJECT=terrain-edit tools/build.sh web): T1 AC5, TerrainGolden's op sequences on a fixed seed map, hashed in wasm. The digest below is pinned from the native core suite (TerrainGolden.Hashes checks it matches golden/op_hashes.json), so this gate fails when a kernel's bytes differ from x86-64. The canvas only says the engine ran that far.\",\n"
               "  \"rgb\": [\n    40,\n    200,\n    90\n  ],\n"
               "  \"tolerance\": 40,\n"
               "  \"isolated\": true,\n"
               "  \"console\": [\n    {\n      \"text\": \"[gate] terrain-edit " + digest + "\",\n      \"level\": \"log\"\n    }\n  ]\n"
               "}\n";
    }

}

TEST(TerrainGolden, ThereAreFortySequences) {
    EXPECT_EQ(sequences().size(), 40u);
}

TEST(TerrainGolden, Hashes) {
    const std::vector<Sequence> list = sequences();
    const std::vector<uint64_t> hashes = runAll();
    ASSERT_EQ(list.size(), hashes.size());

    if (updating()) {
        std::filesystem::create_directories(hashesPath().parent_path());
        std::ofstream out(hashesPath());
        out << "{\n";
        for (size_t i = 0; i < list.size(); i++) {
            out << "  \"" << list[i].name << "\": \"" << hexOf(hashes[i]) << "\"" << (i + 1 < list.size() ? "," : "") << "\n";
        }
        out << "}\n";
        std::ofstream gate(gatePath());
        gate << gateText(hexOf(digestOf(hashes)));
        return;
    }

    const std::string text = readText(hashesPath());
    ASSERT_FALSE(text.empty()) << hashesPath();
    std::map<std::string, std::string> pinned;
    const std::regex row("\"([a-z0-9_]+)\": \"([0-9a-f]{16})\"");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), row); it != std::sregex_iterator(); ++it) {
        pinned[(*it)[1].str()] = (*it)[2].str();
    }
    EXPECT_EQ(pinned.size(), list.size());
    for (size_t i = 0; i < list.size(); i++) {
        EXPECT_EQ(hexOf(hashes[i]), pinned[list[i].name]) << list[i].name;
    }

    // What the wasm gate compares is this same list's digest
    EXPECT_NE(readText(gatePath()).find("[gate] terrain-edit " + hexOf(digestOf(hashes))), std::string::npos) << gatePath();
}

TEST(TerrainGolden, EverySequenceDoesSomething) {
    RawMapStore start(TERRAIN_SIZE, MAX_HEIGHT);
    makeStore(start);
    fillSeedMaps(start);
    const uint64_t seed = stateHash(start);
    const std::vector<Sequence> list = sequences();
    const std::vector<uint64_t> hashes = runAll();
    for (size_t i = 0; i < list.size(); i++) {
        EXPECT_NE(hashes[i], seed) << list[i].name;
    }
}

TEST(TerrainGolden, ThreadCountDoesNotMatter) {
    // Big enough that a whole-map op goes through parallelFor. 0 workers is the web's mode: inline.
    const auto hashAt = [](int workers) {
        jobs::Config config;
        config.workers = workers;
        // (the asset readers take the process job system only at start(): restarted with it, or a
        // later test's reads go to the shut-down pool and end cancelled)
        AssetIOService::instance().stop();
        jobs::shutdown();
        jobs::initialize(config);
        AssetIOService::instance().start();

        constexpr int LARGE = 513;
        RawMapStore store(static_cast<float>(LARGE), MAX_HEIGHT);
        store.addMap(MapId{MapKind::Height, 0}, LARGE, LARGE, 1, 16);
        std::vector<float> heights(static_cast<size_t>(LARGE) * LARGE);
        Pcg32 random(99);
        for (float& value : heights) {
            value = random.nextFloat();
        }
        store.write(MapId{MapKind::Height, 0}, TexelRect{0, 0, LARGE, LARGE}, heights);

        Op smooth;
        smooth.type = OpType::Smooth;
        Op noise;
        noise.type = OpType::AddNoise;
        noise.amplitude = 1.0f;
        noise.seed = 3;
        Op filtered;
        filtered.type = OpType::Raise;
        filtered.delta = 1.0f;
        filtered.filter.slope = FilterRange{0.0f, 40.0f};
        filtered.filter.softness = 0.25f;
        const Op list[] = {smooth, noise, filtered};
        EXPECT_TRUE(applyOps(store, list).ok);
        return mapHash(store, MapId{MapKind::Height, 0});
    };
    const uint64_t one = hashAt(1);
    const uint64_t eight = hashAt(8);
    const uint64_t inlined = hashAt(0);
    AssetIOService::instance().stop();
    jobs::shutdown();
    jobs::initialize(jobs::Config{});
    AssetIOService::instance().start();
    EXPECT_EQ(one, eight);
    EXPECT_EQ(one, inlined);
}

TEST(TerrainGolden, DeterministicArithmetic) {
    // The polynomial stays close to the true value (it is what the kernels use in its place)
    for (float degrees = -720.0f; degrees <= 720.0f; degrees += 1.5f) {
        const float radians = degrees * (3.14159265358979f / 180.0f);
        EXPECT_NEAR(detSin(radians), std::sin(radians), 2e-6f) << degrees;
        EXPECT_NEAR(detCos(radians), std::cos(radians), 2e-6f) << degrees;
    }
    // PCG32's stated output, seed 42 stream 54, from the generator's own demo
    Pcg32 random(42u, 54u);
    EXPECT_EQ(random.next(), 0xa15c02b7u);
    EXPECT_EQ(random.next(), 0x7b47f409u);
    EXPECT_EQ(random.next(), 0xba1d3330u);
}
