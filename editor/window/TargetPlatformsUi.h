// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Target Platforms (Project Settings > Player > Publishing Settings and the Build Settings window's
// Target column): the platforms' names and icons as Build Settings draws them, and the one undoable
// edit both widgets make.

#include "Project.h"
#include "ShippingTypes.h"
#include "command/CommandHistory.h"
#include "command/type/ProjectSettingsValuesCmd.h"
#include "external/IconsFontAwesome6.h"

#include <cstdint>

namespace jasperengine::editor {

    inline const char* targetPlatformTitle(BuildPlatform platform) {
        switch (platform) {
            case BuildPlatform::Windows: return "Windows";
            case BuildPlatform::Linux: return "Linux";
            case BuildPlatform::MacOS: return "macOS";
            case BuildPlatform::Android: return "Android";
            case BuildPlatform::IOS: return "iOS";
            case BuildPlatform::Web: return "Web";
        }
        return "";
    }

    inline const char* targetPlatformIcon(BuildPlatform platform) {
        switch (platform) {
            case BuildPlatform::Windows: return ICON_FA_WINDOW_MAXIMIZE;
            case BuildPlatform::Linux: return ICON_FA_TERMINAL;
            case BuildPlatform::MacOS: return ICON_FA_LAPTOP;
            case BuildPlatform::Android: return ICON_FA_MOBILE_SCREEN_BUTTON;
            case BuildPlatform::IOS: return ICON_FA_MOBILE_BUTTON;
            case BuildPlatform::Web: return ICON_FA_GLOBE;
        }
        return "";
    }

    // one step of the project history ("Set Target Platforms"), saved at once; an empty set is refused
    inline void setTargetPlatformsUndoable(Project* project, uint8_t targets) {
        if (!project || targets == 0 || targets == project->getTargetPlatforms()) return;
        mcp::Json names = mcp::Json::array();
        for (BuildPlatform platform : {BuildPlatform::Windows, BuildPlatform::Linux, BuildPlatform::MacOS, BuildPlatform::Android,
                                       BuildPlatform::IOS, BuildPlatform::Web}) {
            if (targets & Project::targetPlatformBit(platform)) names.push_back(buildPlatformName(platform));
        }
        auto* command = new ProjectSettingsValuesCmd(project, {{"player.target_platforms", names}}, "Set Target Platforms");
        command->setNoMerge();
        project->getProjectCommandHistory()->addCommand(command);
    }

}
