// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// GPU quirks (docs/specs/release-performance-and-gpu-quirks.md P2, AC15-AC18): the rules match
// exactly the devices S32's checks did, ranges compare numbers, overrides win over rules, the
// deprecated variables still work, and the embedded table is the checked-in, valid one.

#include "device/GpuQuirks.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace GpuQuirkTest_cpp {

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    void setQuirkEnv(const char* name, const char* value){
    #if defined(_WIN32)
        _putenv_s(name, value ? value : "");
    #else
        if (value)
            setenv(name, value, 1);
        else
            unsetenv(name);
    #endif
    }

    std::set<std::string> activeOn(const std::string& renderer, const std::string& api, const std::string& driver = "",
                                   const std::vector<GpuQuirkOverride>& overrides = {}){
        GpuQuirkDevice device;
        device.renderer = renderer;
        device.api = api;
        device.driver = driver;
        device.platform = api == "gles3" ? "android" : "pc";
        std::set<std::string> names;
        const std::vector<GpuQuirkState> states = GpuQuirks::resolve(device, GpuQuirks::builtInRules(), overrides);
        for (size_t i = 0; i < states.size(); i++)
            if (states[i].active) names.insert(GpuQuirks::name((GpuQuirk)i));
        return names;
    }

    std::string readRepoFile(const std::string& path){
        std::ifstream file(std::string(JASPER_TEST_REPO_DIR) + "/" + path, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }

    // a check names a gtest ("Suite.Test", registered in this binary or declared in the unit-test
    // sources: a backend's build leaves the other backends' tests out) or an MCP test
    // ("test_x.py::test_name", a function of that file)
    bool checkExists(const std::string& check){
        const size_t sep = check.find("::");
        if (sep != std::string::npos){
            const std::string file = readRepoFile("tools/mcp/tests/" + check.substr(0, sep));
            const std::string function = check.substr(sep + 2);
            return !file.empty() && (file.find("def " + function + "(") != std::string::npos);
        }
        const size_t dot = check.find('.');
        if (dot == std::string::npos)
            return false;
        const std::string suite = check.substr(0, dot);
        const std::string name = check.substr(dot + 1);
        const ::testing::UnitTest& unit = *::testing::UnitTest::GetInstance();
        for (int s = 0; s < unit.total_test_suite_count(); s++){
            const ::testing::TestSuite& testSuite = *unit.GetTestSuite(s);
            if (suite != testSuite.name())
                continue;
            for (int t = 0; t < testSuite.total_test_count(); t++)
                if (name == testSuite.GetTestInfo(t)->name()) return true;
        }
        std::error_code ec;
        for (const char* root : {"engine/tests/core", "editor/tests"}){
            for (const auto& entry : std::filesystem::recursive_directory_iterator(std::string(JASPER_TEST_REPO_DIR) + "/" + root, ec)){
                if (entry.path().extension() != ".cpp")
                    continue;
                std::ifstream file(entry.path(), std::ios::binary);
                std::stringstream text;
                text << file.rdbuf();
                const std::string source = text.str();
                if (source.find("(" + suite + ", " + name + ")") != std::string::npos)
                    return true;
            }
        }
        return false;
    }

    // R13, R14, R27: what every rule must carry; the problems found
    std::vector<std::string> ruleProblems(const std::vector<GpuQuirks::Rule>& rules){
        std::vector<std::string> problems;
        for (const GpuQuirks::Rule& rule : rules){
            if (rule.quirk >= GpuQuirk::Count)
                problems.push_back("unknown quirk \"" + rule.quirkName + "\"");
            if (rule.reason.size() < 20)
                problems.push_back(rule.quirkName + ": a reason under 20 characters");
            if (rule.issue.empty())
                problems.push_back(rule.quirkName + ": no issue");
            if (rule.checks.empty())
                problems.push_back(rule.quirkName + ": no check");
            for (const std::string& check : rule.checks)
                if (!checkExists(check)) problems.push_back(rule.quirkName + ": check " + check + " names no test");
        }
        return problems;
    }

}

