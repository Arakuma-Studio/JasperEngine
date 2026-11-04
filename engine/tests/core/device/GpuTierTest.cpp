// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The GPU database and detection (spec S56 R1, R2, AC1): renderer strings of every platform map
// to the expected tier through the expected rule, unknown GPUs get the platform's heuristic, and
// the embedded database is the checked-in gpu_tiers.json.

#include "Engine.h"
#include "device/Device.h"
#include "device/GpuTierDatabase.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

using namespace jasperengine;

namespace {

    struct GpuTierCase {
        Platform platform;
        const char* renderer;
        int memoryMB;
        DeviceTier tier;
        const char* kind;
    };

    DeviceDetection detectGpuTier(Platform platform, const std::string& renderer, int memoryMB = 0, int cores = 8){
        DeviceSpecs specs;
        specs.platform = (int)platform;
        specs.gpuName = renderer;
        specs.memoryMB = memoryMB;
        specs.cores = cores;
        return Device::detectTier(specs, GpuTierDatabase::builtIn());
    }

}

TEST(GpuTier, Normalize){
    EXPECT_EQ(GpuTierDatabase::normalize("Adreno (TM) 650"), "adreno 650");
    EXPECT_EQ(GpuTierDatabase::normalize("Mesa Intel(R) UHD Graphics 620 (KBL GT2)"), "mesa intel uhd graphics 620 kbl gt2");
    EXPECT_EQ(GpuTierDatabase::normalize("  NVIDIA GeForce RTX 3060/PCIe/SSE2  "), "nvidia geforce rtx 3060 pcie sse2");
    EXPECT_EQ(GpuTierDatabase::normalize("Mali-G78"), "mali g78");
    EXPECT_EQ(GpuTierDatabase::normalize(""), "");
}

TEST(GpuTier, FamilyPatternsMatchWholeWords){
    EXPECT_TRUE(GpuTierDatabase::familyMatches("adreno 6[0-4]\\d", "adreno 640"));
    EXPECT_FALSE(GpuTierDatabase::familyMatches("adreno 6[0-4]\\d", "adreno 650"));
    // a word end after the pattern: 6401 is not a 6xx
    EXPECT_FALSE(GpuTierDatabase::familyMatches("adreno 6[0-4]\\d", "adreno 6401"));
    // from a word start: "xadreno" is not adreno
    EXPECT_FALSE(GpuTierDatabase::familyMatches("adreno 6\\d\\d", "xadreno 650"));
    EXPECT_TRUE(GpuTierDatabase::familyMatches("mali g7[7-9]", "arm mali g78 mp14"));
    EXPECT_TRUE(GpuTierDatabase::familyMatches("apple m\\d", "apple m4 max"));
    EXPECT_TRUE(GpuTierDatabase::familyMatches("a.c", "abc"));
}

// AC1: the table of the spec, each with the rule that decided it
TEST(GpuTier, RendererStringsMapToTiers){
    const GpuTierCase cases[] = {
        {Platform::Android, "Adreno (TM) 530", 4096, DeviceTier::Low, "name"},
        {Platform::Android, "Adreno (TM) 650", 8192, DeviceTier::Medium, "name"},
        {Platform::Android, "Adreno (TM) 740", 8192, DeviceTier::High, "name"},
        {Platform::Android, "Adreno (TM) 830", 12288, DeviceTier::High, "family"},
        {Platform::Android, "Mali-G52 MC2", 4096, DeviceTier::Low, "name"},
        {Platform::Android, "Mali-G78", 8192, DeviceTier::Medium, "name"},
        {Platform::Android, "PowerVR Rogue GE8320", 3072, DeviceTier::Low, "name"},
        {Platform::Android, "PowerVR B-Series BXM-8-256", 4096, DeviceTier::Low, "family"},
        {Platform::iOS, "Apple A12 GPU", 4096, DeviceTier::Medium, "name"},
        {Platform::iOS, "Apple A15 GPU", 6144, DeviceTier::High, "name"},
        {Platform::MacOS, "Apple M1", 16384, DeviceTier::High, "name"},
        {Platform::Linux, "Mesa Intel(R) UHD Graphics 620 (KBL GT2)", 8192, DeviceTier::Medium, "name"},
        {Platform::Windows, "NVIDIA GeForce RTX 3060", 16384, DeviceTier::High, "name"},
        {Platform::Linux, "NVIDIA GeForce RTX 3060/PCIe/SSE2", 16384, DeviceTier::High, "name"},
        {Platform::Web, "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 (0x00002487) Direct3D11 vs_5_0 ps_5_0, D3D11)", 8192, DeviceTier::High, "device"},
        {Platform::Web, "ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device (Subzero) (0x0000C0DE)), SwiftShader driver)", 8192, DeviceTier::Low, "family"},
        {Platform::Linux, "llvmpipe (LLVM 15.0.7, 256 bits)", 16384, DeviceTier::Low, "family"},
        {Platform::Windows, "Microsoft Basic Render Driver", 16384, DeviceTier::Low, "family"},
        {Platform::Windows, "NVIDIA GeForce RTX 5070", 32768, DeviceTier::High, "family"},
        {Platform::Windows, "Intel(R) Arc(TM) B580 Graphics", 16384, DeviceTier::High, "family"},
        {Platform::Windows, "Intel(R) Graphics 2025", 16384, DeviceTier::Medium, "vendor"},
    };
    for (const GpuTierCase& c : cases){
        const DeviceDetection detection = detectGpuTier(c.platform, c.renderer, c.memoryMB);
        EXPECT_EQ(detection.tier, c.tier) << c.renderer << ": " << detection.reason;
        EXPECT_EQ(detection.kind, c.kind) << c.renderer << ": " << detection.reason;
        EXPECT_NE(detection.reason.find(std::string("tier ") + DeviceNames::tierLabel(c.tier)), std::string::npos) << detection.reason;
    }
}

