// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A folder data:// resolves to in place of System::getUserDataPath while it is set: a player's
// test run gives each case a sandbox (docs/specs/project-test-runner.md R25). Main thread.

#include "Export.h"

#include <string>

namespace jasperengine {

    // "" goes back to the game's user data folder
    JASPERENGINE_API void setUserDataOverride(const std::string& folder);
    JASPERENGINE_API const std::string& getUserDataOverride();

}