// AC15: the software-GL rule matches exactly sokol's four renderers, on GL only
TEST(GpuQuirks, SoftwareGlRuleMatchesExactlySokolsRenderers){
    const std::set<std::string> software = {"gl_no_worker_context", "gl_no_compile_threads"};
    for (const char* renderer : {"llvmpipe (LLVM 15.0.7, 256 bits)", "softpipe", "Google SwiftShader", "Apple Software Rasterizer"}){
        SCOPED_TRACE(renderer);
        EXPECT_EQ(activeOn(renderer, "glcore"), software);
    }
    EXPECT_TRUE(activeOn("llvmpipe (LLVM 15.0.7, 256 bits)", "vulkan").empty());
    EXPECT_TRUE(activeOn("NVIDIA GeForce RTX 3080 Ti/PCIe/SSE2", "glcore").empty());
    EXPECT_EQ(activeOn("Adreno (TM) 650", "gles3"), std::set<std::string>{"gl_no_warm_draws"});
    // GLES on a software GL: all three (S32's three checks together)
    EXPECT_EQ(activeOn("llvmpipe (LLVM 15.0.7, 256 bits)", "gles3").size(), 3u);
}

// AC16: driver and OS ranges compare numbers, component-wise
TEST(GpuQuirks, DriverAndOsRangesCompareNumbers){
    std::vector<GpuQuirks::Rule> rules;
    std::string error;
    ASSERT_TRUE(GpuQuirks::parseRules(R"({"version": 1, "rules": [
        {"quirk": "gl_no_warm_draws", "reason": "a test rule of driver ranges", "issue": "test", "check": "test",
         "match": {"api": "gles3", "driver_below": "502.0"}},
        {"quirk": "gl_no_compile_threads", "reason": "a test rule of OS ranges", "issue": "test", "check": "test",
         "match": {"os_at_least": "29"}}]})", rules, error)) << error;
    GpuQuirkDevice device;
    device.api = "gles3";
    device.driver = "OpenGL ES 3.2 V@415.0 (GIT@4b4d5a6, I3b3a0b3d6c) (Date:09/30/19)";
    EXPECT_TRUE(GpuQuirks::ruleMatches(rules[0], device));
    device.driver = "V@0615.0";
    EXPECT_FALSE(GpuQuirks::ruleMatches(rules[0], device));
    device.driver = "OpenGL ES 3.2 V@502.0";
    EXPECT_FALSE(GpuQuirks::ruleMatches(rules[0], device)) << "below is exclusive";
    device.driver = "no number here";
    EXPECT_FALSE(GpuQuirks::ruleMatches(rules[0], device)) << "a string without a dotted number never matches a range";
    // os_at_least reads the Android API level there
    device.osVersion = "29";
    EXPECT_TRUE(GpuQuirks::ruleMatches(rules[1], device));
    device.osVersion = "28";
    EXPECT_FALSE(GpuQuirks::ruleMatches(rules[1], device));
    device.osVersion = "";
    EXPECT_FALSE(GpuQuirks::ruleMatches(rules[1], device));
    EXPECT_TRUE(GpuQuirks::versionLess("1.9.9", "1.10"));
    EXPECT_FALSE(GpuQuirks::versionLess("2.0", "2"));
    EXPECT_EQ(GpuQuirks::dottedNumber("Mesa 23.2.1-1ubuntu"), "23.2.1");
}

// AC17: every rule of the embedded table names a known quirk, says why, cites its issue and names
// a test that exists; the embedded table is the checked-in file. A planted rule without a check fails
TEST(GpuQuirks, EmbeddedTableIsValid){
    EXPECT_EQ(GpuQuirks::embeddedJson(), readRepoFile("engine/core/device/gpu_quirks.json"))
        << "run tools/device/convert_forge_gpu_data.py --embed";
    const std::vector<GpuQuirks::Rule>& rules = GpuQuirks::builtInRules();
    ASSERT_EQ(rules.size(), 3u) << "the three S32 decisions (D8)";
    const std::vector<std::string> problems = ruleProblems(rules);
    for (const std::string& problem : problems)
        ADD_FAILURE() << problem;
    // the red control of the check itself
    std::vector<GpuQuirks::Rule> planted = rules;
    planted.back().checks.clear();
    EXPECT_EQ(ruleProblems(planted).size(), 1u);
    planted = rules;
    planted.back().checks = {"AsyncCreateGpu.NoSuchTest"};
    EXPECT_EQ(ruleProblems(planted).size(), 1u);
    planted = rules;
    planted.back().checks = {"test_async_shaders.py::test_no_such_test"};
    EXPECT_EQ(ruleProblems(planted).size(), 1u);
    std::vector<GpuQuirks::Rule> unknown;
    std::string error;
    ASSERT_TRUE(GpuQuirks::parseRules(R"({"version": 1, "rules": [{"quirk": "gl_invented", "reason": "a quirk data cannot invent",
        "issue": "test", "check": "GpuQuirks.EmbeddedTableIsValid", "match": {"api": "glcore"}}]})", unknown, error)) << error;
    EXPECT_EQ(ruleProblems(unknown).size(), 1u) << "data cannot invent a quirk";
    EXPECT_FALSE(GpuQuirks::parseRules(R"({"version": 1, "rules": [{"quirk": "gl_no_warm_draws", "match": {"gpu": "x"}}]})", unknown, error));
    EXPECT_NE(error.find("unknown match key"), std::string::npos) << error;
}