TEST(GpuTier, ReasonSaysGpuRuleAndTier){
    const DeviceDetection detection = detectGpuTier(Platform::Android, "Adreno (TM) 650", 8192);
    EXPECT_EQ(detection.reason, "Adreno (TM) 650, rule name (adreno 650), tier Medium");
    EXPECT_EQ(detection.vendor, "qualcomm");
}

// R2: an unknown GPU never fails, and its tier comes from the platform's memory and cores
TEST(GpuTier, UnknownGpusGetTheHeuristic){
    EXPECT_EQ(detectGpuTier(Platform::Android, "Some GPU 9000", 2048).tier, DeviceTier::Low);
    EXPECT_EQ(detectGpuTier(Platform::Android, "Some GPU 9000", 4096).tier, DeviceTier::Medium);
    EXPECT_EQ(detectGpuTier(Platform::Android, "Some GPU 9000", 8192).tier, DeviceTier::High);
    EXPECT_EQ(detectGpuTier(Platform::Android, "", 0).tier, DeviceTier::Medium);
    EXPECT_EQ(detectGpuTier(Platform::iOS, "Some GPU", 2048).tier, DeviceTier::Low);
    EXPECT_EQ(detectGpuTier(Platform::iOS, "Some GPU", 8192).tier, DeviceTier::High);
    EXPECT_EQ(detectGpuTier(Platform::Linux, "Some GPU", 8192, 8).tier, DeviceTier::High);
    EXPECT_EQ(detectGpuTier(Platform::Linux, "Some GPU", 8192, 2).tier, DeviceTier::Medium);
    // the web: Medium, Low under 4 GB of deviceMemory; Safari's masked name has no vendor default
    EXPECT_EQ(detectGpuTier(Platform::Web, "Apple GPU", 8192).tier, DeviceTier::Medium);
    EXPECT_EQ(detectGpuTier(Platform::Web, "Apple GPU", 2048).tier, DeviceTier::Low);
    EXPECT_EQ(detectGpuTier(Platform::Web, "WebKit WebGL", 0).tier, DeviceTier::Medium);
    const DeviceDetection unknown = detectGpuTier(Platform::Android, "Some GPU 9000", 4096, 8);
    EXPECT_EQ(unknown.kind, "heuristic");
    EXPECT_EQ(unknown.reason, "Some GPU 9000, unknown GPU, heuristic: Android with 4096 MB and 8 cores, tier Medium");
}

TEST(GpuTier, BadDatabaseIsRefused){
    GpuTierDatabase database;
    std::string error;
    EXPECT_FALSE(database.load("not json", error));
    EXPECT_FALSE(database.load("{\"version\": 2, \"rules\": []}", error));
    EXPECT_FALSE(database.load("{\"version\": 1, \"rules\": [{\"kind\": \"name\", \"name\": \"x\", \"tier\": \"huge\"}]}", error));
    EXPECT_NE(error.find("tier"), std::string::npos) << error;
    EXPECT_TRUE(database.load("{\"version\": 1, \"rules\": [{\"platform\": \"any\", \"kind\": \"name\", \"name\": \"acme 1\", \"tier\": \"ultra\"}]}", error)) << error;
    GpuQuery query;
    query.renderer = "ACME 1";
    query.platform = "pc";
    const GpuMatch match = database.match(query);
    EXPECT_TRUE(match.matched);
    EXPECT_EQ(match.tier, DeviceTier::Ultra);
}

// gpu_tiers.json is what the engine embeds (GpuTiersData.cpp, written by
// tools/device/convert_forge_gpu_data.py --embed)
TEST(GpuTier, EmbeddedDatabaseIsTheCheckedInFile){
    std::ifstream file(std::string(JASPER_TEST_REPO_DIR) + "/engine/core/device/gpu_tiers.json", std::ios::binary);
    ASSERT_TRUE(file.good());
    std::stringstream text;
    text << file.rdbuf();
    EXPECT_EQ(GpuTierDatabase::embeddedJson(), text.str()) << "run tools/device/convert_forge_gpu_data.py --embed";
    EXPECT_GT(GpuTierDatabase::builtIn().getRuleCount(), 500u);
}
