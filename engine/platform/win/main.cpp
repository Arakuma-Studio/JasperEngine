// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JasperEngineWin.h"
#include "../common/PlatformDefaults.h"
#include "player/PlayerLaunch.h"

int main(int argc, char* argv[]) {
    // the build's window settings, then the launch options (and a player's game.json) over them
    jasperengine::applyPlatformDefaults();
    const int launched = jasperengine::player::launch(argc, argv);
    if (launched >= 0)
        return launched;
    return JasperEngineWin::init(argc, argv);
}
