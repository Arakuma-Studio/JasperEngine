// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T2 R43-R45, AC35 at the export's decision, with no player built: a project whose Lua loads a Noise Graph
// ships its graphs and needs the noise module; one that only bakes ships none and stays on a lite variant.
// The prebuilt player's run of the same is tools/mcp/tests/test_noise_export.py (JASPER_SLOW_BUILD).

#include <gtest/gtest.h>

#include "Exporter.h"
#include "players/ModuleUsage.h"
#include "players/PlayerRuntimes.h"

#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    ModuleUsage usageOfLua(const std::string& code) {
        ModuleScan scan;
        scan.navigationInBuilds = false;
        scan.luaScripts.push_back({"lua/game.lua", code});
        return ModuleUsageDetector::analyze(scan);
    }

}

TEST(NoiseExport, ARuntimeGraphShips) {
    const ModuleUsage usage = usageOfLua("local g = NoiseGraph.load('graphs/Plains.noisegraph')\nprint(g:sample(0, 0))");
    EXPECT_TRUE(usage.needs("noise"));
    EXPECT_TRUE(Exporter::shipsForNoise("graphs/Plains.noisegraph", &usage));
    // every graph, not only the one named: a Lua string can be built at run time
    EXPECT_TRUE(Exporter::shipsForNoise("other/Unnamed.NOISEGRAPH", &usage));
    std::vector<std::string> modules = usage.list();
    modules.push_back("physics3d");
    EXPECT_EQ(PlayerRuntimes::variantFor(modules).name, "core");
}

TEST(NoiseExport, ABakedProjectShipsNoGraphAndStaysLite) {
    const ModuleUsage usage = usageOfLua("local noise = 0.5\nprint(noise)");
    EXPECT_FALSE(usage.needs("noise"));
    EXPECT_FALSE(Exporter::shipsForNoise("graphs/Plains.noisegraph", &usage));
    EXPECT_FALSE(Exporter::shipsForNoise("graphs/Loud.NoiseGraph", &usage));
    EXPECT_TRUE(Exporter::shipsForNoise("textures/rock.png", &usage)) << "only graphs are its to drop";
    std::vector<std::string> modules = usage.list();
    modules.push_back("physics3d");
    EXPECT_EQ(PlayerRuntimes::variantFor(modules).name, "lite3d");
}

TEST(NoiseExport, AnUnknownUsageKeepsEverything) {
    EXPECT_TRUE(Exporter::shipsForNoise("graphs/Plains.noisegraph", nullptr)) << "a source export: its user's code may load any";
}
