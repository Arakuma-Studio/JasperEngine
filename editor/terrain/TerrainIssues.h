// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T1 R72: one list of what is wrong with a terrain, read by sample_terrain's issues and by the banner
// at the top of the Terrain window, so neither relies on the console. Codes are §6 Table 2's.

#include "ecs/Entity.h"

#include <string>
#include <vector>

namespace jasperengine{
    struct TerrainComponent;
}

namespace jasperengine::editor{

    enum class TerrainIssueSeverity{
        Info,
        Warning,
        Error
    };

    // What a button can do about an issue
    enum class TerrainIssueFix{
        None,
        CreateTerrain,
        RetryLoad,
        RetryWrite,
        ConvertTo16Bit,
        RaiseTerrainHeight
    };

    struct TerrainIssue{
        std::string code;
        TerrainIssueSeverity severity = TerrainIssueSeverity::Info;
        std::string message;
        TerrainIssueFix fix = TerrainIssueFix::None;
        // the fix as the button and the agent read it ("" without one)
        std::string fixLabel;
    };

    class TerrainIssues{
    public:
        // At least this share of the height texels at 0 or at the top is clipping (Table 2)
        static constexpr double CLIPPED_SHARE = 0.001;

        // Every issue of the terrain as it is now. failedWrites: the map files the writer could not write.
        static std::vector<TerrainIssue> check(TerrainComponent& terrain, size_t failedWrites);

        static const char* severityName(TerrainIssueSeverity severity);
    };

}