// AC18: forced overrides go over the rules, the later one wins, and parsing names the known quirks
TEST(GpuQuirks, ForcedOverridesWinOverRules){
    std::vector<GpuQuirkOverride> overrides;
    std::string error;
    ASSERT_TRUE(GpuQuirks::parseOverrides("gl_no_worker_context:off, gl_no_warm_draws:on", overrides, error)) << error;
    ASSERT_EQ(overrides.size(), 2u);
    EXPECT_EQ(activeOn("llvmpipe (LLVM 15.0.7, 256 bits)", "glcore", "", overrides),
              (std::set<std::string>{"gl_no_compile_threads", "gl_no_warm_draws"}));
    GpuQuirkDevice device;
    device.renderer = "NVIDIA GeForce RTX 3080 Ti/PCIe/SSE2";
    device.api = "glcore";
    const std::vector<GpuQuirkState> states = GpuQuirks::resolve(device, GpuQuirks::builtInRules(), overrides);
    EXPECT_EQ(states[(size_t)GpuQuirk::GlNoWarmDraws].source, "forced_on");
    EXPECT_FALSE(states[(size_t)GpuQuirk::GlNoWarmDraws].reason.empty()) << "the rule's reason says why the lever exists";
    EXPECT_EQ(states[(size_t)GpuQuirk::GlNoWorkerContext].source, "forced_off");
    // the later override wins
    overrides.push_back({GpuQuirk::GlNoWarmDraws, false});
    EXPECT_FALSE(GpuQuirks::resolve(device, GpuQuirks::builtInRules(), overrides)[(size_t)GpuQuirk::GlNoWarmDraws].active);
    std::vector<GpuQuirkOverride> bad;
    EXPECT_FALSE(GpuQuirks::parseOverrides("x:on", bad, error));
    EXPECT_EQ(error, "unknown quirk \"x\" (known: gl_no_worker_context, gl_no_compile_threads, gl_no_warm_draws, test_tint)");
    EXPECT_FALSE(GpuQuirks::parseOverrides("gl_no_warm_draws", bad, error));
    EXPECT_NE(error.find(":on or :off"), std::string::npos) << error;
}

// AC18: the S32 variables still work, as aliases of an override with a deprecation line; a bad
// name in JASPERENGINE_GPU_QUIRKS is a warning naming the known quirks
TEST(GpuQuirks, LegacyEnvironmentAliases){
    LogCapture log;
    setQuirkEnv("JASPER_GL_SOFTWARE_WORKER", "1");
    setQuirkEnv("JASPER_GL_WARM_DRAWS", "1");
    setQuirkEnv("JASPERENGINE_GPU_QUIRKS", "nope:on,gl_no_compile_threads:off");
    const std::vector<GpuQuirkOverride> overrides = GpuQuirks::environmentOverrides();
    setQuirkEnv("JASPER_GL_SOFTWARE_WORKER", nullptr);
    setQuirkEnv("JASPER_GL_WARM_DRAWS", nullptr);
    setQuirkEnv("JASPERENGINE_GPU_QUIRKS", nullptr);
    ASSERT_EQ(overrides.size(), 3u);
    EXPECT_EQ(overrides[0].quirk, GpuQuirk::GlNoWorkerContext);
    EXPECT_FALSE(overrides[0].on);
    EXPECT_EQ(overrides[1].quirk, GpuQuirk::GlNoWarmDraws);
    EXPECT_FALSE(overrides[1].on);
    EXPECT_EQ(overrides[2].quirk, GpuQuirk::GlNoCompileThreads);
    EXPECT_EQ(log.count(D_LOG_WARN, "deprecated"), 2u);
    EXPECT_EQ(log.count(D_LOG_WARN, "unknown quirk \"nope\" (known: gl_no_worker_context"), 1u);
}

}  // namespace GpuQuirkTest_cpp
